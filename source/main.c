/* main.c
 *
 * NetherSX2 (AetherSX2 / PCSX2-lineage PS2 emulator, Android arm64) on the
 * Nintendo Switch. We load libemucore.so with the so_util ELF loader and drive
 * its NativeLibrary JNI entry points directly from C against a fake JNI env,
 * replacing the Java frontend.
 *
 * Startup mirrors EmulationActivity.onCreate -> startEmulationThread (see the
 * reconstructed call order): JNI_OnLoad, initialize(ctx,dataDir,dev,cacheDir),
 * setInputDevices, then a dedicated thread runs the BLOCKING runVMThread(ctx,
 * bootPath, null) -- which boots the ISO and then waits for a window -- while
 * this thread delivers the window via changeSurface(fakeSurface,w,h,hz) +
 * applySettings(). Input is pumped from the main thread.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "prefs.h"
#include "libc_shim.h"
#include "pthr.h"
#include "keycodes.h"
#include "syslang.h"

// Android MotionEvent axis codes (handleControllerAxisEvent expects these)
#define AAXIS_X         0
#define AAXIS_Y         1
#define AAXIS_Z         11
#define AAXIS_RZ        14
#define AAXIS_LTRIGGER  17
#define AAXIS_RTRIGGER  18
#define AAXIS_HAT_X     15
#define AAXIS_HAT_Y     16

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module emu_mod; // libemucore.so

// Handles passed to the NativeLibrary natives. These MUST be real tagged
// FakeObjects (not magic sentinels): the core dereferences them -- e.g. it
// calls GetObjectClass(context) during initialize, and obj->tag would fault on
// a bare sentinel address. Created from jni_obj_new() after jni_init().
static void *NATIVE_CLASS = NULL; // jclass for the NativeLibrary static natives
static void *FAKE_CONTEXT = NULL; // android.content.Context
static void *FAKE_SURFACE = NULL; // android.view.Surface

#if GS_RENDERER == GS_RENDERER_VK
// NVK opens the GPU (nvInitialize/nvMap) internally during vkCreateInstance and
// needs a larger NV transfer-memory arena than the GL path's libnx default.
// libnx declares this weak; a strong definition here overrides it. Tune on
// device if nvMap/instance creation fails.
u32 __nx_nv_transfermem_size = 16 * 1024 * 1024;
#endif

// separate the newlib heap from the .so load region (see config.h SO_REGION_MB)
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    // Leave GPU_RESERVE_MB unmapped: NVK/nvmap allocates outside this heap, and
    // its VK_EXT_memory_budget report is what VMA trusts. See config.h.
    {
      const size_t gpu_reserve = (size_t)GPU_RESERVE_MB * 1024 * 1024;
      if (size > gpu_reserve + (size_t)512 * 1024 * 1024)
        size = (size - gpu_reserve) & ~0x1FFFFF;
    }
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // newlib heap backs the core's malloc + mesa's host allocations (the nouveau
  // GLSL compiler is hungry) + the GameIndex.yaml parse. Reserve a fixed slice
  // for the loaded .so image and give all the rest to newlib. The PS2 RAM
  // mirrors and the recompiler RX caches are mapped SEPARATELY by the core via
  // mmap()/svcMapProcessCodeMemory (see libc_shim.c), not from this heap.
  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  fake_heap_size = (size > so_reserve + so_reserve / 2) ? (size - so_reserve)
                                                        : (size / 2);

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static int path_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static int dir_nonempty(const char *p) {
  // a usable BIOS dir / resources dir must exist; a deeper content check is the
  // core's job (isBIOSAvailable). Here we only confirm the directory is present.
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void check_data(const char *core_so, const char *disc_path) {
  if (!path_exists(core_so))
    fatal_error("Could not find\n%s.\nPlace it in /switch/nethersx2/\n(extract from the APK lib/arm64-v8a/).", core_so);
  if (!dir_nonempty(RESOURCES_DIR))
    fatal_error("Missing %s.\nCopy the APK assets/ folder there\n(shaders, GameIndex.yaml, fonts).", RESOURCES_DIR);
#if GS_RENDERER == GS_RENDERER_VK
  // The Vulkan renderer loads GLSL from shaders/vulkan/ (compiled to SPIR-V at
  // runtime). Check the dir, not a specific file, so a filename difference
  // across core versions doesn't false-fail a valid setup.
  if (!dir_nonempty(RESOURCES_DIR "/shaders/vulkan"))
    fatal_error("Missing Vulkan shaders in\n%s/shaders/vulkan/.\nCopy the APK assets/shaders/.", RESOURCES_DIR);
#else
  if (!path_exists(RESOURCES_DIR "/shaders/opengl/tfx_fs.glsl"))
    fatal_error("Missing OpenGL shaders in\n%s/shaders/opengl/.\nCopy the APK assets/shaders/.", RESOURCES_DIR);
#endif
  if (!dir_nonempty(BIOS_DIR))
    fatal_error("Missing %s.\nPlace your PS2 BIOS dump there.", BIOS_DIR);
  if (disc_path && *disc_path && !path_exists(disc_path))
    fatal_error("Boot disc not found:\n%s\nSet EmuCore/DiscPath in %s\nor place game.iso in /switch/nethersx2/.",
                disc_path, PREFS_NAME);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      panel_width = 1920; panel_height = 1080;
    } else {
      panel_width = 1280; panel_height = 720;
    }
  } else {
    panel_width = w; panel_height = h;
  }

  // The surface IS the full panel; the core aspect-fits and centres its draw rect
  // within it (via the ComputeDrawRectangle centring patch in hooks/patches.c).
  screen_width = panel_width;
  screen_height = panel_height;
  debugPrintf("screen: panel/surface %dx%d\n", panel_width, panel_height);
}

// ---------------------------------------------------------------------------
// NativeLibrary JNI entry points (resolved from libemucore.so)
// ---------------------------------------------------------------------------

typedef unsigned char jbool;

static struct {
  int  (*JNI_OnLoad)(void *vm, void *reserved);
  jbool(*initialize)(void *env, void *cls, void *ctx, void *dataDir, void *devName, void *cacheDir);
  void (*applySettings)(void *env, void *cls);
  jbool(*isBIOSAvailable)(void *env, void *cls);
  void (*setDefaultPadSettings)(void *env, void *cls);
  void (*setInputDevices)(void *env, void *cls, void *deviceArray);
  void (*changeSurface)(void *env, void *cls, void *surface, int w, int h, float hz);
  void (*runVMThread)(void *env, void *cls, void *ctx, void *bootPath, void *saveStatePath);
  void (*stopVMThreadLoop)(void *env, void *cls, jbool wait);
  void (*pauseVM)(void *env, void *cls, jbool pause);
  void (*resetVM)(void *env, void *cls);
  jbool(*hasValidRenderSurface)(void *env, void *cls);
  void (*setPadValue)(void *env, void *cls, int controller, int bind, float value);
  void (*handleControllerButtonEvent)(void *env, void *cls, int dev, int code, jbool down);
  void (*handleControllerAxisEvent)(void *env, void *cls, int dev, int axis, float value);
  void (*handlePointerEvent)(void *env, void *cls, int id, float x, float y);
} nl;

#define RESOLVE(field, sym) \
  nl.field = (void *)so_find_addr_rx(&emu_mod, sym)
#define NLSYM(name) "Java_xyz_aethersx2_android_NativeLibrary_" name

static void resolve_entry_points(void) {
  nl.JNI_OnLoad = (void *)so_find_addr_rx(&emu_mod, "JNI_OnLoad");
  RESOLVE(initialize,                  NLSYM("initialize"));
  RESOLVE(applySettings,               NLSYM("applySettings"));
  RESOLVE(isBIOSAvailable,             NLSYM("isBIOSAvailable"));
  RESOLVE(setInputDevices,             NLSYM("setInputDevices"));
  RESOLVE(changeSurface,               NLSYM("changeSurface"));
  RESOLVE(runVMThread,                 NLSYM("runVMThread"));
  RESOLVE(stopVMThreadLoop,            NLSYM("stopVMThreadLoop"));
  RESOLVE(pauseVM,                     NLSYM("pauseVM"));
  RESOLVE(resetVM,                     NLSYM("resetVM"));
  RESOLVE(hasValidRenderSurface,       NLSYM("hasValidRenderSurface"));
  // setPadValue drives the emulated DualShock2 DIRECTLY (this is what AetherSX2's
  // on-screen touch controls use), bypassing InputManager's binding layer -- which
  // is empty for us: setDefaultPadSettings only writes Pad1 knobs (Type/Deadzone/
  // AxisScale/...), never any button bindings, so handleControllerButtonEvent
  // events matched nothing and were silently dropped.
  nl.setPadValue = (void *)so_try_find_addr_rx(&emu_mod, NLSYM("setPadValue"));
  RESOLVE(handleControllerButtonEvent, NLSYM("handleControllerButtonEvent"));
  RESOLVE(handleControllerAxisEvent,   NLSYM("handleControllerAxisEvent"));
  RESOLVE(handlePointerEvent,          NLSYM("handlePointerEvent"));
  // setDefaultPadSettings is optional (default controller bindings)
  nl.setDefaultPadSettings = (void *)so_try_find_addr_rx(&emu_mod, NLSYM("setDefaultPadSettings"));
}

// ---------------------------------------------------------------------------
// emulation thread: runVMThread blocks here, booting the disc + running the VM
// ---------------------------------------------------------------------------

static char g_disc_path[1024];
static char g_core_so[256];   // core .so to load (Wrapper/CoreSo, default SO_NAME)
static volatile int g_vm_running = 0;

static void *emu_thread_main(void *arg) {
  (void)arg;
  // this thread runs core code -> it needs the fake stack-guard TLS before any
  // libemucore function reads TPIDR_EL0
  pthr_ensure_fake_tls();
  // This IS the EE/VM thread (runVMThread runs the EE+IOP+VU0 recompilers here,
  // ~99% busy). Give it its own cpu; the MTGS/VU worker threads and audio are
  // pinned to the other cores by pthr.c. Created via real pthread_create, so it
  // never went through the trampoline that pins the emucore workers.
  pthr_pin_ee_core();
  debugPrintf("emu thread: runVMThread(%s)\n", g_disc_path);
  g_vm_running = 1;
  // From here the VM executes recompiled PS2 code; fsync a bit more often (every
  // 16 lines vs 128) so a hang leaves a near-complete tail on disk without the
  // per-line fsync stall. Real crashes are captured by the exception handler.
  g_log_fsync_all = 1;
  // saveStatePath = NULL (fresh boot of the disc)
  nl.runVMThread(fake_env, NATIVE_CLASS, FAKE_CONTEXT,
                 jni_make_string(g_disc_path), NULL);
  g_vm_running = 0;
  debugPrintf("emu thread: runVMThread returned\n");
  return NULL;
}

static pthread_t emu_thread;

// ---------------------------------------------------------------------------
// startup sequence
// ---------------------------------------------------------------------------

// --- HD rumble: driven by the core's setVibratorIntensity JNI callback -------
static HidVibrationDeviceHandle g_vib_no1[2], g_vib_hh[2];
static int g_vib_ready = 0;
static int g_vibration = 1;   // Wrapper/Vibration user toggle

static void rumble_init(void) {
  g_vib_ready = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib_no1, 2, HidNpadIdType_No1,
                    HidNpadStyleSet_NpadStandard)) &&
                R_SUCCEEDED(hidInitializeVibrationDevices(g_vib_hh, 2, HidNpadIdType_Handheld,
                    HidNpadStyleSet_NpadStandard));
}

// large = low-frequency motor, small = high-frequency motor, both 0..1. Sent to
// both the attached-controller and handheld device sets (the inactive one is a
// no-op). Called from jni_fake.c's setVibratorIntensity handler.
void wrapper_rumble(float large, float small) {
  if (!g_vib_ready || !g_vibration) return;
  if (large < 0.f) large = 0.f; else if (large > 1.f) large = 1.f;
  if (small < 0.f) small = 0.f; else if (small > 1.f) small = 1.f;
  HidVibrationValue v[2];
  v[0].amp_low = large; v[0].freq_low = 160.0f;
  v[0].amp_high = small; v[0].freq_high = 320.0f;
  v[1] = v[0];
  hidSendVibrationValues(g_vib_no1, v, 2);
  hidSendVibrationValues(g_vib_hh, v, 2);
}

static void *make_input_devices(void) {
  // one InputDeviceInfo with a non-empty descriptor so the core allocates a pad,
  // plus a single Vibrator (opaque -- the core hands it back to setVibratorIntensity)
  // so the core knows the pad can rumble. SDK 30 -> the vibrators[] path (no manager).
  void *dev = jni_obj_new("xyz/aethersx2/android/NativeLibrary$InputDeviceInfo");
  jni_obj_set_string(dev, "descriptor", "Switch-Pad-0");
  void *vibs = jni_make_object_array(1);
  jni_obj_array_set(vibs, 0, jni_obj_new("android/os/Vibrator"));
  jni_obj_set_object(dev, "vibratorManager", NULL);
  jni_obj_set_object(dev, "vibrators", vibs);
  void *arr = jni_make_object_array(1);
  jni_obj_array_set(arr, 0, dev);
  return arr;
}

extern volatile int g_net_ready;  // imports.c -- gates the DEV9 socket shims

// PS2 network adapter (DEV9, "Sockets" backend). Opt-in via the launcher's
// Network toggle; off by default. The libnx bsd service is brought up only when
// the user enabled it (it reserves a transfer buffer we don't want to pay for
// otherwise); once it is up g_net_ready lets the socket shims through. The DEV9
// keys are always written -- EthEnable=false when off so a stray ini line can't
// silently turn the adapter on.
static void apply_network_settings(void) {
  int on = prefs_get_bool("Wrapper/Network", false);
  if (on && !g_net_ready) {
    if (R_SUCCEEDED(socketInitializeDefault())) {
      g_net_ready = 1;
    } else {
      debugPrintf("net: socketInitializeDefault failed; adapter disabled\n");
      on = 0;
    }
  }
  prefs_set_string("DEV9/Eth/EthEnable",     on ? "true" : "false");
  prefs_set_string("DEV9/Eth/EthApi",        "Sockets");
  prefs_set_string("DEV9/Eth/EthDevice",     "");
  prefs_set_string("DEV9/Eth/InterceptDHCP", "true");
  prefs_set_string("DEV9/Eth/ModeDNS1",      "Auto");
  prefs_set_string("DEV9/Eth/ModeDNS2",      "Auto");
  prefs_set_string("DEV9/Hdd/HddEnable",     "false");
  // Optional custom DNS (e.g. a community revival server); blank => Auto. Manual
  // mode makes the core's internal resolver forward lookups to this address.
  const char *dns = prefs_get_string("Wrapper/NetDNS", "");
  if (on && dns && dns[0]) {
    prefs_set_string("DEV9/Eth/ModeDNS1", "Manual");
    prefs_set_string("DEV9/Eth/DNS1",     dns);
  }
}

static void run_startup_sequence(void) {
  debugPrintf("JNI_OnLoad\n");
  nl.JNI_OnLoad(fake_vm, NULL);

  // Fastmem stays OFF: proven infeasible on Horizon (2026-07-10). Fastmem needs
  // the PS2 RAM both ALIASED into a 4 GB window AND per-page mprotect-able (for
  // self-modifying-code detection); no Horizon primitive gives both -- shared
  // memory aliases but can't be mprotect'd (0xd401), svcMapProcessMemory can
  // mprotect but won't alias any source (0xd401). The software VTLB is slower per
  // access but fully correct.
  prefs_set_bool("EmuCore/CPU/Recompiler/EnableFastmem", false);

  // initialize(ctx, dataDir, cacheDir, deviceName): the 2nd string is the cache
  // dir (the core set EmuFolders::Cache from it), the 3rd is the device name.
  debugPrintf("initialize\n");
  jbool ok = nl.initialize(fake_env, NATIVE_CLASS, FAKE_CONTEXT,
                           jni_make_string(DATA_ROOT),
                           jni_make_string(CACHE_DIR),
                           jni_make_string(ANDROID_DEVICE_NAME));
  if (!ok)
    fatal_error("NativeLibrary.initialize() failed.\nCheck %s and the BIOS/resources.", PREFS_NAME);

  // Force the selected renderer and boot disc, overriding any defaults the core
  // wrote during initialize.
  prefs_set_int("EmuCore/GS/Renderer", GS_RENDERER);
  prefs_set_string("EmuCore/DiscPath", g_disc_path);
  // Fast boot skips the BIOS OSDSYS and launches the disc ELF directly. Launcher
  // toggle (default on); off runs the full BIOS boot, which auto-runs the disc but
  // is slower. Some titles need one or the other, so it is user-selectable.
  prefs_set_string("EmuCore/EnableFastBoot",
                   prefs_get_bool("Wrapper/FastBoot", true) ? "1" : "0");
  // fastCDVD off: it collapses CDVD command timing ("may break games") and can hang
  // the boot in sceCdInit's SIF-RPC bind; disc reads work without it.
  prefs_set_bool("EmuCore/Speedhacks/fastCDVD", false);
  // AspectRatio is a PCSX2 enum NAME; repair an invalid value, else respect the
  // user's. The core reads this pref directly to aspect-fit + centre the display.
  {
    const char *ar = prefs_get_string("EmuCore/GS/AspectRatio", "");
    if (strcmp(ar, "Stretch") && strcmp(ar, "Auto 4:3/3:2") && strcmp(ar, "4:3") &&
        strcmp(ar, "16:9"))
      prefs_set_string("EmuCore/GS/AspectRatio", "4:3");
  }
  // Neutral display geometry -- the renderer handles centring itself.
  prefs_set_bool("EmuCore/GS/pcrtc_offsets", false);
  prefs_set_bool("EmuCore/GS/pcrtc_overscan", false);
  prefs_set_bool("EmuCore/GS/IntegerScaling", false);
  prefs_set_int("EmuCore/GS/CropLeft", 0);
  prefs_set_int("EmuCore/GS/CropTop", 0);
  prefs_set_int("EmuCore/GS/CropRight", 0);
  prefs_set_int("EmuCore/GS/CropBottom", 0);
  prefs_set_int("EmuCore/GS/StretchY", 100);
  // PS2 network adapter: bring up sockets (if enabled) and seed the DEV9 keys
  // before the VM thread boots and initialises DEV9.
  apply_network_settings();
  // Core logging off: the EE/IOP console formats a lot of strings per frame.
  prefs_set_string("Logging/EnableSystemConsole", "0");
  prefs_set_string("Logging/EnableFileLogging", "0");
  prefs_set_string("Logging/EnableVerbose", "0");
  prefs_set_string("Logging/EnableEEConsole", "0");
  prefs_set_string("Logging/EnableIOPConsole", "0");
  prefs_save();

  if (!nl.isBIOSAvailable(fake_env, NATIVE_CLASS))
    fatal_error("No usable PS2 BIOS found in\n%s.", BIOS_DIR);

  // Set the PS2 OSD language in the BIOS NVRAM from the Switch system language
  // (before the VM boots and reads it), so games that follow the console language
  // come up localised instead of always English.
  apply_system_language();

  // ORDER MATTERS. setDefaultPadSettings generates the default bindings FOR THE
  // REGISTERED INPUT DEVICES: it clears InputSources + Pad1..Pad8 and rebinds. Run
  // before setInputDevices it binds to an empty device list, so Pad1 ends up with NO
  // bindings and the controller does nothing in-game. Register the device first, then
  // generate defaults, then apply.
  debugPrintf("setInputDevices\n");
  nl.setInputDevices(fake_env, NATIVE_CLASS, make_input_devices());

  if (nl.setDefaultPadSettings)
    nl.setDefaultPadSettings(fake_env, NATIVE_CLASS);

  // Rumble is dispatched by AndroidInputSource::UpdateMotorState only when Pad1's
  // motor outputs are bound to a vibration target ("<device>/Vibrator<N>"). We drive
  // the pad via setPadValue, so setDefaultPadSettings leaves the motors unbound; bind
  // them to our device (named by the descriptor from make_input_devices).
  prefs_set_string("Pad1/LargeMotor", "Switch-Pad-0/Vibrator0");
  prefs_set_string("Pad1/SmallMotor", "Switch-Pad-0/Vibrator0");
  prefs_set_string("Pad1/LargeMotorScale", "1.0");
  prefs_set_string("Pad1/SmallMotorScale", "1.0");

  debugPrintf("applySettings\n");
  nl.applySettings(fake_env, NATIVE_CLASS);

  // spawn the VM thread; it boots the disc then blocks waiting for the window
  debugPrintf("spawn emu thread\n");
  pthread_create(&emu_thread, NULL, emu_thread_main, NULL);

  // give runVMThread a moment to reach its "waiting for window" point, then
  // hand it the render surface (this unblocks the GS thread's GL bring-up)
  svcSleepThread(250000000ull); // 250 ms
  debugPrintf("changeSurface %dx%d\n", screen_width, screen_height);
  nl.changeSurface(fake_env, NATIVE_CLASS, FAKE_SURFACE,
                   screen_width, screen_height, 60.0f);
  nl.applySettings(fake_env, NATIVE_CLASS);

  // the surface bring-up above ran on THIS thread; if it touched GL, hand the
  // single context back so the GS thread can own it
  egl_gl_ownership_release();
  debugPrintf("startup sequence complete\n");
}

// ---------------------------------------------------------------------------
// input pump (main thread)
// ---------------------------------------------------------------------------

typedef struct { u64 hid; int keycode; } PadMap;

// positional mapping: Switch bottom face button (A in Switch layout) -> PS2
// cross. Switch A=right -> PS2 circle, etc. Android keycodes; the core's pad
// binding layer maps them to PS2 buttons (default bindings via setDefaultPadSettings).
static const PadMap pad_map[] = {
  { HidNpadButton_B,      AKEYCODE_BUTTON_A },  // Switch B (down)  -> A (cross)
  { HidNpadButton_A,      AKEYCODE_BUTTON_B },  // Switch A (right) -> B (circle)
  { HidNpadButton_Y,      AKEYCODE_BUTTON_X },  // Switch Y (left)  -> X (square)
  { HidNpadButton_X,      AKEYCODE_BUTTON_Y },  // Switch X (up)    -> Y (triangle)
  { HidNpadButton_L,      AKEYCODE_BUTTON_L1 },
  { HidNpadButton_R,      AKEYCODE_BUTTON_R1 },
  { HidNpadButton_ZL,     AKEYCODE_BUTTON_L2 },
  { HidNpadButton_ZR,     AKEYCODE_BUTTON_R2 },
  { HidNpadButton_StickL, AKEYCODE_BUTTON_THUMBL },
  { HidNpadButton_StickR, AKEYCODE_BUTTON_THUMBR },
  { HidNpadButton_Plus,   AKEYCODE_BUTTON_START },
  { HidNpadButton_Minus,  AKEYCODE_BUTTON_SELECT },
  { HidNpadButton_Up,     AKEYCODE_DPAD_UP },
  { HidNpadButton_Down,   AKEYCODE_DPAD_DOWN },
  { HidNpadButton_Left,   AKEYCODE_DPAD_LEFT },
  { HidNpadButton_Right,  AKEYCODE_DPAD_RIGHT },
};

static PadState pad;
static u64 pad_prev = 0;

// PS2 DualShock2 bind indices, recovered from libemucore.so's InputBindingInfo
// array (0x75d88, stride 0x30). This is the `bind` argument of setPadValue().
enum {
  PB_UP = 0, PB_RIGHT, PB_DOWN, PB_LEFT,
  PB_TRIANGLE, PB_CIRCLE, PB_CROSS, PB_SQUARE,
  PB_SELECT, PB_START,
  PB_L1, PB_L2, PB_R1, PB_R2, PB_L3, PB_R3,
  PB_ANALOG, PB_PRESSURE,
  PB_LUP, PB_LRIGHT, PB_LDOWN, PB_LLEFT,
  PB_RUP, PB_RRIGHT, PB_RDOWN, PB_RLEFT,
};

// ---------------------------------------------------------------------------
// Controller mapping. The core has no Android-side InputManager binding table
// (the "Pad1"/"SDL-"/"MapController" machinery is absent -- confirmed by RE), so
// input is driven DIRECTLY through setPadValue. Full user rebinding is therefore
// done in the wrapper: each PS2 target reads a Switch-button/stick TOKEN from
// nethersx2.ini ("Wrapper/Pad1/<name>"), written by the SDL launcher. Defaults
// reproduce the previous hardcoded Nintendo-face layout (A=Circle, B=Cross...).
// ---------------------------------------------------------------------------

// Switch button name token -> HID mask (case-insensitive; "None"/"" = unbound).
static const struct { const char *tok; u64 hid; } hid_tokens[] = {
  { "A", HidNpadButton_A },     { "B", HidNpadButton_B },
  { "X", HidNpadButton_X },     { "Y", HidNpadButton_Y },
  { "L", HidNpadButton_L },     { "R", HidNpadButton_R },
  { "ZL", HidNpadButton_ZL },   { "ZR", HidNpadButton_ZR },
  { "Plus", HidNpadButton_Plus }, { "Minus", HidNpadButton_Minus },
  { "StickL", HidNpadButton_StickL }, { "StickR", HidNpadButton_StickR },
  { "Up", HidNpadButton_Up },   { "Down", HidNpadButton_Down },
  { "Left", HidNpadButton_Left }, { "Right", HidNpadButton_Right },
};

// 16 PS2 digital targets in ini order: { ini key, PB_* bind, default Switch token }.
static const struct { const char *key; int bind; const char *def; } ps2_buttons[] = {
  { "Up", PB_UP, "Up" },         { "Down", PB_DOWN, "Down" },
  { "Left", PB_LEFT, "Left" },   { "Right", PB_RIGHT, "Right" },
  { "Triangle", PB_TRIANGLE, "X" }, { "Circle", PB_CIRCLE, "A" },
  { "Cross", PB_CROSS, "B" },    { "Square", PB_SQUARE, "Y" },
  { "L1", PB_L1, "L" },          { "R1", PB_R1, "R" },
  { "L2", PB_L2, "ZL" },         { "R2", PB_R2, "ZR" },
  { "Select", PB_SELECT, "Minus" }, { "Start", PB_START, "Plus" },
  { "L3", PB_L3, "StickL" },     { "R3", PB_R3, "StickR" },
};
#define NUM_PS2_BUTTONS (sizeof(ps2_buttons) / sizeof(*ps2_buttons))

// runtime binding table + stick config, populated by pad_load_bindings().
static struct { u64 hid; int bind; } pad_binds[NUM_PS2_BUTTONS];
static int pad_binds_count = 0;
typedef struct { int src; int invX, invY; } PadStick; // src: 0=LStick 1=RStick -1=none
static PadStick stick_l = { 0, 0, 0 };
static PadStick stick_r = { 1, 0, 0 };
static float stick_deadzone = 0.f; // radial deadzone 0..1 (Wrapper/Pad1/Deadzone %)

static u64 tok_to_hid(const char *tok) {
  if (!tok || !*tok || !strcasecmp(tok, "None")) return 0;
  for (unsigned i = 0; i < sizeof(hid_tokens) / sizeof(*hid_tokens); i++)
    if (!strcasecmp(tok, hid_tokens[i].tok)) return hid_tokens[i].hid;
  return 0; // unknown token -> unbound
}

static int stick_src_from_tok(const char *tok) {
  if (tok && !strcasecmp(tok, "LStick")) return 0;
  if (tok && !strcasecmp(tok, "RStick")) return 1;
  return -1; // None / unknown
}

// Read the Wrapper/Pad1/* bindings from prefs into pad_binds + stick_l/r.
// Called once after prefs_init; defaults reproduce the classic layout so an
// unconfigured install behaves exactly as before.
static void pad_load_bindings(void) {
  pad_binds_count = 0;
  for (unsigned i = 0; i < NUM_PS2_BUTTONS; i++) {
    char key[64];
    snprintf(key, sizeof(key), "Wrapper/Pad1/%s", ps2_buttons[i].key);
    const u64 hid = tok_to_hid(prefs_get_string(key, ps2_buttons[i].def));
    if (hid) {
      pad_binds[pad_binds_count].hid = hid;
      pad_binds[pad_binds_count].bind = ps2_buttons[i].bind;
      pad_binds_count++;
    }
  }
  stick_l.src  = stick_src_from_tok(prefs_get_string("Wrapper/Pad1/LeftStick", "LStick"));
  stick_l.invX = prefs_get_bool("Wrapper/Pad1/LeftStickInvertX", false);
  stick_l.invY = prefs_get_bool("Wrapper/Pad1/LeftStickInvertY", false);
  stick_r.src  = stick_src_from_tok(prefs_get_string("Wrapper/Pad1/RightStick", "RStick"));
  stick_r.invX = prefs_get_bool("Wrapper/Pad1/RightStickInvertX", false);
  stick_r.invY = prefs_get_bool("Wrapper/Pad1/RightStickInvertY", false);
  { int dz = prefs_get_int("Wrapper/Pad1/Deadzone", 0); if (dz < 0) dz = 0; if (dz > 90) dz = 90;
    stick_deadzone = (float)dz / 100.0f; }
  g_vibration = prefs_get_bool("Wrapper/Vibration", true);
  debugPrintf("pad: %d button bindings loaded; sticks L=%d(inv %d,%d) R=%d(inv %d,%d)\n",
              pad_binds_count, stick_l.src, stick_l.invX, stick_l.invY,
              stick_r.src, stick_r.invX, stick_r.invY);
}

// PCSX2 models each analog stick as four HALF-axis binds, so a stick axis is
// split into its two directions (only one is ever non-zero).
static void pad_axis(int neg_bind, int pos_bind, float v) {
  nl.setPadValue(fake_env, NATIVE_CLASS, 0, pos_bind, v > 0.f ? v : 0.f);
  nl.setPadValue(fake_env, NATIVE_CLASS, 0, neg_bind, v < 0.f ? -v : 0.f);
}

// Feed one PS2 analog stick from a configured physical Switch stick. The
// baseline (src=matching stick, no invert) reproduces the old fixed mapping:
// x right-positive -> pos_x bind, y up-positive -> pos_y bind.
static void apply_ps2_stick(const PadStick *c, HidAnalogStickState ls, HidAnalogStickState rs,
                            int negX, int posX, int negY, int posY) {
  if (c->src < 0) { pad_axis(negX, posX, 0.f); pad_axis(negY, posY, 0.f); return; }
  const HidAnalogStickState s = (c->src == 0) ? ls : rs;
  const float scale = 1.f / 32767.0f;
  float x = (float)s.x * scale, y = (float)s.y * scale;
  if (stick_deadzone > 0.f) { // radial deadzone, rescaled so the edge maps to 0
    float mag = sqrtf(x * x + y * y);
    if (mag <= stick_deadzone) { x = 0.f; y = 0.f; }
    else { float k = (mag - stick_deadzone) / ((1.f - stick_deadzone) * mag); x *= k; y *= k; }
  }
  if (c->invX) x = -x;
  if (c->invY) y = -y;
  pad_axis(negX, posX, x);
  pad_axis(negY, posY, y);
}

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 changed = down ^ pad_prev;

  if (nl.setPadValue) {
    // Drive the DualShock2 directly -- no InputManager bindings involved.
    // pad_binds[] + stick_l/r come from Wrapper/Pad1/* via pad_load_bindings().
    for (int i = 0; i < pad_binds_count; i++) {
      if (!(changed & pad_binds[i].hid)) continue;
      const int pressed = (down & pad_binds[i].hid) ? 1 : 0;
      nl.setPadValue(fake_env, NATIVE_CLASS, 0, pad_binds[i].bind, pressed ? 1.0f : 0.0f);
    }
    const HidAnalogStickState ls = padGetStickPos(&pad, 0);
    const HidAnalogStickState rs = padGetStickPos(&pad, 1);
    apply_ps2_stick(&stick_l, ls, rs, PB_LLEFT, PB_LRIGHT, PB_LDOWN, PB_LUP);
    apply_ps2_stick(&stick_r, ls, rs, PB_RLEFT, PB_RRIGHT, PB_RDOWN, PB_RUP);
  } else {
    // Fallback: the Android keycode path (needs InputManager bindings to exist).
    for (unsigned i = 0; i < sizeof(pad_map) / sizeof(*pad_map); i++) {
      if (changed & pad_map[i].hid)
        nl.handleControllerButtonEvent(fake_env, NATIVE_CLASS, 0, pad_map[i].keycode,
                                       (down & pad_map[i].hid) ? 1 : 0);
    }
    const float scale = 1.f / 32767.0f;
    const HidAnalogStickState ls = padGetStickPos(&pad, 0);
    const HidAnalogStickState rs = padGetStickPos(&pad, 1);
    nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, 0, AAXIS_X,  (float)ls.x *  scale);
    nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, 0, AAXIS_Y,  (float)ls.y * -scale);
    nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, 0, AAXIS_Z,  (float)rs.x *  scale);
    nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, 0, AAXIS_RZ, (float)rs.y * -scale);
    nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, 0, AAXIS_LTRIGGER, (down & HidNpadButton_ZL) ? 1.f : 0.f);
    nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, 0, AAXIS_RTRIGGER, (down & HidNpadButton_ZR) ? 1.f : 0.f);
  }
  pad_prev = down;
}

#define MAX_TOUCHES 8
typedef struct { int active; u32 finger_id; float x, y; } TouchSlot;
static TouchSlot touch_prev[MAX_TOUCHES];

static int touch_slot_find(u32 id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_prev[i].active && touch_prev[i].finger_id == id) return i;
  return -1;
}
static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_prev[i].active) return i;
  return -1;
}

static void update_touch(void) {
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;
  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)t->y * sy;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0) continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
    }
    nl.handlePointerEvent(fake_env, NATIVE_CLASS, slot, x, y);
    touch_prev[slot].x = x; touch_prev[slot].y = y;
    seen[slot] = 1;
  }
  for (int slot = 0; slot < MAX_TOUCHES; slot++) {
    if (touch_prev[slot].active && !seen[slot]) {
      // pointer up: send an off-screen position (the core releases on no-move)
      nl.handlePointerEvent(fake_env, NATIVE_CLASS, slot, -1.0f, -1.0f);
      touch_prev[slot].active = 0;
    }
  }
}

int main(void) {
  debugPrintf("==== NetherSX2_nx main() entered ====\n");
  {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    debugPrintf("applet mode: %d, mem total %llu MB, used %llu MB\n",
                (int)appletGetOperationMode(),
                (unsigned long long)(total / (1024 * 1024)),
                (unsigned long long)(used / (1024 * 1024)));
  }

  cpu_boost(1);

  // settings store: load nethersx2.ini + seed OpenGL/folder defaults
  prefs_init(PREFS_PATH);
  pad_load_bindings(); // controller map from Wrapper/Pad1/* (launcher-written)
  debugPrintf("prefs initialized\n");
  // boot disc: prefs EmuCore/DiscPath, else the default game.iso
  snprintf(g_disc_path, sizeof(g_disc_path), "%s",
           prefs_get_string("EmuCore/DiscPath", DEFAULT_DISC_PATH));
  prefs_set_disc_path(g_disc_path);

  // core .so to load. The SDL launcher writes Wrapper/CoreSo to pick the build
  // (4248 "Patched" vs 3668 "Classic"); default is the bundled name. patch_game()
  // version-detects and applies the matching offset table.
  snprintf(g_core_so, sizeof(g_core_so), "%s",
           prefs_get_string("Wrapper/CoreSo", SO_NAME));

  check_syscalls();
  check_data(g_core_so, g_disc_path);
  set_screen_size(0, 0);   // auto: 720p handheld / 1080p docked (see set_screen_size)

  extern char *fake_heap_start;
  const unsigned heap_mb =
      (unsigned)(((char *)heap_so_base - fake_heap_start) / (1024 * 1024));
  debugPrintf("heap: newlib %u MB, lib base %p, lib region %u MB\n",
              heap_mb, heap_so_base, (unsigned)(heap_so_limit / (1024 * 1024)));

  // a PS2 emulator (core + JIT caches + PS2 RAM + mesa) needs the full game
  // memory pool; launched as an applet we only get ~0.5 GB and would OOM mid-boot
  if (heap_mb < 1500)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Launch hbmenu over a game (hold R while\n"
                "starting any installed title), then start\n"
                "NetherSX2 from there.", heap_mb);

  debugPrintf("so_load(%s)...\n", g_core_so);
  if (so_load(&emu_mod, g_core_so, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", g_core_so);
  debugPrintf("so_load ok: base=%p virtbase=%p size=%u KB\n",
              emu_mod.load_base, emu_mod.load_virtbase,
              (unsigned)(emu_mod.load_size / 1024));

  update_imports();
  debugPrintf("so_relocate...\n");
  so_relocate(&emu_mod);
  debugPrintf("so_resolve (%u imports, taint=1)...\n", (unsigned)dynlib_numfunctions);
  so_resolve(&emu_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  resolve_entry_points();
  debugPrintf("entry points resolved (initialize=%p runVMThread=%p)\n",
              (void *)nl.initialize, (void *)nl.runVMThread);

  so_finalize(&emu_mod);
  so_flush_caches(&emu_mod);

  // fake TLS for THIS thread before any core code (init_array C++ ctors read
  // the stack-guard cookie from TPIDR_EL0)
  pthr_install_fake_tls();

  debugPrintf("so_execute_init_array (C++ ctors)...\n");
  so_execute_init_array(&emu_mod);
  so_free_temp(&emu_mod);
  debugPrintf("init_array done\n");

  jni_init();
  // real fake-objects for the class/context/surface the core dereferences
  NATIVE_CLASS = jni_obj_new("xyz/aethersx2/android/NativeLibrary");
  FAKE_CONTEXT = jni_obj_new("android/content/Context");
  FAKE_SURFACE = jni_obj_new("android/view/Surface");
  run_startup_sequence();

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  rumble_init();

  // Present counter for the boot-done / CPU-boost-off trigger. GL bumps
  // egl_swap_count (hooks/egl.c); Vulkan bumps vk_present_count (hooks/vk.c)
  // since the VK path never calls eglSwapBuffers.
#if GS_RENDERER == GS_RENDERER_VK
  extern volatile int vk_present_count;
#define FRAME_COUNT vk_present_count
#else
  extern volatile int egl_swap_count;
#define FRAME_COUNT egl_swap_count
#endif
  u64 ticks = 0;
  int boosting = 1;

  // the input pump wakes ~120x/s; keep it off the EE core so it never preempts
  // the hot recompiler thread (it's a background-class thread once booted).
  pthr_pin_bg_core();

  while (appletMainLoop()) {
    ++ticks;
    update_gamepad();
    update_touch();
    svcSleepThread(1000000000ull / 120); // ~120 Hz input polling
    egl_gl_service_handover();

    if (boosting && FRAME_COUNT >= 300) {   // boot done: drop the load-time CPU boost
      cpu_boost(0);
      boosting = 0;
    }

    if (!g_vm_running && ticks > 240)       // VM thread exited -> shut down
      break;
  }

  // Allow RequestStop to actually stop now: our reqstop_hook (hooks/patches.c)
  // ignores the core's spurious boot-time stop, so the intentional shutdown must
  // re-enable it or stopVMThreadLoop would be swallowed and the join would hang.
  { extern volatile int g_allow_stop; g_allow_stop = 1; }
  nl.stopVMThreadLoop(fake_env, NATIVE_CLASS, 1);
  pthread_join(emu_thread, NULL);
  prefs_save();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
