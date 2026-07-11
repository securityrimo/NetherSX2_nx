/* egl.c -- thin EGL pass-through to switch-mesa for NetherSX2_nx
 *
 * libemucore.so has NO libGLESv2 in DT_NEEDED: it resolves every GL entry
 * point through eglGetProcAddress (GLAD). It also runs ONE GLES3.1+ context on
 * a dedicated GS thread (SysMtgsThread) that owns it for life. So unlike the
 * lswtcs port -- which had to virtualize four sloppy shared contexts across
 * threads -- this layer is a near-verbatim pass-through to mesa. We keep only
 * the bring-up fixes the Switch stack actually needs:
 *  - eglGetProcAddress returns mesa's REAL GL pointers (THE linchpin: GLAD
 *    fails to load otherwise and nothing renders);
 *  - eglChooseConfig strips EGL_PBUFFER_BIT (no Switch pbuffer configs) and
 *    forces an ES3-renderable config, falling back to relaxed/minimal lists;
 *  - eglCreatePbufferSurface fakes a tiny surface when mesa has no pbuffer
 *    config (the GS thread asks for one to go "surfaceless" on pause);
 *  - eglQuerySurface overrides a 0x0 window result with the screen size (mesa
 *    reports 0x0 before the first buffer acquire; the core would build a 0x0
 *    render target -> black screen);
 *  - glGetString never returns NULL (device sniffing dereferences it).
 *
 * The ownership park/release/handover hooks implement a SIMPLE sticky
 * single-owner model: the first bring-up may run on the wrapper main thread
 * (during changeSurface) and is handed to the GS thread afterwards. In steady
 * state there is one GL thread, so these are effectively no-ops.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"

// EGL_OPENGL_ES3_BIT lives in eglext.h (KHR_create_context); define it here so
// we don't depend on that header being on the include path.
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

// switch-mesa exposes no pbuffer-capable configs. When the core asks for a
// pbuffer (its surfaceless-context fallback while paused) and mesa refuses, we
// hand back this stand-in. It records only geometry; the GS thread treats it
// as "no real drawable" and we bind the window surface (or NO_SURFACE) instead.
#define FAKE_PBUFFER_MAGIC 0x50425546u /* "PBUF" */

typedef struct FakePbuffer {
  uint32_t magic;
  EGLint width;
  EGLint height;
} FakePbuffer;

// Globals shared with main.c (heartbeat / cpu-boost-off) and the rest of the
// EGL layer. Single context, single window surface, single display.
volatile int egl_swap_count = 0;
EGLDisplay last_dpy = EGL_NO_DISPLAY;
EGLSurface main_window_surface = EGL_NO_SURFACE;
EGLContext main_real_context = EGL_NO_CONTEXT;

// ---------------------------------------------------------------------------
// Simple sticky single-owner GL ownership.
//
// In steady state the GS thread owns the one real context for its whole life,
// so no handshake is needed. But the FIRST eglMakeCurrent may happen on the
// wrapper main thread during changeSurface; main.c then calls
// egl_gl_ownership_release() so the GS thread can take it. We track only "does
// THIS thread currently hold the real context" in TLS, plus the one identity
// of the holder so a release from the wrong thread is a no-op.
// ---------------------------------------------------------------------------
static __thread int tls_holds_context = 0;       // this thread bound a context
static pthread_t gl_owner;                        // who holds it now
static int gl_owner_valid = 0;
static pthread_mutex_t gl_owner_mutex = PTHREAD_MUTEX_INITIALIZER;

extern int pthr_is_render_thread(void);

