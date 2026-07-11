/* pthr.c -- bionic<->newlib pthread wrappers for libTTapp.so
 *
 * Ported from gm666q/lswtcs-vita (reimpl/pthr.c), adapted for devkitA64/libnx.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <switch.h>

#include "pthr.h"
#include "util.h"
#include "so_util.h"

// set in the trampoline when the game's renderThread_main starts; the GL
// ownership layer lets this thread keep the real context across parks, so its
// wait shims below must service handover in short slices instead of blocking
static __thread int tls_is_render_thread = 0;

int pthr_is_render_thread(void) {
  return tls_is_render_thread;
}

#define BIONIC_PTHREAD_MUTEX_INITIALIZER            0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000

// ---------------------------------------------------------------------------
// fake RW TLS: AArch64 -fstack-protector loads the canary relative to
// TPIDR_EL0, which libnx leaves pointing at read-only/zero storage. Each game
// thread gets a small zeroed block installed in TPIDR_EL0 so those loads (and
// any other bionic TLS-relative reads) land on valid, writable memory. The
// block is intentionally leaked: it must stay live for the thread's lifetime.
// ---------------------------------------------------------------------------

void pthr_install_fake_tls(void) {
  uint8_t *tls = calloc(1, 0x200);
  armSetTlsRw(tls);
}

void pthr_ensure_fake_tls(void) {
  if (armGetTlsRw() == NULL)
    pthr_install_fake_tls();
}

// ---------------------------------------------------------------------------
// lazy first-use initialization of game-owned sync objects: the hot path is
// one acquire-load of the magic word; only init and destroy take init_lock
// ---------------------------------------------------------------------------

#define PTHR_MUTEX_MAGIC 0x4D58544Du // "MTXM"
#define PTHR_COND_MAGIC  0x444E434Du // "CNDM"

static Mutex init_lock;

static int attr_static_init(pthread_attr_t_bionic *attr) {
  if (attr->magic != 0x42424242) {
    attr->magic = 0x42424242;
    attr->real_ptr = malloc(sizeof(pthread_attr_t));
    return pthread_attr_init(attr->real_ptr);
  }
  return 0;
}

static int mutex_static_init(pthread_mutex_t_bionic *mutex, const pthread_mutexattr_t *attr) {
  // pairs with the release store below so real_ptr is visible once magic is
  if (__atomic_load_n(&mutex->magic, __ATOMIC_ACQUIRE) == PTHR_MUTEX_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) == PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock); // another thread won the first-use race
    return 0;
  }

  int kind = PTHREAD_MUTEX_NORMAL;
  if (attr) {
    pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
  } else {
    // the kind word of a statically initialized bionic mutex (overlaps the
    // low half of real_ptr, which we haven't written yet)
    switch (*(int *)mutex) {
      case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:  kind = PTHREAD_MUTEX_RECURSIVE;  break;
      case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: kind = PTHREAD_MUTEX_ERRORCHECK; break;
      default:                                          kind = PTHREAD_MUTEX_NORMAL;     break;
    }
  }

  pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));

  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, kind);
  int ret = pthread_mutex_init(real, &ma);
  pthread_mutexattr_destroy(&ma);

  if (ret == 0) {
    mutex->real_ptr = real;
    __atomic_store_n(&mutex->magic, PTHR_MUTEX_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
    debugPrintf("pthr: mutex init for %p failed (%d)\n", (void *)mutex, ret);
  }
  mutexUnlock(&init_lock);
  return ret;
}

static int cond_static_init(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (__atomic_load_n(&cond->magic, __ATOMIC_ACQUIRE) == PTHR_COND_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) == PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }

  pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
  int ret = pthread_cond_init(real, attr);

  if (ret == 0) {
    cond->real_ptr = real;
    __atomic_store_n(&cond->magic, PTHR_COND_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
    debugPrintf("pthr: cond init for %p failed (%d)\n", (void *)cond, ret);
  }
  mutexUnlock(&init_lock);
  return ret;
}

// ---------------------------------------------------------------------------
// core affinity for NetherSX2's thread model.
//
// PS2 emulation parallelizes into a few HEAVY threads -- the EE/VM thread
// (main.c's emu_thread, ~99% busy), the MTGS thread (GS command processing),
// and, when MTVU is enabled, the VU1 thread -- plus LIGHT background threads
// (audio playback, GS-ring / texture workers). libnx starts every thread on the
// same default core, so without explicit affinity the whole emulator
// time-shares ONE cpu while the others sit idle -- the "running on one core"
// symptom. We spread the work:
//
//   core 0             : EE/VM thread, kept to itself (the hot path)
//   cores 1 .. N-2     : MTGS + VU1 + emucore workers, round-robined
//   core N-1 (if N>=4) : audio + other background threads
//
// Each heavy thread gets a DISTINCT preferred core but a WIDE affinity mask
// (all hot cores), so Horizon spreads them by default yet can still migrate one
// off a momentarily-contended core. On a 4-core grant the top core (core 3, the
// Switch system core) is reserved for light background work so heavy emulation
// never fights the OS audio/HID/fs servers.
//
// (The Vita-port role system this replaced keyed off that engine's thread names
// -- AndroidMain/renderThread/NuThread -- which never match PS2's EE/GS/VU, so
// every emucore thread fell through to a single background core.)
// ---------------------------------------------------------------------------

static Mutex core_lock;
static int      core_count = 0;
static int      ee_core    = 0;     // dedicated to the EE/VM thread (hard-pinned)
static unsigned hot_mask   = 0x1;   // all cores used for heavy emulation
static unsigned work_mask  = 0x1;   // hot_mask minus the EE core: MTGS/VU/workers
static int      bg_core    = -1;    // dedicated background core, or -1 = share work pool
static int      work_list[4];       // hot cores excluding ee_core (MTGS/VU/workers)
static int      work_count = 0;
static unsigned work_rr = 0, bg_rr = 0;

static void core_init_once(void) {
  if (core_count)
    return;
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
    mask = 0x7; // fallback: cores 0,1,2 (typical app allotment)
  int core_list[4];
  for (int c = 0; c < 4; c++)
    if (mask & (1u << c))
      core_list[core_count++] = c;

  // Reserve the TOP core for background only when 4+ are granted; with <=3 we
  // need them all for the heavy emulation threads.
  int hot_count;
  if (core_count >= 4) {
    hot_count = core_count - 1;
    bg_core   = core_list[core_count - 1];
  } else {
    hot_count = core_count;
    bg_core   = -1;
  }
  hot_mask = 0;
  for (int i = 0; i < hot_count; i++)
    hot_mask |= (1u << core_list[i]);

  ee_core = core_list[0];
  // MTGS/VU/worker pool = the hot cores EXCEPT the EE's, so heavy workers never
  // share -- nor migrate onto -- the EE core. If only one hot core exists (a 1-2
  // core grant) everything unavoidably shares it.
  work_count = 0;
  for (int i = 1; i < hot_count; i++)
    work_list[work_count++] = core_list[i];
  if (work_count == 0)
    work_list[work_count++] = ee_core;
  work_mask = (hot_count >= 2) ? (hot_mask & ~(1u << ee_core)) : hot_mask;

  debugPrintf("pthr: %d cores (mask 0x%x): EE=core %d, work=%d core(s) mask 0x%x, bg=core %d\n",
              core_count, (unsigned)mask, ee_core, work_count, work_mask, bg_core);
}

// EE/VM thread: hard-pinned to its own core (exclusive mask) -- it is the
// hottest thread and benefits most from a stable core / warm cache.
void pthr_pin_ee_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = ee_core; const unsigned m = 1u << ee_core;
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
  debugPrintf("pthr: EE/VM thread pinned to core %d (mask 0x%x)\n", core, m);
}

// heavy emucore threads (MTGS, VU1, ring/texture workers): distinct preferred
// core round-robined over the work pool; work_mask lets a light worker migrate
// off a heavy peer's core but never onto the EE core.
static void assign_work_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = work_list[work_rr++ % (unsigned)work_count]; const unsigned m = work_mask;
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
}

// background threads (audio, etc.): the dedicated bg core if we have one, else
// share the work pool. For threads our vendored code spawns directly through
// newlib pthread_create (the audio mixer/callback) -- they never pass through
// the trampoline, so without this they'd default onto the EE core.
void pthr_pin_bg_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  int core; unsigned m;
  if (bg_core >= 0) { core = bg_core; m = 1u << bg_core; }
  else { core = work_list[bg_rr++ % (unsigned)work_count]; m = work_mask; }
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
}

// ---------------------------------------------------------------------------
// thread creation (installs the fake TLS before running game code)
// ---------------------------------------------------------------------------

typedef struct {
  void *(*start)(void *);
  void *arg;
} ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  pthr_install_fake_tls();
  // every emucore-spawned thread is a heavy worker (MTGS, VU1, ring/texture);
  // spread them across the work pool. The EE/VM thread and the audio threads are
  // pinned separately -- they don't come through here.
  assign_work_core();
  {
    extern so_module emu_mod;
    debugPrintf("pthr: thread entering start fn=%p (emucore+0x%lx) arg=%p\n",
                (void *)s.start,
                (unsigned long)((uintptr_t)s.start - (uintptr_t)emu_mod.load_virtbase),
                s.arg);
  }
  void *ret = s.start(s.arg);
  debugPrintf("pthr: thread fn=%p returned\n", (void *)s.start);
  // unconditional: a thread exiting while owning the GL context would orphan
  // it forever (EGL bindings are per-thread; nobody else can release it)
  extern void egl_gl_ownership_release(void);
  egl_gl_ownership_release();
  return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr,
                            void *(*start)(void *), void *param) {
  ThreadStart *s = malloc(sizeof(*s));
  s->start = start;
  s->arg = param;

  pthread_attr_t a;
  pthread_attr_init(&a);
  // the engine's worker/render threads recurse deeply; give them headroom
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  if (attr) {
    attr_static_init((pthread_attr_t_bionic *)attr);
    size_t want = 0;
    if (attr->real_ptr && pthread_attr_getstacksize(attr->real_ptr, &want) == 0 &&
        want > 2 * 1024 * 1024)
      pthread_attr_setstacksize(&a, want);
  }

  int ret = pthread_create(thread, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
  if (ret != 0)
    free(s);
  return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr) { return pthread_join(thread, value_ptr); }
int pthread_detach_soloader(pthread_t thread) { return pthread_detach(thread); }
pthread_t pthread_self_soloader(void) { return pthread_self(); }

int pthread_equal_soloader(pthread_t t1, pthread_t t2) {
  if (t1 == t2) return 1;
  if (!t1 || !t2) return 0;
  return pthread_equal(t1, t2);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param) {
  // newlib on devkitA64 doesn't expose pthread_getschedparam; the game only
  // reads these to echo them back, so reporting a default schedule is fine
  (void)thread;
  if (policy) *policy = 0; // SCHED_OTHER
  if (param) param->sched_priority = 0;
  return 0;
}

enum {
  PTHREAD_ONCE_SOLOADER_INIT = 0,
  PTHREAD_ONCE_SOLOADER_RUNNING = 1,
  PTHREAD_ONCE_SOLOADER_DONE = 2,
};

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return EINVAL;

  if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == PTHREAD_ONCE_SOLOADER_DONE)
    return 0;

  pthread_mutex_lock(&once_lock);
  for (;;) {
    int state = __atomic_load_n(once_control, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_ONCE_SOLOADER_DONE) {
      pthread_mutex_unlock(&once_lock);
      return 0;
    }

    if (state == PTHREAD_ONCE_SOLOADER_INIT) {
      __atomic_store_n(once_control, PTHREAD_ONCE_SOLOADER_RUNNING, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&once_lock);

      (*init_routine)();

      pthread_mutex_lock(&once_lock);
      __atomic_store_n(once_control, PTHREAD_ONCE_SOLOADER_DONE, __ATOMIC_RELEASE);
      pthread_cond_broadcast(&once_cond);
      pthread_mutex_unlock(&once_lock);
      return 0;
    }

    // A peer is still inside init_routine. POSIX requires callers to wait
    // until the initializer has completed; returning early exposes partially
    // initialized game singletons to other threads.
    pthread_mutex_unlock(&once_lock);
    extern void egl_gl_ownership_park(void);
    egl_gl_ownership_park();
    pthread_mutex_lock(&once_lock);
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == PTHREAD_ONCE_SOLOADER_RUNNING)
      pthread_cond_wait(&once_cond, &once_lock);
  }
}

// ---------------------------------------------------------------------------
// mutex / cond / attr
// ---------------------------------------------------------------------------

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_init(attr); }
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) { return pthread_mutexattr_settype(attr, type); }
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_destroy(attr); }

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr) {
  if (!uid) return EINVAL;
  return mutex_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) != PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&mutex->magic, 0, __ATOMIC_RELEASE);
  pthread_mutex_t *real = mutex->real_ptr;
  mutex->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_mutex_destroy(real);
  free(real);
  return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  // fast path: uncontended
  if (pthread_mutex_trylock(mutex->real_ptr) == 0)
    return 0;
  // contended: never block on a game mutex while holding the real GL context
  // (ABBA against the game's BeginCriticalSectionGL) -- park first
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_mutex_lock(mutex->real_ptr);
  // render thread still holding (park keeps it): the mutex holder might want
  // exactly that context, so poll with handover service instead of blocking
  extern void egl_gl_service_handover(void);
  for (;;) {
    if (pthread_mutex_trylock(mutex->real_ptr) == 0)
      return 0;
    egl_gl_service_handover();
    struct timespec ts = { 0, 100 * 1000 }; // 0.1 ms
    nanosleep(&ts, NULL);
  }
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex || !mutex->real_ptr) return EINVAL;
  return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) != PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&cond->magic, 0, __ATOMIC_RELEASE);
  pthread_cond_t *real = cond->real_ptr;
  cond->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_cond_destroy(real);
  free(real);
  return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_broadcast(cond->real_ptr);
}

// render-thread wait slice: while it parks holding the GL context, its waits
// poll for handover requests at this granularity
#define RENDER_WAIT_SLICE_NS (4 * 1000 * 1000)

static void timespec_add_ns(struct timespec *ts, long ns) {
  ts->tv_nsec += ns;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}

static int timespec_before(const struct timespec *a, const struct timespec *b) {
  if (a->tv_sec != b->tv_sec)
    return a->tv_sec < b->tv_sec;
  return a->tv_nsec < b->tv_nsec;
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  // about to park: hand back the GL context so other threads can render
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
  // render thread still holding the GL binding: wait ONE short slice and
  // report a timeout as a spurious wakeup (legal; predicate loops re-check).
  // Never loop internally: the game may signal without holding the mutex,
  // and a signal landing between laps is lost forever -- the one-shot
  // level-ready signal then leaves the renderer asleep for good.
  extern void egl_gl_service_handover(void);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, &ts);
  if (r == ETIMEDOUT) {
    egl_gl_service_handover();
    return 0; // spurious wakeup (POSIX/bionic allow them)
  }
  return r;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !abstime || !egl_gl_thread_holds_context())
    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
  // same single-slice spurious-wakeup scheme as pthread_cond_wait_soloader;
  // the caller's real deadline is only ever reported once it actually passes
  extern void egl_gl_service_handover(void);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  const int final_slice = !timespec_before(&ts, abstime);
  int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr,
                                 final_slice ? abstime : &ts);
  if (r == ETIMEDOUT && !final_slice) {
    egl_gl_service_handover();
    return 0; // spurious wakeup before the caller's deadline
  }
  return r;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr) {
  if (!attr) return EINVAL;
  return attr_static_init(attr);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}
