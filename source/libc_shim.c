/* libc_shim.c -- bionic-compatible libc wrappers for the 2.1.131 libs
 *
 * libGame.so and libc++_shared.so are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches
 * is passed straight through from imports.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}

long __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
  (void)dstlen; (void)srclen;
  return strncpy(dst, src, n);
}

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  value[0] = '\0';
  return 0;
}

unsigned long getauxval_fake(unsigned long type) {
  (void)type;
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID:
      return gettid_fake();
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

int sched_get_priority_max_fake(int policy) {
  (void)policy;
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1;
}

int __register_atfork_fake(void) {
  return 0;
}

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3;
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:
      debugPrintf("libc: sysconf(%d) -> -1\n", name);
      return -1;
  }
}

// High-resolution clock for all clock ids. The game's frame timer
// (NuTimeGetTicks) calls clock_gettime(CLOCK_REALTIME) and busy-waits in
// NuFrameEnd until the microsecond delta reaches the frame budget -- so the
// clock MUST advance at real-time rate with sub-millisecond resolution.
// newlib's CLOCK_REALTIME on the Switch is RTC-backed and effectively static
// here, which froze that loop forever (the post-audio 100%-CPU hang). We back
// every clock id with the 19.2 MHz system tick, like the Vita port uses
// sceKernelGetProcessTimeWide. A fixed epoch base keeps tv_sec plausible for
// any absolute-time consumer; it cancels out of the deltas the game computes.
#define FAKE_EPOCH_BASE 1700000000ull // ~2023-11, seconds

int clock_gettime_fake(int clk_id, struct timespec *tp) {
  (void)clk_id;
  if (!tp)
    return -1;
  // the NuFrameEnd busy-wait spins here without GL or blocking; make the
  // spin a hand-over service point or it can hold the context forever
  extern void egl_gl_service_handover(void);
  egl_gl_service_handover();
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq(); // 19200000 on the Switch
  const u64 tick = armGetSystemTick();
  tp->tv_sec = (time_t)(FAKE_EPOCH_BASE + tick / freq);
  tp->tv_nsec = (long)(((tick % freq) * 1000000000ull) / freq);
  return 0;
}

// ---------------------------------------------------------------------------
// path remapping
//
// The startup natives hand the game Android-absolute paths (INTERNAL_PATH and
// the four asset-pack .dat paths). The game concatenates and opens these, so
// every filesystem entry point rewrites that prefix onto the game directory
// (the process cwd, which the launcher sets to the .nro's folder). Mirrors
// lswtcs-vita's fix_path().
// ---------------------------------------------------------------------------

const char *fix_path(const char *path) {
  static _Thread_local char buf[2][1024];
  static _Thread_local int which = 0;

  if (!path)
    return path;
  debugPrintf("fs: %s\n", path); // every fopen/open/stat/mkdir funnels here

  // NetherSX2 is driven entirely with absolute SD paths derived from DataRoot
  // (/switch/nethersx2/...) plus the resources dir, so the normal case is a
  // pass-through. We only redirect stray Android-internal app paths the core
  // might synthesize, mapping them under DataRoot so they land on the SD card
  // instead of failing. (The Android private data dir prefix and the legacy
  // /sdcard /storage/emulated prefixes.)
  static const char *const android_prefixes[] = {
    "/data/user/0/xyz.aethersx2.android/files",
    "/data/data/xyz.aethersx2.android/files",
    "/storage/emulated/0/Android/data/xyz.aethersx2.android/files",
    "/sdcard/Android/data/xyz.aethersx2.android/files",
  };
  for (unsigned i = 0; i < sizeof(android_prefixes) / sizeof(*android_prefixes); i++) {
    size_t plen = strlen(android_prefixes[i]);
    if (!strncmp(path, android_prefixes[i], plen)) {
      const char *rest = path + plen;
      char *out = buf[which];
      which ^= 1;
      snprintf(out, sizeof(buf[0]), "%s%s", DATA_ROOT, rest);
      return out;
    }
  }

  return path; // absolute SD path (or relative) -- use as-is
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  int fd = open(fix_path(path), convert_open_flags(flags), mode);
  return fd;
}

// bionic's fortified open with no variadic mode (read/existing files)
int open2_fake(const char *path, int flags) {
  return open(fix_path(path), convert_open_flags(flags), 0666);
}

int mkdir_fake(const char *path, unsigned int mode) {
  return mkdir(fix_path(path), mode);
}

int remove_fake(const char *path) {
  return remove(fix_path(path));
}

int rename_fake(const char *from, const char *to) {
  // both sides need remapping; fix_path uses two rotating buffers so a single
  // call to each is safe before we hand them to rename()
  const char *f = fix_path(from);
  const char *t = fix_path(to);
  // Horizon's filesystem RenameFile FAILS if the destination already exists,
  // whereas POSIX rename() atomically replaces it. The game saves atomically
  // (write "<save>.incomplete", then rename over "<save>"), so without
  // clearing the existing target first every save after the first silently
  // fails and all progress is lost on reload. remove() gives POSIX semantics.
  remove(t);
  return rename(f, t);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  const int ret = stat(fix_path(path), &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // NOTE: not thread-safe
  struct dirent *e = readdir((DIR *)dirp);
  if (!e)
    return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return (void *)1;
}

void freelocale_fake(void *loc) {
  (void)loc;
}

void *uselocale_fake(void *loc) {
  (void)loc;
  return (void *)1;
}

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha)
WRAP_ISW_L(iswblank)
WRAP_ISW_L(iswcntrl)
WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower)
WRAP_ISW_L(iswprint)
WRAP_ISW_L(iswpunct)
WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper)
WRAP_ISW_L(iswxdigit)
WRAP_ISW_L(towlower)
WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) {
  (void)loc;
  return strcoll(a, b);
}

size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc;
  return strftime(s, max, fmt, (const struct tm *)tm);
}

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc;
  return strtold(s, end);
}

long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoull(s, end, base);
}

int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) {
  (void)loc;
  return wcscoll(a, b);
}

size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) {
  (void)loc;
  return wcsxfrm(dst, src, n);
}

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  // ascii-ish naive conversion
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr)
// libc++_shared initializes std::cout/cerr against &__sF[1]/&__sF[2];
// these wrappers absorb accesses to those fake FILEs and forward the rest
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#ifdef DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total);
    buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) {
    debugPrintf("stdio: %s", s);
    return 0;
  }
  return fputs(s, f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fclose(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#ifdef DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#ifdef DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int getc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return getc(f);
}

// fopen with a large stream buffer for the big game archives: the engine
// streams the multi-hundred-MB .dat packs with many small reads/seeks, and
// fsdev round trips dominate without buffering.
FILE *fopen_fake(const char *path, const char *mode) {
  // CPU topology: the Switch has no /sys, and the core reads these to size its
  // worker-thread pool -- a failed/garbage read here can spawn a runaway number
  // of threads. Report 4 present/online CPUs (matches the 3 app cores we use).
  if (path && strchr(mode, 'r') && !strncmp(path, "/sys/devices/system/cpu/", 24)) {
    const char *leaf = path + 24;
    const char *content = (!strcmp(leaf, "kernel_max")) ? "63\n" : "0-3\n";
    FILE *m = fmemopen((void *)content, strlen(content), "r");
    debugPrintf("fopen(%s) -> synthetic '%s'\n", path, content);
    if (m) return m;
  }
  const char *p = fix_path(path);
  FILE *f = fopen(p, mode);
  if (f && strchr(mode, 'r')) {
    const char *ext = strrchr(p, '.');
    if (ext && strcasecmp(ext, ".dat") == 0)
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  if (!f)
    debugPrintf("fopen(%s => %s, %s) FAILED\n", path, p, mode);
  else
    debugPrintf("fopen(%s, %s) ok\n", p, mode);
  return f;
}

// ---------------------------------------------------------------------------
// ANativeWindow -> NWindow mapping
// ---------------------------------------------------------------------------

// NOTE: a custom aspect-fitted VI layer was tried and does not work here --
// viCreateLayer only yields a STRAY layer (libnx exposes no viOpenLayer), and
// stray layers cannot be resized or positioned, so viSetLayerSize/Position fail
// with 0x1272 (VI, desc 9). We keep the default full-panel window; the core
// aspect-fits and centres its own draw rect (ComputeDrawRectangle patch).
void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env;
  // a NULL Surface means "no window" (the core's pause path) -> return NULL so
  // it takes the surfaceless/pbuffer branch instead of binding the default win
  if (surface == NULL) {
    debugPrintf("ANativeWindow_fromSurface(NULL) -> NULL (surfaceless)\n");
    return NULL;
  }
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  nwindowSetCrop(win, 0, 0, screen_width, screen_height);
  u32 cw = 0, ch = 0;
  nwindowGetDimensions(win, &cw, &ch);
  debugPrintf("ANativeWindow_fromSurface -> %p dims=%ux%u\n", win, cw, ch);
  return win;
}

int ANativeWindow_getWidth_fake(void *win) {
  (void)win;
  return screen_width;
}

int ANativeWindow_getHeight_fake(void *win) {
  (void)win;
  return screen_height;
}

void ANativeWindow_acquire_fake(void *win) {
  (void)win; // single default window; no refcount
}

void ANativeWindow_release_fake(void *win) {
  (void)win; // never destroy the default window
}

int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)format;
  debugPrintf("ANativeWindow_setBuffersGeometry(%d, %d)\n", w, h);
  if (w > 0 && h > 0)
    nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection
// (bionic types are plain structs the game allocates; we stash a pointer
// to the real object in their first bytes, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  Semaphore sem;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) {
    free(*s);
    *s = NULL;
  }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s)
    semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s)
    semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem))
    return 0;
  errno = EAGAIN;
  return -1;
}

// ---------------------------------------------------------------------------
// libjnigraphics: AndroidBitmap_* (screenshots/covers -- stubbable)
// The core calls Bitmap.createBitmap via JNI (jni_fake returns a fake object)
// then locks its pixels here. We hand back a transient owned buffer; screenshot
// content is discarded. Returns ANDROID_BITMAP_RESULT_SUCCESS (0).
// ---------------------------------------------------------------------------

#define ANDROID_BITMAP_RESULT_SUCCESS 0

static void *g_bitmap_scratch = NULL;
static size_t g_bitmap_scratch_size = 0;

int AndroidBitmap_lockPixels_fake(void *env, void *bitmap, void **addrPtr) {
  (void)env; (void)bitmap;
  // a generous scratch (max plausible cover/screenshot at 4 bytes/px)
  const size_t need = (size_t)1920 * 1080 * 4;
  if (g_bitmap_scratch_size < need) {
    free(g_bitmap_scratch);
    g_bitmap_scratch = malloc(need);
    g_bitmap_scratch_size = g_bitmap_scratch ? need : 0;
  }
  if (addrPtr) *addrPtr = g_bitmap_scratch;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int AndroidBitmap_unlockPixels_fake(void *env, void *bitmap) {
  (void)env; (void)bitmap;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

// ASharedMemory_create(name, size) -> fd. THIS IS LOAD-BEARING: AetherSX2's
// VirtualMemoryManager (the PS2 RAM / VTLB backing) is created over an ashmem
// region so it can be mmap'd. The core dlopen's libandroid.so + dlsym's this
// (API-26 compat). Horizon has no memfd/ashmem and no functional file mmap, so
// we back the "fd" with a page-aligned heap buffer and a fake high-numbered fd;
// mmap_fake(fd) then hands out views into that buffer (see below). Without this,
// the VMManager base stays null and VtlbMemoryReserve aborts.
//
// (Fastmem RAM mirrors -- the same physical RAM at many host addresses -- are NOT
// possible on Horizon; see main.c. This is a single linear backing = software VTLB.)
#define ASHMEM_FD_BASE 0x40000000
#define MAX_ASHMEM     16
typedef struct { int used; void *ptr; size_t size; } AshmemRegion;
static AshmemRegion g_ashmem[MAX_ASHMEM];
static Mutex g_ashmem_lock;

// returns the region for a fake ashmem fd, or NULL if fd isn't one of ours
static AshmemRegion *ashmem_get(int fd) {
  if (fd < ASHMEM_FD_BASE || fd >= ASHMEM_FD_BASE + MAX_ASHMEM) return NULL;
  AshmemRegion *r = &g_ashmem[fd - ASHMEM_FD_BASE];
  return r->used ? r : NULL;
}

int ASharedMemory_create_fake(const char *name, size_t size) {
  size_t sz = (size + 0xFFF) & ~(size_t)0xFFF;
  void *p = memalign(0x1000, sz);
  if (!p) {
    debugPrintf("ashmem: create '%s' size=%zu FAILED (oom)\n", name ? name : "?", size);
    errno = ENOMEM;
    return -1;
  }
  memset(p, 0, sz);
  mutexLock(&g_ashmem_lock);
  int fd = -1;
  for (int i = 0; i < MAX_ASHMEM; i++) {
    if (!g_ashmem[i].used) { g_ashmem[i] = (AshmemRegion){ 1, p, sz }; fd = ASHMEM_FD_BASE + i; break; }
  }
  mutexUnlock(&g_ashmem_lock);
  if (fd < 0) { free(p); errno = EMFILE; return -1; }
  debugPrintf("ashmem: create '%s' size=%zu -> fd=%d ptr=%p\n", name ? name : "?", size, fd, p);
  return fd;
}

// close() shim: ashmem fds are kept alive (their backing stays mapped); real fds
// forward to newlib. The core closes the ashmem fd after mmap -- the mapping must
// survive, so we DON'T free the backing here (it lives for the session).
int close_fake(int fd) {
  if (ashmem_get(fd)) { debugPrintf("ashmem: close fd=%d (kept)\n", fd); return 0; }
  return close(fd);
}

// ashmem ioctl handler. AetherSX2 calls ASharedMemory_create THEN configures the
// fd with the raw ashmem ioctls (SET_NAME/SET_SIZE) and bails if SET_SIZE fails.
// Our region is already created at the right size, so SET_* just succeed; GET_SIZE
// returns it. Codes: _IOW(0x77,n,..) -- SET_NAME=0x41007701, SET_SIZE=0x40087703,
// GET_SIZE=0x7704, SET_PROT_MASK=0x40087705, GET_PROT_MASK=0x7706.
// Returns 0 and sets *ret if fd is one of ours; -1 if not an ashmem fd.
int ashmem_ioctl(int fd, unsigned long req, int *ret) {
  AshmemRegion *r = ashmem_get(fd);
  if (!r) return -1;
  switch (req & 0xffffffff) {
    case 0x00007704: *ret = (int)r->size; break;   // ASHMEM_GET_SIZE
    case 0x00007706: *ret = 0x7;          break;   // ASHMEM_GET_PROT_MASK -> RWX
    default:         *ret = 0;            break;   // SET_NAME/SET_SIZE/SET_PROT/PIN... -> ok
  }
  debugPrintf("ashmem: ioctl fd=%d req=0x%lx -> %d\n", fd, req, *ret);
  return 0;
}

// ---------------------------------------------------------------------------
// JIT-capable mmap/mprotect/munmap  (THE #1 risk -- PS2 recompiler RX memory)
//
// libnx/newlib has no functional mmap. The PS2 recompiler reserves code arenas
// and toggles them W^X via mprotect. Horizon forbids RWX, but allows a single
// virtual address to flip between RW and RX if it is "process code memory":
//   src (heap) --svcMapProcessCodeMemory--> dst (code region)
//   then svcSetProcessMemoryPermission(dst, Perm_Rw)  for codegen,
//        svcSetProcessMemoryPermission(dst, Perm_Rx)  to execute.
// This matches PCSX2's W^X mprotect model. Plain (non-exec) anonymous mappings
// fall back to aligned heap allocation. FLAG: validate the recompiler arenas on
// hardware; if a region needs simultaneous RWX this model must change.
// ---------------------------------------------------------------------------

#ifndef PROT_NONE
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#define MAP_ANON 0x20
#endif

#define MMAP_PAGE 0x1000
#define MAX_MMAP_REGIONS 256

typedef struct {
  void *dst;     // returned address (NULL if free slot). For code=the RW backing.
  void *src;     // heap backing (NULL for plain alloc / reservation)
  size_t size;
  int is_code;   // 0=plain heap, 1=dual-mapped JIT, 2=virtmem reservation
  void *rv;      // VirtmemReservation* for the code-mem / reserved range (NULL if none)
  void *code_rx; // executable alias of the backing (code only); NULL otherwise
} MmapRegion;

static MmapRegion g_mmaps[MAX_MMAP_REGIONS];
static Mutex g_mmap_lock;

// --- dual-mapped JIT redirect table (read by the exception handler, crash.c) ---
// AetherSX2's recompiler expects RWX: it writes a block to the arena then jumps
// into it, WITHOUT any mprotect (Horizon also forbids W->X on code memory). So we
// hand the core a plain RW heap buffer `back`, keep an executable alias `rx` of
// the same physical pages, and when the core jumps into `back` (instruction abort,
// RW isn't executable) the handler bumps PC by `delta` (= rx - back) so execution
// continues in the RX alias. Direct (PC-relative) branches between blocks keep the
// same delta so they stay in the RX view; only entries from the emucore dispatcher
// fault-and-redirect. Lock-free array (few arenas, only appended) for handler use.
// (JitRedirect + these externs are declared in libc_shim.h for crash.c.)
JitRedirect g_jit_redir[8];
volatile int g_jit_redir_n = 0;
static volatile long g_mmap_total = 0; // bytes currently handed to the core via mmap_fake


// exported for the JNI OOM diagnostic (jni_fake.c)
long jit_mmap_total_kb(void) { return g_mmap_total / 1024; }

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  if (length == 0) return MAP_FAILED;
  const size_t len = (length + MMAP_PAGE - 1) & ~(size_t)(MMAP_PAGE - 1);

  // ashmem-backed mapping (the PS2 RAM / VTLB backing): hand out a view into the
  // region's heap buffer. Fastmem (EnableFastmem) is OFF, so the core never asks
  // for MAP_FIXED mirror pages -- it uses the software VTLB over this single view.
  AshmemRegion *ash = ashmem_get(fd);
  if (ash) {
    if ((size_t)offset + len > ash->size) {
      debugPrintf("mmap_fake: ashmem fd=%d off+len (0x%lx) > size (0x%zx) -- FAILED\n",
                  fd, (unsigned long)offset + len, ash->size);
      return MAP_FAILED;
    }
    // MAP_FIXED `addr` is not honored (Horizon can't alias) -- return the canonical
    // linear view; the core uses the software VTLB.
    void *src = (char *)ash->ptr + offset;
    static unsigned long ash_maps = 0;
    if (ash_maps < 4 || (ash_maps & 0xfff) == 0)
      debugPrintf("mmap_fake: ashmem fd=%d off=0x%lx len=%lu KB -> %p (map #%lu)\n",
                  fd, (unsigned long)offset, (unsigned long)(len / 1024), src, ash_maps);
    ash_maps++;
    return src;
  }

  // verbose log only for the (rare) anonymous / reservation / code-arena maps
  debugPrintf("mmap_fake: addr=%p len=%lu KB prot=0x%x flags=0x%x fd=%d off=0x%lx\n",
              addr, (unsigned long)(len / 1024), prot, flags, fd, (unsigned long)offset);

  if (fd >= 0)
    return MAP_FAILED;            // other file-backed mappings unsupported
  (void)flags;

  // Large PROT_NONE anonymous mapping = a pure address-space RESERVATION: the PS2
  // fastmem window is a 4GB mmap(PROT_NONE) that the core only reserves + records
  // now (RAM mirrors get mapped into it later). memalign(4GB) is impossible AND
  // wrong here -- reserve virtual address space via virtmem instead (no commit).
  if (prot == PROT_NONE) {
    virtmemLock();
    void *base = virtmemFindAslr(len, 0);
    VirtmemReservation *rv = base ? virtmemAddReservation(base, len) : NULL;
    virtmemUnlock();
    if (!base) {
      debugPrintf("mmap_fake: RESERVE %lu KB FAILED (no address space)\n", (unsigned long)(len / 1024));
      return MAP_FAILED;
    }
    debugPrintf("mmap_fake: RESERVED addr-space %p len=%lu KB (PROT_NONE)\n",
                base, (unsigned long)(len / 1024));
    mutexLock(&g_mmap_lock);
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
      if (!g_mmaps[i].dst) { g_mmaps[i] = (MmapRegion){ base, NULL, len, 2, (void *)rv }; break; }
    }
    mutexUnlock(&g_mmap_lock);
    return base;
  }

  // non-executable, never-executable data: plain aligned heap is cheapest.
  // (recompiler data arenas + the PS2 RAM mirrors never need exec.)
  if (!(prot & PROT_EXEC)) {
    void *p = memalign(MMAP_PAGE, len);
    if (!p) { debugPrintf("mmap_fake: memalign(%lu KB) FAILED\n", (unsigned long)(len/1024)); return MAP_FAILED; }
    g_mmap_total += (long)len;
    if (prot == PROT_NONE) { /* leave committed; PROT_NONE rarely enforced */ }
    mutexLock(&g_mmap_lock);
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
      if (!g_mmaps[i].dst) {
        g_mmaps[i] = (MmapRegion){ p, NULL, len, 0, NULL };
        break;
      }
    }
    mutexUnlock(&g_mmap_lock);
    return p;
  }

  // executable JIT arena via CodeMemory DUAL-VIEW. AetherSX2's recompiler writes
  // machine code then runs it, with no mprotect (Android RWX). Horizon forbids
  // W^X-violating perms AND svcMapProcessCodeMemory makes the *source* buffer
  // inaccessible -- so a single mapping can't be both writable and executable, and
  // the earlier "return the heap src" attempt faulted when the emitter wrote to it.
  // svcCreateCodeMemory backs a code object from a heap buffer; MapOwner gives a RW
  // alias and MapSlave a RX alias of the SAME physical pages, BOTH live at once.
  // Return the RW view; when the core jumps into it (instruction abort, RW isn't
  // executable) the exception handler redirects to the RX view (+delta). Direct
  // PC-relative branches keep the same delta so linked blocks stay in the RX view.
  void *back = memalign(MMAP_PAGE, len);   // physical backing (owned by the code obj)
  if (!back) {
    debugPrintf("mmap_fake: JIT backing memalign(%lu KB) FAILED\n", (unsigned long)(len / 1024));
    return MAP_FAILED;
  }
  Handle ch = 0;
  Result rcC = svcCreateCodeMemory(&ch, back, len);
  if (R_FAILED(rcC)) {
    debugPrintf("mmap_fake: svcCreateCodeMemory(%lu KB) rc=0x%x\n", (unsigned long)(len / 1024), rcC);
    free(back);
    return MAP_FAILED;
  }
  virtmemLock();
  void *rw = virtmemFindCodeMemory(len, MMAP_PAGE);
  VirtmemReservation *rvw = rw ? virtmemAddReservation(rw, len) : NULL;
  void *rx = virtmemFindCodeMemory(len, MMAP_PAGE);
  VirtmemReservation *rvx = rx ? virtmemAddReservation(rx, len) : NULL;
  virtmemUnlock();
  Result rcO = (rw && rx) ? svcControlCodeMemory(ch, CodeMapOperation_MapOwner, rw, len, Perm_Rw) : MAKERESULT(1, 1);
  Result rcS = (rw && rx) ? svcControlCodeMemory(ch, CodeMapOperation_MapSlave, rx, len, Perm_Rx) : MAKERESULT(1, 1);
  debugPrintf("mmap_fake: JIT CodeMem create rc=0x%x MapOwner(rw=%p) rc=0x%x MapSlave(rx=%p) rc=0x%x delta=0x%lx\n",
              rcC, rw, rcO, rx, rcS, (unsigned long)((char *)rx - (char *)rw));
  if (R_FAILED(rcO) || R_FAILED(rcS)) {
    if (rvw) { virtmemLock(); virtmemRemoveReservation(rvw); virtmemUnlock(); }
    if (rvx) { virtmemLock(); virtmemRemoveReservation(rvx); virtmemUnlock(); }
    svcCloseHandle(ch);
    free(back);
    return MAP_FAILED;
  }
  // WRITE-redirect: hand the core the RX (executable) view so recompiled code
  // runs DIRECTLY with no per-dispatch faults. The recompiler's codegen writes to
  // RX (which isn't writable) fault (data abort); the exception handler decodes
  // the store and re-issues it to the RW alias (far + delta, delta = rw - rx).
  // Writes are far rarer than dispatches -> much faster + fewer exceptions.
  if (g_jit_redir_n < (int)(sizeof(g_jit_redir) / sizeof(g_jit_redir[0]))) {
    g_jit_redir[g_jit_redir_n] = (JitRedirect){ (uintptr_t)rx, (uintptr_t)rx + len,
                                                (intptr_t)((char *)rw - (char *)rx) };
    g_jit_redir_n++;
  }
  g_mmap_total += (long)len;
  mutexLock(&g_mmap_lock);
  for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
    if (!g_mmaps[i].dst) {
      // dst=rx (returned RX view; what the core sees), code_rx=rx, src=back(backing),
      // rv=rvw. rvx/ch leak on munmap -- the JIT arena is session-lived.
      g_mmaps[i] = (MmapRegion){ rx, back, len, 1, rvw, rx };
      break;
    }
  }
  mutexUnlock(&g_mmap_lock);
  return rx; // the RX execute view; codegen WRITES are redirected to RW via crash.c
}

