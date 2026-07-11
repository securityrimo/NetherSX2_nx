/* hooks.h -- EGL->mesa hook layer + game patches for NetherSX2_nx
 *
 * Unlike lswtcs, libemucore.so has NO DT_NEEDED on libGLESv2: it loads every GL
 * entry point dynamically through eglGetProcAddress (GLAD). So we do NOT wrap the
 * gl* calls -- eglGetProcAddressHook just returns mesa's real pointers. Only the
 * thin EGL layer (config/surface/context bring-up fixes) is hooked, plus a
 * glGetString NULL-guard.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HOOKS_H__
#define __HOOKS_H__

#include <EGL/egl.h>
#include <GLES2/gl2.h>

// applies binary patches to the loaded libemucore.so (none needed initially;
// a hook point for neutralizing any Android-specific path found on-device)
void patch_game(void);

// Detected libemucore.so build. Two builds are supported, each with its own patch-
// offset table (patch_game picks by build): 4248 ("v2.2n-4248 (Patched)") and 3668
// ("v2.2n-3668 (Classic)"). Detected in patch_game() from the version strings at
// 0xd4436 (4248) / 0xff01f (3668).
typedef enum {
  CORE_VER_UNKNOWN = 0,
  CORE_VER_V22N_4248,
  CORE_VER_V22N_3668
} CoreVersion;
extern CoreVersion g_core_version;
int core_is_3668(void);  // build 3668 ("Classic")

// hooks/egl.c -- single-context GL ownership handover. The FIRST EGL bring-up
// may run on the wrapper main thread (during changeSurface); after that the GS
// (SysMtgsThread) thread owns the one real context for life. These are cheap
// no-ops in steady state but cover the one-shot main->GS handover.
void egl_gl_ownership_park(void);     // before a thread blocks (from pthr wait shims)
void egl_gl_ownership_release(void);  // unconditional release (end of main bring-up)
void egl_gl_service_handover(void);   // release if a waiter asked (main loop)
int  egl_gl_thread_holds_context(void);

// EGL hooks (hooks/egl.c) -- registered in imports.c in place of the real eglXxx
EGLDisplay eglGetDisplayHook(EGLNativeDisplayType display_id);
EGLBoolean eglInitializeHook(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglBindAPIHook(EGLenum api);
const char *eglQueryStringHook(EGLDisplay dpy, EGLint name);
EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list);
EGLSurface eglCreateWindowSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                      EGLNativeWindowType win, const EGLint *attrib_list);
EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list);
EGLBoolean eglDestroySurfaceHook(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglDestroyContextHook(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
EGLBoolean eglSwapBuffersHook(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval);
const GLubyte *glGetStringHook(GLenum name);
void *eglGetProcAddressHook(const char *name);

#endif
