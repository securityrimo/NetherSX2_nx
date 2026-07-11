/* patches.c -- binary patches applied to the loaded libemucore.so
 *
 * Runs from main() AFTER so_relocate/so_resolve and BEFORE so_finalize, so the
 * image is still the writable load_base heap mirror (so_finalize maps it RX and
 * so_flush_caches invalidates icache afterwards).
 *
 * --- Signature / anti-tamper bypass ---
 * Java_..._initialize() verifies the APK signing certificate: it derives a hash
 * string from getSigningCertificateHistory()[0].toByteArray() and memchr-scans
 * it for the original signer's marker (two 22-byte compares). A re-signed build
 * (this APK is signed by "Qiqi Mao") fails, so initialize() returns false and
 * nothing boots. We can't forge the original signer, so we neutralize the two
 * compares: each ends with `orr x9,x9,x11` (fold the last XOR bytes into the
 * mismatch accumulator) right before `cbz x9,<match>`. Replacing that orr with
 * `mov x9,xzr` forces x9==0 -> the first memchr candidate "matches" with all
 * registers naturally valid, so execution flows through both stages into the
 * real initialization. Found via capstone disassembly of initialize @0x839c30.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"
#include "../so_util.h"
#include "../prefs.h"

extern so_module emu_mod; // libemucore.so (defined in main.c)

#define INSN_NOP        0xd503201fu // nop
#define INSN_MOV_X9_XZR 0xaa1f03e9u // mov x9, xzr (force the XOR-match accumulator to 0)
#define INSN_MOV_X8_XZR 0xaa1f03e8u // mov x8, xzr (same, for the runVMThread check)

// Each of the two signature stages has three gates that bail to 0x83bc48 (return
// false) when our re-signed cert's hash-string lacks the original marker:
//   cmp x8,#0x16 ; b.lt 0x83bc48     length-of-hash-string < 22  -> NOP
//   bl  memchr   ; cbz x0,0x83bc48   marker first-byte not found -> NOP the call
//                                    (x0 stays = x22, the buffer ptr, so cbz x0 is never taken)
//   ...orr x9,x9,x11 ; cbz x9,<ok>   22-byte mismatch            -> mov x9,xzr (force match)
// Letting the stage CODE run (so its std::string locals construct/destruct
// normally for the epilogue's RAII) while neutralizing the three gates makes the
// first candidate "match" with valid registers, flowing into real init.
//
// --- Class-name integrity gates (stages 3 & 4) ---
// FURTHER down the SAME initialize() there are two more obfuscated checks that
// also bail to 0x83bc48: they build an expected string at runtime (deobfuscated
// byte-subtract, same style as the sig check), fetch a reflected string
// (context.getClass().getName() etc. via GetStringUTFChars), byte-compare them
// and set a match flag (w24 / w23), then `cbz wN,0x83bc48`. On a fake/native
// frontend the context class is "android.content.Context", not the expected app
// class, so w24=0 and init fails. NOP the two `cbz wN` so they always continue as
// if matched (the flag isn't read afterwards). The nearby `cbz x0` gates are
// genuine JNI null/exception checks -- leave them alone.
typedef struct { uint32_t vaddr, expect, insn; const char *what; } Patch;
static const Patch g_patches_4248[] = {
  { 0x83a3d0, 0x5400c3cb, INSN_NOP,        "s1 b.lt(len)" },
  { 0x83a3e8, 0x940c7546, INSN_NOP,        "s1 bl memchr" },
  { 0x83a418, 0xaa0b0129, INSN_MOV_X9_XZR, "s1 force match" },
  { 0x83a774, 0x5400a6ab, INSN_NOP,        "s2 b.lt(len)" },
  { 0x83a78c, 0x940c745d, INSN_NOP,        "s2 bl memchr" },
  { 0x83a7bc, 0xaa0b0129, INSN_MOV_X9_XZR, "s2 force match" },
  { 0x83b8c0, 0x34001c58, INSN_NOP,        "s3 class-name match (cbz w24)" },
  { 0x83bbd0, 0x340003d7, INSN_NOP,        "s4 string match (cbz w23)" },
  // runVMThread() @0x83cc40 repeats the cert-hash marker scan before booting the
  // VM (memchr for the signer marker + 24-byte XOR compare, bailing to 0x83d3d8).
  // Same neutralization as the sig stages: NOP length gate, NOP the memchr call
  // (x0 stays = the hash-string ptr so cbz x0 isn't taken), force the compare to
  // match (mov x8,xzr before `cbz x8,<ok>`). Without this runVMThread returns
  // instantly (no onVMStarting) and the app shuts down.
  { 0x83ce74, 0x54002b2b, INSN_NOP,        "vm b.lt(len)" },
  { 0x83ce8c, 0x940c6a9d, INSN_NOP,        "vm bl memchr" },
  { 0x83ceb8, 0xaa0a0108, INSN_MOV_X8_XZR, "vm force match" },
  // --- GameDB cache version-string fix (boot-time perf, not anti-tamper) ---
  // On boot the core validates gamedb.cache by comparing a 20-byte version tag in
  // the file against the rodata constant "v2.2n-4248 (Patched)" (@0xd4436). The SAVE
  // path (@0x2e3fbc) builds that tag from the rodata's first 16 bytes plus a hardcoded
  // 4-byte immediate w10 = "7d69", so it writes "...(Patc7d69", which never matches ->
  // validation fails and the core reparses GameIndex.yaml (~3.4s) every boot. Rewrite
  // the two immediates so the save emits "hed)" instead of "7d69" -- the tag becomes
  // "v2.2n-4248 (Patched)" and matches, so the cache persists.
  //   mov  w10,#0x6437         -> mov  w10,#0x6568       ("7d"->"eh", low half)
  //   movk w10,#0x3936,lsl #16 -> movk w10,#0x2964,lsl16 ("69"->"d)", high half)
  { 0x2e3fc8, 0x528c86ea, 0x528cad0a, "gamedb cache ver lo" },
  { 0x2e3fcc, 0x72a726ca, 0x72a52c8a, "gamedb cache ver hi" },
  // --- Display centring (fixes the left-anchored FMV / 4:3 content) ---
  // ComputeDrawRectangle @0x61c7a0 aspect-fits then positions by an align arg:
  // 0=left/top (Android default), 1=centre, 2=right/bottom. Left/top left-anchors a
  // narrower-than-window image (4:3 content, or a 4:3 FMV in a widescreen game). The
  // centring math (dim-target)*0.5 is present but gated by a b.ne when align!=1; NOP
  // both so the narrower axis always centres.
  { 0x61c924, 0x54000141, INSN_NOP, "center display X" },
  { 0x61c964, 0x54000221, INSN_NOP, "center display Y" },
  // --- SMC (self-modifying-code) detection fix: black-screen boot on titles that
  // reboot the IOP. The EE recompiler protects compiled pages to discard blocks when
  // the game DMAs new code over them. ProtMode_Write(1) uses mprotect, a silent no-op
  // on our ashmem-backed EE RAM (Horizon can't write-protect it) -> stale code runs
  // after an IOP reboot -> SIF set up wrong -> black screen. Force ProtMode_Manual(2)
  // (inline integrity check) for every in-RAM page: at 0x711180 `ldr w10,[x10,#4]`
  // (PageType) -> `mov w10,#2`; only in-RAM pages (mode 0/1/2) reach here. Launcher
  // toggle Wrapper/EESmcCheck gates it.
  { 0x711180, 0xb940054a, 0x5280004a, "SMC: force ProtMode_Manual" },
};

// Build 3668 ("Classic", "v2.2n-3668 (Classic)"). Same source, different build, so
// all offsets moved. The 11 sig-bypass sites are the same roles/encodings as 4248;
// the two gamedb immediates build the 3668 version tail "sic)" via w11 (not w10).
// RequestStop slot/VM-state differ too (handled below).
static const Patch g_patches_3668[] = {
  { 0x82d0c0, 0x5400c3eb, INSN_NOP,        "s1 b.lt(len)" },
  { 0x82d0d8, 0x940c68aa, INSN_NOP,        "s1 bl memchr" },
  { 0x82d108, 0xaa0b0129, INSN_MOV_X9_XZR, "s1 force match" },
  { 0x82d464, 0x5400a6cb, INSN_NOP,        "s2 b.lt(len)" },
  { 0x82d47c, 0x940c67c1, INSN_NOP,        "s2 bl memchr" },
  { 0x82d4ac, 0xaa0b0129, INSN_MOV_X9_XZR, "s2 force match" },
  { 0x82e5b4, 0x34001c58, INSN_NOP,        "s3 class-name match (cbz w24)" },
  { 0x82e8c4, 0x340003d7, INSN_NOP,        "s4 string match (cbz w23)" },
  { 0x82fb64, 0x54002c2b, INSN_NOP,        "vm b.lt(len)" },
  { 0x82fb7c, 0x940c5e01, INSN_NOP,        "vm bl memchr" },
  { 0x82fba8, 0xaa0a0108, INSN_MOV_X8_XZR, "vm force match" },
  // gamedb tail "sic)" for "v2.2n-3668 (Classic)": mov w11,#0x6973 / movk w11,#0x2963,lsl16
  { 0x2e3378, 0x528c26cb, 0x528d2e6b, "gamedb cache ver lo" },
  { 0x2e337c, 0x72a7068b, 0x72a52c6b, "gamedb cache ver hi" },
  // Display centring -- same fix as 4248 (see that table). ComputeDrawRectangle is
  // at 0x62a0xx here; NOP the two align!=1 centring-skip b.ne (byte-identical
  // encodings, same function layout).
  { 0x62a204, 0x54000141, INSN_NOP, "center display X" },
  { 0x62a244, 0x54000221, INSN_NOP, "center display Y" },
  // SMC detection fix -- same as 4248 (see that table). The PageType load in
  // memory_protect_recompiled_code is at 0x6f6c10 here, byte-identical
  // (`ldr w10,[x10,#4]` -> `mov w10,#2`). Wrapper/EESmcCheck gates it ("SMC:" prefix).
  { 0x6f6c10, 0xb940054a, 0x5280004a, "SMC: force ProtMode_Manual" },
};

// --- VM RequestStop vtable hook ---------------------------------------------
// The core stops the VM via a virtual RequestStop(this) that sets its VM-state
// global to Stopping; the run loop then tears down and returns, so the app would
// exit itself. That method's only stored pointer is a vtable slot (one RELATIVE
// reloc, no direct callers), so repointing it here intercepts every stop. The
// core queues a spurious stop ~150ms into the BIOS boot (before EELOAD) that
// would kill it, so we IGNORE stops until main.c sets g_allow_stop for the real
// shutdown. RequestStop ignores `this` (operates on globals), so forwarding is safe.
static void *g_real_reqstop = 0;
static uintptr_t g_reqstop_slot = 0xb80558;  // per-build (set in patch_game); default 4248
volatile int g_allow_stop = 0;               // main.c sets =1 before the intentional shutdown

static void reqstop_hook(void *thisptr) {
  if (!g_allow_stop) return;                     // ignore the spurious boot-time stop
  ((void (*)(void *))g_real_reqstop)(thisptr);   // real shutdown -> forward
}

CoreVersion g_core_version = CORE_VER_UNKNOWN;

int core_is_3668(void) { return g_core_version == CORE_VER_V22N_3668; }

// Guard raw reads/writes against the mapped image size: Wrapper/CoreSo lets the
// user point at an arbitrary .so, so a wrong/smaller core must not fault us.
static int in_range(uint32_t off, uint32_t sz) {
  return (uint64_t)off + sz <= (uint64_t)emu_mod.load_size;
}

// Identify the build from its embedded version string: 4248 "v2.2n-4248 (Patched)"
// at 0xd4436, 3668 "v2.2n-3668 (Classic)" at 0xff01f. The build selects the patch
// table.
static void detect_core_version(void) {
  g_core_version = CORE_VER_UNKNOWN;
  if (in_range(0xd4436, 24) &&
      !memcmp((const char *)((uintptr_t)emu_mod.load_base + 0xd4436), "v2.2n-4248", 10))
    g_core_version = CORE_VER_V22N_4248;
  else if (in_range(0xff01f, 24) &&
           !memcmp((const char *)((uintptr_t)emu_mod.load_base + 0xff01f), "v2.2n-3668", 10))
    g_core_version = CORE_VER_V22N_3668;
  static const char *names[] = { "UNKNOWN", "v2.2n-4248", "v2.2n-3668" };
  debugPrintf("patch_game: detected core build -> %s\n", names[g_core_version]);
}

void patch_game(void) {
  detect_core_version();

  // Pick the per-build patch table + RequestStop vtable slot. Unknown build falls
  // back to the 4248 table (its expect-guard skips every mismatched site).
  const Patch *tbl; int total;
  if (core_is_3668()) {
    tbl = g_patches_3668; total = (int)(sizeof(g_patches_3668) / sizeof(*g_patches_3668));
    g_reqstop_slot = 0xb6f920;
  } else {
    tbl = g_patches_4248; total = (int)(sizeof(g_patches_4248) / sizeof(*g_patches_4248));
    g_reqstop_slot = 0xb80558;
  }

  // The SMC (self-modifying-code) patch fixes stuck-at-boot games (IOP-reboot titles
  // whose reloaded EE code the recompiler would otherwise run stale) but adds a
  // per-block integrity check that costs some EE speed on every game. Launcher
  // toggle, default ON (compatibility); OFF restores the faster path for titles that
  // don't overwrite EE code via DMA. Per-game overridable like any other option.
  const int smc = prefs_get_bool("Wrapper/EESmcCheck", true);

  int n = 0;
  for (int i = 0; i < total; i++) {
    const Patch *pt = &tbl[i];
    if (!smc && !strncmp(pt->what, "SMC:", 4)) {
      debugPrintf("patch_game: %s @0x%x: SKIPPED (Wrapper/EESmcCheck=off)\n", pt->what, pt->vaddr);
      continue;
    }
    if (!in_range(pt->vaddr, 4)) {
      debugPrintf("patch_game: %s @0x%x: past image (size 0x%x) -- SKIPPED\n",
                  pt->what, pt->vaddr, (unsigned)emu_mod.load_size);
      continue;
    }
    volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)emu_mod.load_base + pt->vaddr);
    if (*p != pt->expect) {
      debugPrintf("patch_game: %s @0x%x: unexpected 0x%08x (wanted 0x%08x) -- SKIPPED\n",
                  pt->what, pt->vaddr, *p, pt->expect);
      continue;
    }
    *p = pt->insn;
    debugPrintf("patch_game: %s @0x%x: 0x%08x -> 0x%08x\n",
                pt->what, pt->vaddr, pt->expect, pt->insn);
    n++;
  }
  debugPrintf("patch_game: %d/%d patches applied (build %s)\n",
              n, total, core_is_3668() ? "3668" : "4248");

  // Install the RequestStop vtable hook (see reqstop_hook above). The slot holds the
  // relocated pointer to RequestStop@0x845300; save it and repoint the slot at our
  // interceptor. Written through the load_base mirror (same physical as the finalized
  // image), so the core sees it at runtime.
  if (!in_range((uint32_t)g_reqstop_slot, 8)) {
    debugPrintf("patch_game: RequestStop vtable slot @0x%lx past image -- SKIPPED\n",
                (unsigned long)g_reqstop_slot);
  } else {
    volatile uintptr_t *slot = (volatile uintptr_t *)((uintptr_t)emu_mod.load_base + g_reqstop_slot);
    g_real_reqstop = (void *)*slot;
    *slot = (uintptr_t)&reqstop_hook;
    debugPrintf("patch_game: RequestStop vtable hook @0x%lx: real=%p -> hook=%p\n",
                (unsigned long)g_reqstop_slot, g_real_reqstop, (void *)&reqstop_hook);
  }

}