int mprotect_fake(void *addr, size_t length, int prot) {
  const size_t len = (length + MMAP_PAGE - 1) & ~(size_t)(MMAP_PAGE - 1);
  mutexLock(&g_mmap_lock);
  for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
    if (g_mmaps[i].dst == addr || (g_mmaps[i].is_code && addr >= g_mmaps[i].dst &&
        (char *)addr < (char *)g_mmaps[i].dst + g_mmaps[i].size)) {
      mutexUnlock(&g_mmap_lock);
      // Dual-mapped JIT (is_code==1): `back` stays RW, the `rx` alias stays RX --
      // W^X is handled by the exec-redirect, so the core's mprotect (which Horizon
      // would reject on code memory anyway) is a no-op. Plain data (0) / bare
      // reservation (2): nothing to enforce either.
      (void)len;
      return 0;
    }
  }
  mutexUnlock(&g_mmap_lock);
  // unknown region: best-effort no-op (the core sometimes mprotects its own bss)
  return 0;
}

int munmap_fake(void *addr, size_t length) {
  (void)length;
  mutexLock(&g_mmap_lock);
  for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
    if (g_mmaps[i].dst == addr) {
      MmapRegion r = g_mmaps[i];
      g_mmaps[i].dst = NULL;
      if (r.is_code != 2) g_mmap_total -= (long)r.size; // reservations aren't committed
      mutexUnlock(&g_mmap_lock);
      if (r.is_code == 2) {
        // bare address-space reservation: nothing mapped, just release it
        if (r.rv) { virtmemLock(); virtmemRemoveReservation((VirtmemReservation *)r.rv); virtmemUnlock(); }
      } else if (r.is_code == 1) {
        // dual-view JIT (svcCreateCodeMemory): full teardown needs the code handle
        // and both reservations, which we don't retain -- the JIT arena is session-
        // lived (created once per VM), so drop it (leak). Release the RW reservation
        // best-effort; the RX reservation, code handle and backing intentionally leak.
        if (r.rv) { virtmemLock(); virtmemRemoveReservation((VirtmemReservation *)r.rv); virtmemUnlock(); }
      } else {
        free(r.dst);
      }
      return 0;
    }
  }
  mutexUnlock(&g_mmap_lock);
  return 0;
}
