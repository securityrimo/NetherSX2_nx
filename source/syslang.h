/* syslang.h -- set the PS2 BIOS language from the Switch system language.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SYSLANG_H__
#define __SYSLANG_H__

// Rewrite the OSD-language byte in the BIOS NVRAM (<bios>.nvm) per the launcher
// option Wrapper/SystemLanguage ("auto" = follow the Switch, "off" = leave as-is,
// else an explicit PS2 language id). Call once before the VM boots.
void apply_system_language(void);

#endif