// release whatever this thread bound; the next makeCurrent rebinds. eglMakeCurrent
// flushes the outgoing context implicitly (one real context = one command stream).
static void release_gl_ownership(void) {
  if (!tls_holds_context)
    return;
  if (last_dpy != EGL_NO_DISPLAY)
    eglMakeCurrent(last_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  pthread_mutex_lock(&gl_owner_mutex);
  if (gl_owner_valid && pthread_equal(gl_owner, pthread_self()))
    gl_owner_valid = 0;
  pthread_mutex_unlock(&gl_owner_mutex);
  tls_holds_context = 0;
}

// before a thread blocks (from the pthr wait shims). The render/GS thread parks
// every frame; keep its binding so uncontended frames pay no unbind/rebind --
// it is the sole long-lived GL thread. Other threads (the one-shot main
// bring-up) release on park so they never orphan the context.
void egl_gl_ownership_park(void) {
  if (pthr_is_render_thread())
    return;
  release_gl_ownership();
}

// unconditional release (end of the main thread's startup hand-off, or thread
// exit). The park above keeps the binding for the render thread, which would
// otherwise orphan it; main.c calls this after the first bring-up.
void egl_gl_ownership_release(void) {
  release_gl_ownership();
}

// service point for threads that keep running without touching GL or parking.
// With a single GL thread there is never a waiter, so this is a no-op; kept so
// the main loop can call it cheaply alongside lswtcs's wiring.
void egl_gl_service_handover(void) {
  // no contended owner model: nothing to hand over in the single-context case.
}

int egl_gl_thread_holds_context(void) {
  return tls_holds_context;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static void log_attribs(const char *tag, const EGLint *attribs) {
  if (!attribs) {
    debugPrintf("EGL: %s attribs = (null)\n", tag);
    return;
  }
  char buf[512];
  int pos = 0;
  for (int i = 0; attribs[i] != EGL_NONE && pos < (int)sizeof(buf) - 32; i += 2)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%04x=%d ", attribs[i], attribs[i + 1]);
  buf[pos] = '\0';
  debugPrintf("EGL: %s attribs = [ %s]\n", tag, buf);
}

static EGLint attrib_value(const EGLint *attribs, EGLint key, EGLint fallback) {
  if (!attribs)
    return fallback;
  for (int i = 0; attribs[i] != EGL_NONE; i += 2)
    if (attribs[i] == key)
      return attribs[i + 1];
  return fallback;
}

static FakePbuffer *fake_pbuffer_from_surface(EGLSurface surface) {
  // a FakePbuffer pointer never collides with a real EGLSurface (real ones
  // come from mesa); the magic word disambiguates safely.
  if (surface == EGL_NO_SURFACE)
    return NULL;
  FakePbuffer *p = (FakePbuffer *)surface;
  return (p->magic == FAKE_PBUFFER_MAGIC) ? p : NULL;
}

// ---------------------------------------------------------------------------
// EGL hooks (registered in imports.c in place of the real eglXxx)
// ---------------------------------------------------------------------------

EGLDisplay eglGetDisplayHook(EGLNativeDisplayType display_id) {
  // mesa uses the Switch default display regardless of display_id.
  EGLDisplay dpy = eglGetDisplay(display_id);
  debugPrintf("EGL: eglGetDisplay(%p) -> %p\n", (void *)display_id, dpy);
  return dpy;
}

EGLBoolean eglInitializeHook(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  EGLBoolean r = eglInitialize(dpy, major, minor);
  debugPrintf("EGL: eglInitialize -> %d (v%d.%d)\n", r,
              major ? *major : 0, minor ? *minor : 0);
  return r;
}

EGLBoolean eglBindAPIHook(EGLenum api) {
  // the OGL renderer binds EGL_OPENGL_ES_API; pass it straight through.
  EGLBoolean r = eglBindAPI(api);
  debugPrintf("EGL: eglBindAPI(0x%x) -> %d\n", api, r);
  return r;
}

const char *eglQueryStringHook(EGLDisplay dpy, EGLint name) {
  // the core reads EGL_EXTENSIONS to decide surfaceless vs pbuffer; pass mesa's
  // real answer through so it picks the path mesa actually supports.
  const char *s = eglQueryString(dpy, name);
  debugPrintf("EGL: eglQueryString(0x%x) -> %s\n", name, s ? s : "(null)");
  return s;
}

EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config) {
  log_attribs("eglChooseConfig", attrib_list);

  // Copy the attrib list, then: strip EGL_PBUFFER_BIT from EGL_SURFACE_TYPE
  // (no Switch mesa config has it; the pbuffer it's meant for is faked below),
  // and ensure EGL_RENDERABLE_TYPE includes ES3 (the core is a GLES3.1+ client).
  EGLint attribs[64];
  int n = 0;
  int saw_renderable = 0;
  if (attrib_list) {
    while (attrib_list[n] != EGL_NONE && n < 60) {
      attribs[n] = attrib_list[n];
      attribs[n + 1] = attrib_list[n + 1];
      if (attribs[n] == EGL_SURFACE_TYPE && (attribs[n + 1] & EGL_PBUFFER_BIT)) {
        debugPrintf("EGL: stripping PBUFFER bit from surface type 0x%x\n", attribs[n + 1]);
        attribs[n + 1] &= ~EGL_PBUFFER_BIT;
      }
      if (attribs[n] == EGL_RENDERABLE_TYPE) {
        attribs[n + 1] |= EGL_OPENGL_ES3_BIT; // never ask for less than ES3
        saw_renderable = 1;
      }
      n += 2;
    }
  }
  if (!saw_renderable && n < 60) {
    attribs[n++] = EGL_RENDERABLE_TYPE;
    attribs[n++] = EGL_OPENGL_ES3_BIT;
  }
  attribs[n] = EGL_NONE;

  EGLBoolean r = eglChooseConfig(dpy, attribs, configs, config_size, num_config);
  debugPrintf("EGL: eglChooseConfig -> %d, %d configs\n", r, num_config ? *num_config : -1);
  if (r == EGL_TRUE && num_config && *num_config > 0)
    return r;

  // Nothing matched: retry with progressively relaxed lists so the core always
  // gets a usable ES3 window config (it logs but tolerates failure here).
  static const EGLint relaxed[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  static const EGLint minimal[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_NONE
  };
  const EGLint *fallbacks[] = { relaxed, minimal };
  for (unsigned i = 0; i < 2; i++) {
    r = eglChooseConfig(dpy, fallbacks[i], configs, config_size, num_config);
    debugPrintf("EGL: eglChooseConfig fallback %u -> %d, %d configs\n",
                i, r, num_config ? *num_config : -1);
    if (r == EGL_TRUE && num_config && *num_config > 0)
      return r;
  }
  return r;
}

EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list) {
  // Single GS context: straight pass-through, no aliasing. Cache it so the
  // ownership/release path and main.c know the one real context.
  log_attribs("eglCreateContext", attrib_list);
  EGLContext ctx = eglCreateContext(dpy, config, share_context, attrib_list);
  if (ctx != EGL_NO_CONTEXT)
    main_real_context = ctx;
  debugPrintf("EGL: eglCreateContext(cfg=%p, share=%p) -> %p (err 0x%x)\n",
              config, share_context, ctx, eglGetError());
  return ctx;
}

EGLSurface eglCreateWindowSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                      EGLNativeWindowType win, const EGLint *attrib_list) {
  // win is the NWindow* from our ANativeWindow_fromSurface shim, which is
  // exactly libnx EGL's EGLNativeWindowType. Pass through; cache for queries.
  EGLSurface s = eglCreateWindowSurface(dpy, config, win, attrib_list);
  if (s != EGL_NO_SURFACE) {
    last_dpy = dpy;
    main_window_surface = s;
  }
  debugPrintf("EGL: eglCreateWindowSurface(cfg=%p, win=%p) -> %p (err 0x%x)\n",
              config, (void *)win, s, eglGetError());
  return s;
}

EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list) {
  log_attribs("eglCreatePbufferSurface", attrib_list);
  // Try the real call first in case mesa ever grows a pbuffer config.
  EGLSurface s = eglCreatePbufferSurface(dpy, config, attrib_list);
  if (s != EGL_NO_SURFACE) {
    debugPrintf("EGL: eglCreatePbufferSurface -> %p (real)\n", s);
    return s;
  }
  EGLint err = eglGetError();

  // No pbuffer config: hand back a stand-in recording WIDTH/HEIGHT. The GS
  // thread uses this only to go surfaceless while paused; eglMakeCurrent binds
  // the window surface (or NO_SURFACE) and eglSwapBuffers is a no-op for it.
  FakePbuffer *fake = (FakePbuffer *)calloc(1, sizeof(*fake));
  if (!fake) {
    debugPrintf("EGL: eglCreatePbufferSurface failed (err 0x%x), fake alloc failed\n", err);
    return EGL_NO_SURFACE;
  }
  fake->magic = FAKE_PBUFFER_MAGIC;
  fake->width = attrib_value(attrib_list, EGL_WIDTH, 1);
  fake->height = attrib_value(attrib_list, EGL_HEIGHT, 1);
  if (fake->width <= 0) fake->width = 1;
  if (fake->height <= 0) fake->height = 1;
  debugPrintf("EGL: eglCreatePbufferSurface failed (err 0x%x), faking %dx%d pbuffer %p\n",
              err, fake->width, fake->height, (void *)fake);
  return (EGLSurface)fake;
}

