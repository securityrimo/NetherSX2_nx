/* dl_emu.c -- dlopen/dlsym/dlclose/dlerror/dladdr over the so_util loader.
 *
 * libemucore.so imports dlopen/dlsym/dlclose: it dlopen()s host libraries
 * (notably libEGL.so) and dlsym()s entry points (e.g. eglGetProcAddress), and
 * occasionally probes optional symbols via dlsym(RTLD_DEFAULT, name). We don't
 * track per-library handles -- every dlsym resolves against the SAME two tables
 * the loader already uses: the host import table (dynlib_functions, which routes
 * eglGetProcAddress -> our hook) and the loaded module's own exports. So
 * dlopen() hands out a single non-NULL sentinel handle and dlsym ignores it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_util.h"
#include "dl_emu.h"
#include "util.h"
#include "imports.h"

// the loaded core module (defined in main.c)
extern so_module emu_mod;

#define HANDLE_DEFAULT ((void *)0x444C4F50) // "DLOP": the one pseudo-handle

static char dl_err[256];
static int dl_err_set = 0;

void *dlopen_fake(const char *filename, int flags) {
  (void)flags;

  dl_err_set = 0;
  // any name (libEGL.so, libaaudio.so, self) -> the same pseudo handle; dlsym
  // searches the import table + module exports regardless of which lib was asked
  return HANDLE_DEFAULT;
}

void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol)
    return NULL;

  // 1) host import table (libc/EGL/AAudio/...); routes eglGetProcAddress -> hook
  DynLibFunction *f = so_find_import(dynlib_functions, (int)dynlib_numfunctions, symbol);
  if (f) {
    dl_err_set = 0;
    return (void *)f->func;
  }

  // 2) the core module's own exported symbols
  uintptr_t addr = so_try_find_addr_rx(&emu_mod, symbol);
  if (addr) {
    dl_err_set = 0;
    return (void *)addr;
  }

  snprintf(dl_err, sizeof(dl_err), "undefined symbol: %s", symbol);
  dl_err_set = 1;

  return NULL;
}

int dlclose_fake(void *handle) {
  (void)handle;
  return 0;
}
