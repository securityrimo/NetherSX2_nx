/* syslang.c -- set the PS2 BIOS language from the Switch system language.
 *
 * Multi-language PS2 games pick their language from the console setting in the BIOS
 * NVRAM (<bios>.nvm): the low 5 bits of the OSD language byte hold it (0=JP 1=EN
 * 2=FR 3=ES 4=DE 5=IT 6=NL 7=PT). It lives in the config1 block -- cdvdReadLanguage-
 * Params reads config1+0xF, so the byte is config1+0x11. On every retail BIOS
 * (>= v1.70, nvmlayouts[1]) config1 = 0x2B0, so the language byte is at file offset
 * 0x2C1 (config1+0x10 = 0x2C0 is the biosLangDefaults block). The NVM is the shared
 * console NVRAM (one per BIOS, not per game), so patching it once localises every
 * game. We write it from the Switch system language (or a launcher choice) before
 * the VM boots. (Single-language discs ignore it -- that is the disc, not this.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>

#include "config.h"
#include "prefs.h"
#include "util.h"
#include "syslang.h"

#define NVM_CFG1      0x2C0  // config1 (0x2B0, BIOS >= v1.70) + 0x10: OSD language block
#define NVM_REGPARAMS 0x180  // PStwo region params: "<RR><lll><RR>" (region + 3-char lang)
#define NVM_SIZE      1024

// PS2 OSD language id -> region-params 3-char language code (ISO-639-2/B, as PS2 uses).
static const char *const REGION_LANG[8] = { "jpn", "eng", "fre", "spa", "ger", "ita", "dut", "por" };

// Switch SetLanguage -> PS2 OSD language id. Unsupported languages -> English.
static int ps2_lang_from_switch(void) {
  if (R_FAILED(setInitialize())) return 1;
  u64 code = 0;
  SetLanguage sl = SetLanguage_ENUS;
  if (R_SUCCEEDED(setGetSystemLanguage(&code)))
    setMakeLanguage(code, &sl);
  setExit();
  switch (sl) {
    case SetLanguage_JA:                          return 0; // Japanese
    case SetLanguage_FR:  case SetLanguage_FRCA:  return 2; // French
    case SetLanguage_ES:  case SetLanguage_ES419: return 3; // Spanish
    case SetLanguage_DE:                          return 4; // German
    case SetLanguage_IT:                          return 5; // Italian
    case SetLanguage_NL:                          return 6; // Dutch
    case SetLanguage_PT:  case SetLanguage_PTBR:  return 7; // Portuguese
    default:                                      return 1; // English + unsupported
  }
}

// Set the console language in the NVRAM. Two independent fields carry it, so we set
// both to match a real localised console:
//   1. OSD language block (config1+0x10): the language byte must carry the 0x20
//      "version" bits (store 0x20|lang; the block follows the biosLangDefaults
//      pattern 0x30/0x70/0x41 so a zeroed/incomplete block becomes valid). This is
//      what sceScfGetLanguage returns.
//   2. Region-params 3-char language code (regparams+2, e.g. "eng"/"fre"): PCSX2
//      defaults Europe to "eng"; a real French console has "fre". Only touched when
//      the region bytes are already set, so we never invent an invalid region.
// Timezone bytes (block[3..4]) are left untouched. Returns 1 if anything changed.
static int set_nvm_lang(unsigned char *nvm, int langid) {
  unsigned char b1[16], br[8];
  memcpy(b1, nvm + NVM_CFG1, 16);
  memcpy(br, nvm + NVM_REGPARAMS, 8);
  nvm[NVM_CFG1 + 0]  = 0x30;
  nvm[NVM_CFG1 + 1]  = 0x20 | (langid & 0x1F);       // version bits + language
  nvm[NVM_CFG1 + 5]  = 0x70;
  nvm[NVM_CFG1 + 15] = (langid == 0) ? 0x30 : 0x41;  // JP vs the rest
  if (nvm[NVM_REGPARAMS] != 0) {                      // region already set -> retag lang
    const char *code = REGION_LANG[langid & 7];
    nvm[NVM_REGPARAMS + 2] = code[0];
    nvm[NVM_REGPARAMS + 3] = code[1];
    nvm[NVM_REGPARAMS + 4] = code[2];
  }
  return memcmp(b1, nvm + NVM_CFG1, 16) != 0 || memcmp(br, nvm + NVM_REGPARAMS, 8) != 0;
}

// Rewrite the language fields in every <bios>.nvm the core has created. A fresh
// install with no NVM yet gets English on its first-ever boot, then the created
// NVM is patched from the next boot on (self-healing, one-time).
static int patch_existing_nvms(int langid) {
  DIR *d = opendir(BIOS_DIR);
  if (!d) return 0;
  int patched = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot || strcasecmp(dot, ".nvm")) continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", BIOS_DIR, e->d_name);
    FILE *f = fopen(path, "r+b");
    if (!f) continue;
    unsigned char nvm[NVM_SIZE];
    if (fread(nvm, 1, NVM_SIZE, f) == NVM_SIZE && set_nvm_lang(nvm, langid) &&
        fseek(f, 0, SEEK_SET) == 0 && fwrite(nvm, 1, NVM_SIZE, f) == NVM_SIZE)
      patched++;
    fclose(f);
  }
  closedir(d);
  return patched;
}

void apply_system_language(void) {
  const char *pref = prefs_get_string("Wrapper/SystemLanguage", "auto");
  if (!strcmp(pref, "off")) return;                 // leave the BIOS NVRAM as-is
  int langid;
  if (!strcmp(pref, "auto")) langid = ps2_lang_from_switch();
  else { langid = atoi(pref); if (langid < 0 || langid > 7) langid = 1; }
  patch_existing_nvms(langid);
}