EGLBoolean eglDestroySurfaceHook(EGLDisplay dpy, EGLSurface surface) {
  FakePbuffer *fake = fake_pbuffer_from_surface(surface);
  if (fake) {
    debugPrintf("EGL: eglDestroySurface(fake pbuffer %p)\n", (void *)fake);
    free(fake);
    return EGL_TRUE;
  }
  if (surface == main_window_surface)
    main_window_surface = EGL_NO_SURFACE;
  EGLBoolean r = eglDestroySurface(dpy, surface);
  debugPrintf("EGL: eglDestroySurface(%p) -> %d\n", surface, r);
  return r;
}

EGLBoolean eglDestroyContextHook(EGLDisplay dpy, EGLContext ctx) {
  if (ctx == main_real_context)
    main_real_context = EGL_NO_CONTEXT;
  EGLBoolean r = eglDestroyContext(dpy, ctx);
  debugPrintf("EGL: eglDestroyContext(%p) -> %d\n", ctx, r);
  return r;
}

static volatile int egl_makecurrent_count = 0;

EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  const int n = ++egl_makecurrent_count;
  const int log = (n <= 40);

  if (ctx == EGL_NO_CONTEXT || ctx == NULL) {
    // the core's release point (it does this on pause). Drop the real binding.
    EGLBoolean r = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    last_dpy = dpy;
    pthread_mutex_lock(&gl_owner_mutex);
    if (gl_owner_valid && pthread_equal(gl_owner, pthread_self()))
      gl_owner_valid = 0;
    pthread_mutex_unlock(&gl_owner_mutex);
    tls_holds_context = 0;
    if (log)
      debugPrintf("EGL: eglMakeCurrent #%d release -> %d\n", n, r);
    return r;
  }

  // A fake pbuffer has no real drawable. Bind the window surface if we have one
  // (resume), else NO_SURFACE (surfaceless pause) -- either way with the real
  // context. Mesa must advertise KHR_surfaceless_context for the NO_SURFACE
  // case; otherwise the window surface keeps the context valid.
  FakePbuffer *fake_draw = fake_pbuffer_from_surface(draw);
  FakePbuffer *fake_read = fake_pbuffer_from_surface(read);
  EGLSurface real_draw = draw;
  EGLSurface real_read = read;
  if (fake_draw || fake_read) {
    real_draw = (main_window_surface != EGL_NO_SURFACE) ? main_window_surface : EGL_NO_SURFACE;
    real_read = real_draw;
  } else if (draw != EGL_NO_SURFACE) {
    main_window_surface = draw;
    main_real_context = ctx;
  }

  last_dpy = dpy;
  EGLBoolean r = eglMakeCurrent(dpy, real_draw, real_read, ctx);
  if (r == EGL_TRUE) {
    pthread_mutex_lock(&gl_owner_mutex);
    gl_owner = pthread_self();
    gl_owner_valid = 1;
    pthread_mutex_unlock(&gl_owner_mutex);
    tls_holds_context = 1;
  } else {
    static int nfail = 0;
    if (nfail++ < 20)
      debugPrintf("EGL: eglMakeCurrent #%d FAILED (draw=%p ctx=%p err=0x%x)\n",
                  n, real_draw, ctx, eglGetError());
  }
  if (log)
    debugPrintf("EGL: eglMakeCurrent #%d (draw=%p read=%p ctx=%p fake=%p) -> %d\n",
                n, draw, read, ctx, (void *)fake_draw, r);
  return r;
}

EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  FakePbuffer *fake = fake_pbuffer_from_surface(surface);
  if (fake) {
    EGLint v = 0;
    switch (attribute) {
      case EGL_WIDTH:  v = fake->width;  break;
      case EGL_HEIGHT: v = fake->height; break;
      default:         v = 0;            break;
    }
    if (value) *value = v;
    debugPrintf("EGL: eglQuerySurface(fake pbuffer %p, 0x%x) -> %d\n",
                (void *)fake, attribute, v);
    return EGL_TRUE;
  }

  EGLBoolean r = eglQuerySurface(dpy, surface, attribute, value);
  // CRITICAL: mesa reports the window surface as 0x0 until the first buffer is
  // acquired. The core trusts this and builds a 0x0 render target -> black
  // screen. Override a zero WIDTH/HEIGHT with the real framebuffer size.
  if (r == EGL_TRUE && value && *value == 0) {
    if (attribute == EGL_WIDTH) {
      debugPrintf("EGL: eglQuerySurface(%p, WIDTH) = 0, overriding -> %d\n", surface, screen_width);
      *value = screen_width;
      return r;
    }
    if (attribute == EGL_HEIGHT) {
      debugPrintf("EGL: eglQuerySurface(%p, HEIGHT) = 0, overriding -> %d\n", surface, screen_height);
      *value = screen_height;
      return r;
    }
  }
  debugPrintf("EGL: eglQuerySurface(%p, 0x%x) -> %d, value=%d\n",
              surface, attribute, r, value ? *value : -1);
  return r;
}

EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval) {
  // the core uses this for vsync pacing; mesa/NWindow honour it. Pass through.
  EGLBoolean r = eglSwapInterval(dpy, interval);
  debugPrintf("EGL: eglSwapInterval(%d) -> %d\n", interval, r);
  return r;
}

EGLBoolean eglSwapBuffersHook(EGLDisplay dpy, EGLSurface surface) {
  // bumped unconditionally: main.c reads egl_swap_count for the cpu-boost-off
  // trigger and the heartbeat (shows whether the GS thread is still presenting).
  ++egl_swap_count;

  // Present heartbeat: first few frames, then every 30th. Tells the log whether
  // the GS is actually presenting (frames advancing = running, just slow) or
  // stuck at N frames (hang after the overlay).
  if (egl_swap_count <= 8 || (egl_swap_count % 30) == 0)
    debugPrintf("EGL: eglSwapBuffers frame #%d\n", egl_swap_count);

  // a fake pbuffer has nothing to present (paused/surfaceless); succeed quietly.
  if (fake_pbuffer_from_surface(surface))
    return EGL_TRUE;

  EGLBoolean r = eglSwapBuffers(dpy, surface);
  if (r != EGL_TRUE) {
    static int nfail = 0;
    if (nfail++ < 20)
      debugPrintf("EGL: eglSwapBuffers FAILED (err=0x%x surface=%p)\n",
                  eglGetError(), surface);
  }
  return r;
}

void *eglGetProcAddressHook(const char *name) {
  // GLAD resolves the whole GLES entry set through this; return mesa's real
  // pointers unchanged except glGetString, which is NULL-guarded below.
  void *p = (void *)eglGetProcAddress(name);
  if (name && p && !strcmp(name, "glGetString"))
    p = (void *)glGetStringHook;
  debugPrintf("EGL: eglGetProcAddress(%s) -> %p\n", name ? name : "(null)", p);
  return p;
}

const GLubyte *glGetStringHook(GLenum name) {
  // The core's device detection (DetermineDeviceSpecs / GLAD GL_VERSION parse)
  // dereferences these without a NULL check. mesa always returns valid strings
  // with a context current, but guard anyway with a safe ES3 fallback.
  const GLubyte *s = glGetString(name);
  if (s) {
    if (name == GL_VENDOR || name == GL_RENDERER || name == GL_VERSION)
      debugPrintf("GL: glGetString(0x%x) = %s\n", name, (const char *)s);
    return s;
  }
  debugPrintf("GL: glGetString(0x%x) = NULL! no context? using fallback\n", name);
  switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"NVIDIA";
    case GL_RENDERER: return (const GLubyte *)"NVIDIA Tegra";
    case GL_VERSION:  return (const GLubyte *)"OpenGL ES 3.2";
    case 0x8B8C:      return (const GLubyte *)"OpenGL ES GLSL ES 3.20"; // GL_SHADING_LANGUAGE_VERSION
    case GL_EXTENSIONS:
      return (const GLubyte *)"GL_OES_surfaceless_context "
                              "GL_OES_depth24 GL_OES_packed_depth_stencil";
    default:          return (const GLubyte *)"";
  }
}
