/* dl_emu.h -- dlopen/dlsym/dlclose/dlerror/dladdr over the so_util loader
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __DL_EMU_H__
#define __DL_EMU_H__

void *dlopen_fake(const char *filename, int flags);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);

#endif
