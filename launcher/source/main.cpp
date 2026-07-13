/* NetherSX2 launcher -- SDL2 cover-art frontend for the Switch so-loader port.
 *
 * The single .nro the user runs. Bundles both emulator cores AND both renderer
 * binaries in its romfs; scans a game folder and shows a cover grid; edits the
 * shared nethersx2.ini (global + per-game overrides); can fetch covers from
 * SteamGridDB; then extracts the chosen core + renderer to SD and chainloads.
 *
 * MIT license.
 */
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>

#include "griddb.h"
#include "forwarder.h"

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------
// Switch face buttons vs SDL's Xbox-layout naming: Nintendo A (right) = SDL B,
// Nintendo B (bottom) = SDL A, Nintendo X (top) = SDL Y. UI code uses these so
// the on-screen A/B/X match the physical buttons.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B  // Nintendo A
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A  // Nintendo B
#define BTN_SETTINGS SDL_CONTROLLER_BUTTON_Y  // Nintendo X

static const char *DATA_DIR    = "sdmc:/switch/nethersx2";
static const char *LAUNCHER_INI= "sdmc:/switch/nethersx2/launcher.ini"; // launcher's canonical global (emulator never touches it)
static const char *EMU_INI     = "sdmc:/switch/nethersx2/nethersx2.ini"; // written fresh each launch for the emulator
static const char *COVERS_DIR = "sdmc:/switch/nethersx2/covers";
static const char *CORES_DIR  = "sdmc:/switch/nethersx2/cores";
static const char *GAMECFG_DIR= "sdmc:/switch/nethersx2/gamecfg";
static const char *DEF_GAMEDIR= "sdmc:/switch/nethersx2/games";
static const char *BIOS_DIR   = "sdmc:/switch/nethersx2/bios";      // user-supplied PS2 BIOS
static const char *RESOURCES_DIR = "sdmc:/switch/nethersx2/resources"; // per-core, auto-extracted

// ---------------------------------------------------------------------------
// flat "Section/Key = value" ini store (matches the port's prefs.c parser).
// Two live stores: the global nethersx2.ini and, when editing a game, its
// per-game override. The active store is what the settings screens read/write.
// ---------------------------------------------------------------------------
struct KV { std::string k, v; };
struct Store { std::vector<KV> kv; std::string path; };

static Store g_global;   // launcher.ini
static Store g_game;     // gamecfg/<key>.ini (per-game overrides), or empty
static Store g_titles;   // titles.ini: game key -> user-renamed display title
static Store *g_active = &g_global;
static const char *TITLES_INI = "sdmc:/switch/nethersx2/titles.ini";

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
static const char *storeGet(Store &s, const char *key, const char *def) {
  for (auto &e : s.kv) if (e.k == key) return e.v.c_str();
  return def;
}
static void storeSet(Store &s, const char *key, const char *val) {
  for (auto &e : s.kv) if (e.k == key) { e.v = val; return; }
  s.kv.push_back({ key, val });
}
static void storeRemove(Store &s, const char *key) {
  for (size_t i = 0; i < s.kv.size(); i++) if (s.kv[i].k == key) { s.kv.erase(s.kv.begin()+i); return; }
}
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  s.path = path;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof(line), f)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (!k.empty()) s.kv.push_back({ k, v });
  }
  fclose(f);
}
static void storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "# NetherSX2 -- written by the launcher\n");
  for (auto &e : s.kv) fprintf(f, "%s = %s\n", e.k.c_str(), e.v.c_str());
  fclose(f);
}

// active-store accessors used by the settings UI. For the per-game store, an
// un-overridden key falls back to the GLOBAL value (so per-game screens show the
// effective value, and only user-changed keys become actual overrides).
static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game) {
    for (auto &e : g_game.kv)   if (e.k == key) return e.v.c_str();
    for (auto &e : g_global.kv) if (e.k == key) return e.v.c_str();
    return def;
  }
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

// ---------------------------------------------------------------------------
// settings model (data-driven; one renderer drives every settings screen)
// ---------------------------------------------------------------------------
enum OType { OT_CHOICE, OT_RANGE, OT_SUBMENU, OT_TEXT };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
  int sub;      // OT_SUBMENU target screen
  const char *gateKey;  // if set, this row is greyed + non-adjustable while
  const char *gateOff;  // iniGet(gateKey) == gateOff (e.g. its parent toggle is off)
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d, 0, nullptr, nullptr }
#define O_SUB(l,scr)           { l, nullptr, OT_SUBMENU, nullptr,0, 0,0,0, nullptr, scr, nullptr, nullptr }
#define O_CHOICEG(l,k,c,d,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go }
#define O_RANGEG(l,k,lo,hi,s,d,gk,go) { l, k, OT_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go }
#define O_TEXT(l,k,d)          { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, nullptr, nullptr }
#define O_TEXTG(l,k,d,gk,go)   { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, gk, go }

static const Choice C_backend[]  = { {"Vulkan (NVK)","14"}, {"OpenGL","12"} };
static const Choice C_build[]    = { {"Patched (4248)","4248"}, {"Classic (3668)","3668"} };
// AetherSX2's core (2022 PCSX2) clamps upscale_multiplier to an INTEGER 1-8; it has
// no fractional/float upscaling (that landed in mainline PCSX2 in 2024), so a "1.5x"
// would just truncate to 1x. Integer steps only.
static const Choice C_upscale[]  = { {"1x (native ~480p)","1"},{"2x (~720p)","2"},{"3x (~1080p)","3"},
                                     {"4x (~1440p)","4"},{"5x (~1800p)","5"},{"6x (4K ~2160p)","6"} };
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_aspect[]   = { {"4:3","4:3"}, {"16:9","16:9"}, {"Stretch","Stretch"}, {"Auto","Auto 4:3/3:2"} };
static const Choice C_fmvasp[]   = { {"Off","Off"}, {"4:3","4:3"}, {"16:9","16:9"} };
static const Choice C_vsync[]    = { {"Off","0"}, {"On","1"}, {"Adaptive","2"} };
static const Choice C_filter[]   = { {"Nearest","0"}, {"Bilinear (PS2)","2"}, {"Bilinear (Forced)","1"}, {"Bilinear no-sprite","3"} };
static const Choice C_aniso[]    = { {"Off","0"}, {"2x","2"}, {"4x","4"}, {"8x","8"}, {"16x","16"} };
static const Choice C_tvshader[] = { {"None","0"}, {"Scanline","1"}, {"Diagonal","2"}, {"Triangular","3"}, {"Wave","4"}, {"Lottes CRT","5"} };
static const Choice C_blend[]    = { {"Minimum","0"}, {"Basic","1"}, {"Medium","2"}, {"High","3"}, {"Full","4"}, {"Maximum","5"} };
static const Choice C_deint[]    = { {"Auto","0"}, {"None","1"}, {"Weave TFF","2"}, {"Weave BFF","3"}, {"Bob TFF","4"}, {"Bob BFF","5"}, {"Blend TFF","6"}, {"Blend BFF","7"}, {"Adaptive TFF","8"}, {"Adaptive BFF","9"} };
static const Choice C_dither[]   = { {"Off","0"}, {"Scaled","1"}, {"Unscaled","2"} };
static const Choice C_trifilter[]= { {"Automatic","-1"}, {"Off","0"}, {"Trilinear (PS2)","1"}, {"Trilinear (Forced)","2"} };
static const Choice C_crc[]      = { {"Auto","-1"}, {"None","0"}, {"Minimum","1"}, {"Partial","2"}, {"Full","3"}, {"Aggressive","4"} };
static const Choice C_preload[]  = { {"None","0"}, {"Partial","1"}, {"Full","2"} };
static const Choice C_cas[]      = { {"Off","0"}, {"Sharpen only","1"}, {"Sharpen + upscale","2"} };
static const Choice C_hwdl[]     = { {"Accurate","0"}, {"Disable readbacks","1"}, {"Unsynchronized","2"}, {"Disabled","3"} };
static const Choice C_interp[]   = { {"Nearest","0"}, {"Linear","1"}, {"Cubic","2"}, {"Hermite","3"}, {"Catmull-Rom","4"} };
static const Choice C_sync[]     = { {"TimeStretch","0"}, {"Async","1"}, {"None","2"} };
static const Choice C_eecr[]     = { {"50%","-3"}, {"60%","-2"}, {"75%","-1"}, {"100%","0"}, {"130%","1"}, {"180%","2"}, {"300%","3"} };
static const Choice C_eecs[]     = { {"Off","0"}, {"Mild","1"}, {"Moderate","2"}, {"Maximum","3"} };
// PS2 OSD language written into the BIOS NVRAM (values are PS2 language ids).
static const Choice C_syslang[]  = { {"Auto","auto"}, {"English","1"}, {"Japanese","0"},
                                     {"French","2"}, {"Spanish","3"}, {"German","4"}, {"Italian","5"},
                                     {"Dutch","6"}, {"Portuguese","7"}, {"Don't change","off"} };
// controller: a Switch button/none per PS2 target
static const Choice C_btn[]      = { {"A","A"},{"B","B"},{"X","X"},{"Y","Y"},{"L","L"},{"R","R"},{"ZL","ZL"},{"ZR","ZR"},
                                     {"Plus","Plus"},{"Minus","Minus"},{"L-Stick","StickL"},{"R-Stick","StickR"},
                                     {"D-Up","Up"},{"D-Down","Down"},{"D-Left","Left"},{"D-Right","Right"},{"None","None"} };
static const Choice C_stick[]    = { {"Left Stick","LStick"}, {"Right Stick","RStick"}, {"None","None"} };

enum { SCR_GRAPHICS, SCR_ENHANCE, SCR_AUDIO, SCR_EMU, SCR_NETWORK, SCR_CONTROLLER, SCR_COUNT };

static const Opt S_graphics[] = {
  O_CHOICE("Renderer",         "EmuCore/GS/Renderer", C_backend, "14"),
  O_CHOICE("Resolution scale", "EmuCore/GS/upscale_multiplier", C_upscale, "1"),
  O_CHOICE("Aspect ratio",     "EmuCore/GS/AspectRatio", C_aspect, "4:3"),
  O_CHOICE("FMV aspect",       "EmuCore/GS/FMVAspectRatioSwitch", C_fmvasp, "Off"),
  O_CHOICE("VSync",            "EmuCore/GS/VsyncEnable", C_vsync, "0"),
  O_CHOICE("Texture filtering","EmuCore/GS/filter", C_filter, "2"),
  O_CHOICE("Anisotropic",      "EmuCore/GS/MaxAnisotropy", C_aniso, "0"),
  O_CHOICE("Show FPS",         "EmuCore/GS/OsdShowFPS", C_bool, "false"),
  O_CHOICE("On-screen messages","EmuCore/GS/OsdShowMessages", C_bool, "true"),
  O_CHOICE("Widescreen patch", "EmuCore/EnableWideScreenPatches", C_bool, "false"),
  O_CHOICE("No-interlace patch","EmuCore/EnableNoInterlacingPatches", C_bool, "false"),
  O_SUB   ("Enhancements...",  SCR_ENHANCE),
};
static const Opt S_enhance[] = {
  O_CHOICE("Blending accuracy","EmuCore/GS/accurate_blending_unit", C_blend, "1"),
  O_CHOICE("Deinterlacing",    "EmuCore/GS/deinterlace_mode", C_deint, "0"),
  O_CHOICE("Dithering",        "EmuCore/GS/dithering_ps2", C_dither, "1"),
  O_CHOICE("Trilinear",        "EmuCore/GS/UserHacks_TriFilter", C_trifilter, "-1"),
  O_CHOICE("Mipmapping",       "EmuCore/GS/mipmap_hw", C_bool, "true"),
  O_CHOICE("CRC hack level",   "EmuCore/GS/CRCHackLevel", C_crc, "-1"),
  O_CHOICE("Texture preload",  "EmuCore/GS/texture_preloading", C_preload, "2"),
  O_CHOICE("GPU palette conv", "EmuCore/GS/paltex", C_bool, "false"),
  O_CHOICE("Anti-blur",        "EmuCore/GS/pcrtc_antiblur", C_bool, "true"),
  O_CHOICE("CRT/TV shader",    "EmuCore/GS/TVShader", C_tvshader, "0"),
  O_CHOICE("CAS sharpening",   "EmuCore/GS/CASMode", C_cas, "0"),
  O_RANGEG("CAS strength",     "EmuCore/GS/CASSharpness", 0, 100, 5, "50", "EmuCore/GS/CASMode", "0"),
  O_CHOICE("Shade boost",      "EmuCore/GS/ShadeBoost", C_bool, "false"),
  O_RANGEG("  Brightness",     "EmuCore/GS/ShadeBoost_Brightness", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("  Contrast",       "EmuCore/GS/ShadeBoost_Contrast", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("  Saturation",     "EmuCore/GS/ShadeBoost_Saturation", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_CHOICE("Texture replace",  "EmuCore/GS/LoadTextureReplacements", C_bool, "false"),
  O_CHOICE("SW renderer FMV",  "EmuCore/GS/SoftwareRendererFMV", C_bool, "false"),
  O_CHOICE("HW download mode", "EmuCore/GS/HWDownloadMode", C_hwdl, "0"),
};
static const Opt S_audio[] = {
  O_RANGE ("Volume",           "SPU2/Mixing/FinalVolume", 0, 100, 5, "100"),
  O_CHOICE("Interpolation",    "SPU2/Mixing/Interpolation", C_interp, "4"),
  O_CHOICE("Sync mode",        "SPU2/Output/SynchMode", C_sync, "0"),
  O_RANGE ("Buffer latency",   "SPU2/Output/Latency", 15, 200, 5, "60"),
  O_RANGE ("Output latency",   "SPU2/Output/OutputLatency", 20, 200, 10, "100"),
};
static const Opt S_emu[] = {
  O_CHOICE("Core version",     "Wrapper/CoreBuild", C_build, "4248"),
  O_CHOICE("System language",  "Wrapper/SystemLanguage", C_syslang, "auto"),
  O_CHOICE("EE cycle rate",    "EmuCore/Speedhacks/EECycleRate", C_eecr, "0"),
  O_CHOICE("EE cycle skip",    "EmuCore/Speedhacks/EECycleSkip", C_eecs, "0"),
  O_CHOICE("SMC code checks",  "Wrapper/EESmcCheck", C_bool, "true"),
  O_CHOICE("Fast boot",        "Wrapper/FastBoot", C_bool, "true"),
  O_CHOICE("MTVU (multi-VU)",  "EmuCore/Speedhacks/vuThread", C_bool, "true"),
  O_CHOICE("Instant VU1",      "EmuCore/Speedhacks/vu1Instant", C_bool, "true"),
  O_CHOICE("VU flag hack",     "EmuCore/Speedhacks/vuFlagHack", C_bool, "true"),
  O_CHOICE("Frame limiter",    "EmuCore/GS/FrameLimitEnable", C_bool, "true"),
  O_CHOICE("Sync to refresh",  "EmuCore/GS/SyncToHostRefreshRate", C_bool, "false"),
  O_CHOICE("Game patches",     "EmuCore/EnablePatches", C_bool, "true"),
  O_CHOICE("Cheats",           "EmuCore/EnableCheats", C_bool, "false"),
};
// Experimental PS2 network adapter (DEV9 "Sockets" backend). main.c brings up
// the libnx socket service and seeds the DEV9/Eth keys from these two.
static const Opt S_network[] = {
  O_CHOICE("Network adapter",  "Wrapper/Network", C_bool, "false"),
  O_TEXTG ("Custom DNS server", "Wrapper/NetDNS", "", "Wrapper/Network", "false"),
};
static const Opt S_controller[] = {
  O_CHOICE("Vibration",   "Wrapper/Vibration",     C_bool, "true"),
  O_CHOICE("Cross  (X)",  "Wrapper/Pad1/Cross",    C_btn, "B"),
  O_CHOICE("Circle (O)",  "Wrapper/Pad1/Circle",   C_btn, "A"),
  O_CHOICE("Square",      "Wrapper/Pad1/Square",   C_btn, "Y"),
  O_CHOICE("Triangle",    "Wrapper/Pad1/Triangle", C_btn, "X"),
  O_CHOICE("L1",          "Wrapper/Pad1/L1",       C_btn, "L"),
  O_CHOICE("R1",          "Wrapper/Pad1/R1",       C_btn, "R"),
  O_CHOICE("L2",          "Wrapper/Pad1/L2",       C_btn, "ZL"),
  O_CHOICE("R2",          "Wrapper/Pad1/R2",       C_btn, "ZR"),
  O_CHOICE("L3",          "Wrapper/Pad1/L3",       C_btn, "StickL"),
  O_CHOICE("R3",          "Wrapper/Pad1/R3",       C_btn, "StickR"),
  O_CHOICE("Select",      "Wrapper/Pad1/Select",   C_btn, "Minus"),
  O_CHOICE("Start",       "Wrapper/Pad1/Start",    C_btn, "Plus"),
  O_CHOICE("D-Pad Up",    "Wrapper/Pad1/Up",       C_btn, "Up"),
  O_CHOICE("D-Pad Down",  "Wrapper/Pad1/Down",     C_btn, "Down"),
  O_CHOICE("D-Pad Left",  "Wrapper/Pad1/Left",     C_btn, "Left"),
  O_CHOICE("D-Pad Right", "Wrapper/Pad1/Right",    C_btn, "Right"),
  O_CHOICE("Left stick",  "Wrapper/Pad1/LeftStick",  C_stick, "LStick"),
  O_CHOICE("  invert X",  "Wrapper/Pad1/LeftStickInvertX",  C_bool, "false"),
  O_CHOICE("  invert Y",  "Wrapper/Pad1/LeftStickInvertY",  C_bool, "false"),
  O_CHOICE("Right stick", "Wrapper/Pad1/RightStick", C_stick, "RStick"),
  O_CHOICE("  invert X",  "Wrapper/Pad1/RightStickInvertX", C_bool, "false"),
  O_CHOICE("  invert Y",  "Wrapper/Pad1/RightStickInvertY", C_bool, "false"),
  O_RANGE ("Stick deadzone %", "Wrapper/Pad1/Deadzone", 0, 50, 5, "0"),
};
struct Screen { const char *title; const Opt *opts; int n; bool binds; };
static const Screen g_screens[SCR_COUNT] = {
  { "Graphics",            S_graphics,   (int)(sizeof(S_graphics)/sizeof(Opt)),   false },
  { "Enhancements",        S_enhance,    (int)(sizeof(S_enhance)/sizeof(Opt)),    false },
  { "Audio",               S_audio,      (int)(sizeof(S_audio)/sizeof(Opt)),      false },
  { "Emulation / System",  S_emu,        (int)(sizeof(S_emu)/sizeof(Opt)),        false },
  { "Network (experimental)", S_network, (int)(sizeof(S_network)/sizeof(Opt)),    false },
  { "Controller",          S_controller, (int)(sizeof(S_controller)/sizeof(Opt)), true  },
};

// commit every managed option of the active store (so what was shown persists)
static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (o.key && (o.type == OT_CHOICE || o.type == OT_RANGE || o.type == OT_TEXT)) {
        std::string v = iniGet(o.key, o.def);
        iniSet(o.key, v.c_str());
      }
    }
}

// ---------------------------------------------------------------------------
// SDL / assets
// ---------------------------------------------------------------------------
static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr;
static SDL_Texture  *g_logo = nullptr; // romfs:/logo.png
static int SW = 1280, SH = 720;

static const SDL_Color COL_BG  = { 22, 24, 30, 255 };
static const SDL_Color COL_TXT = { 228, 230, 235, 255 };
static const SDL_Color COL_DIM = { 150, 155, 165, 255 };
static const SDL_Color COL_HI  = { 96, 200, 255, 255 };
static const SDL_Color COL_VAL = { 255, 210, 100, 255 };
static const SDL_Color COL_SEL = { 255, 170, 0, 255 };

static void fillRect(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(g_ren,&r); }
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }
static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf); SDL_Rect d={x,y,sf->w,sf->h}; SDL_FreeSurface(sf);
  if(t){ SDL_RenderCopy(g_ren,t,nullptr,&d); SDL_DestroyTexture(t); }
}
static int textW(TTF_Font*f,const char*s){ int w=0,h=0; if(f&&s) TTF_SizeUTF8(f,s,&w,&h); return w; }
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

// forward declarations (definitions live with the grid/menu renderers below)
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();

// --- region flag badges: 3 tiny textures drawn once at startup, reused per game
static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr }; // [1]=US [2]=EU [3]=JP
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){                                   // Japan: white field + red disc
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){                            // USA: stripes + blue canton
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){                            // Europe: blue + ring of stars
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

// --- Nintendo-style button glyphs: dark rounded buttons drawn once at startup, so
// control hints read as real controller icons instead of "A: launch". Face buttons
// are circles; shoulders (L/R) are pills. The letter is scaled to sit inside.
static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,*g_gL=nullptr,*g_gR=nullptr;
// Rendered at GLYPH_SS x the display size and drawn downscaled, so the circle/pill
// edges and the letter come out smooth (the naive 1x version looked jagged).
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;             // no shared font -> no glyphs
  const int S=GLYPH_SS, base=TTF_FontHeight(g_font_sm)+6;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  SDL_Color edge={14,16,22,255}, hi={92,99,114,255}, face={52,57,68,255}, ink={246,248,252,255};
  if(pill){                                     // concentric: dark edge -> light rim -> face
    int r=H/2;
    fillCircle(r,r,r,edge);     fillCircle(W-r,r,r,edge);     fillRect(r,0,W-2*r,H,edge);
    fillCircle(r,r,r-S,hi);     fillCircle(W-r,r,r-S,hi);     fillRect(r,S,W-2*r,H-2*S,hi);
    fillCircle(r,r,r-S*2,face); fillCircle(W-r,r,r-S*2,face); fillRect(r,S*2,W-2*r,H-S*4,face);
  } else {
    int R=H/2;
    fillCircle(W/2,H/2,R,edge);        // dark outline
    fillCircle(W/2,H/2,R-S,hi);        // light rim
    fillCircle(W/2,H/2,R-S*2,face);    // face
  }
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);   // big source -> crisp when downscaled
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false); g_gL=makeGlyph("L",true); g_gR=makeGlyph("R",true);
}

// --- on-screen control hints: a centred row of {glyph,label} pairs. Each item's
// tap rectangle is recorded so the same strip is usable by touch (footTapAct).
enum FootAct { FA_NONE, FA_LAUNCH, FA_SORT, FA_OPTIONS, FA_SETTINGS, FA_PAGEL, FA_PAGER, FA_QUIT };
struct FootItem { SDL_Texture *glyph; const char *label; int act; };
static SDL_Rect g_footHit[10]; static int g_footAct[10]; static int g_footN=0;
static void drawFooterHints(const FootItem *it,int n,int cy){
  const int gap=8, pairGap=26, glyphGap=16, fh=TTF_FontHeight(g_font_sm);
  int total=0;
  for(int i=0;i<n;i++){ int gw=0; if(it[i].glyph) SDL_QueryTexture(it[i].glyph,0,0,&gw,0); gw/=GLYPH_SS;
    total+=gw; bool L=it[i].label&&it[i].label[0];
    if(L) total+=gap+textW(g_font_sm,it[i].label);
    if(i<n-1) total+=(L?pairGap:glyphGap); }
  int x=(SW-total)/2; g_footN=0;
  for(int i=0;i<n;i++){ int gw=0,gh=0; if(it[i].glyph) SDL_QueryTexture(it[i].glyph,0,0,&gw,&gh); gw/=GLYPH_SS; gh/=GLYPH_SS;
    int x0=x;
    if(it[i].glyph){ SDL_Rect d={x,cy-gh/2,gw,gh}; SDL_RenderCopy(g_ren,it[i].glyph,nullptr,&d); }
    x+=gw; bool L=it[i].label&&it[i].label[0];
    if(L){ x+=gap; drawText(g_font_sm,x,cy-fh/2,it[i].label,COL_DIM); x+=textW(g_font_sm,it[i].label); }
    if(g_footN<10){ g_footHit[g_footN]={x0-6,cy-gh/2-8,(x-x0)+12,gh+16}; g_footAct[g_footN]=it[i].act; g_footN++; }
    if(i<n-1) x+=(L?pairGap:glyphGap);
  }
}
static int footTapAct(int px,int py){
  for(int i=0;i<g_footN;i++){ SDL_Rect &r=g_footHit[i];
    if(px>=r.x && px<r.x+r.w && py>=r.y && py<r.y+r.h) return g_footAct[i]; }
  return FA_NONE;
}

// --- touchscreen gestures (handheld only). The devkitPro SDL2 Switch driver emits
// SDL_FINGER* with 0..1 coords normalised to 1280x720; in handheld SW/SH are exactly
// that, so tfinger.x*SW is the raw panel pixel. Track one finger DOWN->UP, classify.
enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R };
struct TouchG { bool active=false; SDL_FingerID fid=0; float x0=0,y0=0; Uint32 t0=0; };
static TouchG g_touch;
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    // ignore a genuine second finger, but recover if a prior finger-up was lost
    // (e.g. an applet/home transition mid-touch) by restarting tracking after a gap.
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.fid=e.tfinger.fingerId;
    g_touch.x0=e.tfinger.x*SW; g_touch.y0=e.tfinger.y*SH; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS){ if(ox)*ox=(int)ux; if(oy)*oy=(int)uy; return TOUCH_TAP; }
  }
  return TOUCH_NONE;
}

// analog-stick navigation: turn left-stick pushes into synthetic D-Pad button
// events (edge-triggered per axis) so every menu loop supports the stick too.
static char stickNav(const SDL_Event &e){
  static bool lx=false, ly=false;
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!lx && e.caxis.value<-TH){ lx=true; return 'L'; }
    if(!lx && e.caxis.value> TH){ lx=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) lx=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!ly && e.caxis.value<-TH){ ly=true; return 'U'; }
    if(!ly && e.caxis.value> TH){ ly=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) ly=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static SDL_GameController *g_pad=nullptr;    // set in main(); used by navRepeat()
// Auto-repeat held dpad/stick as dpad events after a hold delay; call once per frame.
static void navRepeat(){
  if(!g_pad) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)    || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  static int held=0; static Uint32 since=0,last=0;
  Uint32 now=SDL_GetTicks();
  if(dir!=held){ held=dir; since=now; last=now; return; }  // direction changed: the physical event moves first
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-since<DELAY || now-last<RATE) return;
  last=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

// ---------------------------------------------------------------------------
// game scanning
// ---------------------------------------------------------------------------
struct Game {
  std::string path;    // emulator-form path ("/switch/..."), for EmuCore/DiscPath
  std::string file;    // filename
  std::string title;   // cleaned display title / SteamGridDB search term
  std::string key;     // sanitized filename stem (cover + gamecfg key)
  SDL_Texture *cover = nullptr;
  Uint32 coverAt = 0;   // when the texture was decoded -> brief fade-in on the grid
  bool triedCover = false;
  bool hasCfg = false;  // a gamecfg/<key>.ini exists (shows a marker)
  int region = 0;       // 0 unknown, 1 US, 2 EU, 3 JP (detected once at scan)
  long long added = 0;  // file mtime -> "recently added" sort
  long long played = 0; // last-played sequence -> "recently played" sort
};
static std::vector<Game> g_games;

// listing order (persisted as Wrapper/SortMode); Y on the grid cycles it.
enum { SORT_ALPHA, SORT_RECENT, SORT_ADDED, SORT_COUNT };
static const char *SORT_NAME[SORT_COUNT] = { "A-Z", "Recently played", "Recently added" };
static int g_sort = SORT_ALPHA;
static Store g_recent;   // recent.ini: game key -> last-played sequence number
static const char *RECENT_INI = "sdmc:/switch/nethersx2/recent.ini";

// region from the filename's (…)/[…] tags (Redump/No-Intro style), else 0.
static int detectRegion(const std::string &file) {
  std::string tags; int depth = 0;
  for (char c : file) {
    if (c=='('||c=='[') depth++;
    else if (c==')'||c==']') { if (depth) depth--; if (depth==0) tags += '|'; }
    else if (depth) tags += (char)tolower((unsigned char)c);
  }
  auto has = [&](const char *s){ return tags.find(s) != std::string::npos; };
  if (has("japan")||has("ntsc-j")||has("jpn")||has("(j)")) return 3;
  if (has("usa")||has("ntsc-u")||has("america")||has("(u)")) return 1;
  if (has("europe")||has("pal")||has("australia")||has("(uk")||has("france")||
      has("germany")||has("spain")||has("ital")||has("(e)")) return 2;
  std::string l; for (char c : file) l += (char)tolower((unsigned char)c);   // untagged fallback
  if (l.find("ntsc-j")!=std::string::npos) return 3;
  if (l.find("ntsc-u")!=std::string::npos) return 1;
  return 0;
}
static void applySort() {
  auto cmpTitle = [](const Game &a, const Game &b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; };
  std::sort(g_games.begin(), g_games.end(), [&](const Game &a, const Game &b){
    if (g_sort == SORT_RECENT && a.played != b.played) return a.played > b.played;
    if (g_sort == SORT_ADDED  && a.added  != b.added)  return a.added  > b.added;
    return cmpTitle(a, b);   // tiebreak + default
  });
}
// stamp a game as just-played (a monotonically increasing sequence in recent.ini)
// so "Recently played" sort has an order without needing a wall clock.
static void recordPlayed(const std::string &key){
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b); storeSave(g_global,LAUNCHER_INI);
  storeSet(g_recent,key.c_str(),b);        storeSave(g_recent,RECENT_INI);
}

static bool hasDiscExt(const char *n) {
  const char *e = strrchr(n, '.');
  if (!e) return false;
  static const char *x[] = { ".iso",".chd",".cso",".zso",".bin",".mdf",".img",".gz",".nrg" };
  for (auto s : x) if (!strcasecmp(e, s)) return true;
  return false;
}
static std::string toEmu(const std::string &sdmc) { return sdmc.rfind("sdmc:", 0) == 0 ? sdmc.substr(5) : sdmc; }
static std::string join(const std::string &b, const std::string &n) { std::string r=b; if(!r.empty()&&r.back()=='/') r.pop_back(); return r+"/"+n; }

// region/dump/format tokens to strip when they trail the name (outside brackets)
static bool isJunkToken(const std::string &tok) {
  std::string l;
  for (char c : tok) l += (char)tolower((unsigned char)c);
  static const char *junk[] = {
    "pal","ntsc","ntsc-u","ntsc-j","ntscu","ntscj","usa","us","europe","eu","japan","jp","jpn",
    "world","korea","asia","multi","multi3","multi5","nkit","redump","proper","unl","disc","cd","dvd",
    "iso","chd","cso","zso",
  };
  for (auto j : junk) if (l == j) return true;
  if (l.size() >= 2 && l[0] == 'v' && isdigit((unsigned char)l[1])) return true; // v1.01 etc.
  return false;
}
// clean a filename into a display title: drop extension, (paren)/[bracket] tags,
// collapse whitespace, and strip trailing region/dump tokens.
// "Dark Cloud 2 [PAL] [nKit]" / "Dark Cloud 2 - PAL - nKit" -> "Dark Cloud 2".
static std::string cleanTitle(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o; int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
    else if (!depth) o += (c == '_') ? ' ' : c; // keep . and - (subtitles)
  }
  // collapse whitespace
  std::string w; bool sp = true;
  for (char c : o) { if (isspace((unsigned char)c)) { if (!sp) w += ' '; sp = true; } else { w += c; sp = false; } }
  o = trim(w);
  // strip trailing junk tokens separated by space or dash
  for (;;) {
    size_t p = o.find_last_of(" -");
    std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
    if (!last.empty() && isJunkToken(last) && p != std::string::npos) {
      o = trim(o.substr(0, p));
      while (!o.empty() && (o.back() == '-' || o.back() == ' ' || o.back() == '.')) o.pop_back();
    } else break;
  }
  return trim(o);
}
static std::string sanitize(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o;
  for (char c : s) o += (isalnum((unsigned char)c) || c=='-'||c=='_') ? c : '_';
  return o;
}
static void scanGames(const std::string &sdmcDir) {
  for (auto &g : g_games) if (g.cover) SDL_DestroyTexture(g.cover);
  g_games.clear();
  DIR *d = opendir(sdmcDir.c_str());
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.' || !hasDiscExt(e->d_name)) continue;
    Game g;
    g.file = e->d_name;
    g.path = toEmu(join(sdmcDir, e->d_name));
    g.key = sanitize(e->d_name);
    const char *ct = storeGet(g_titles, g.key.c_str(), ""); // user rename wins
    g.title = ct[0] ? std::string(ct) : cleanTitle(e->d_name);
    g.region = detectRegion(e->d_name);
    g.played = atoll(storeGet(g_recent, g.key.c_str(), "0"));
    struct stat st;
    if (stat(join(sdmcDir, e->d_name).c_str(), &st) == 0) g.added = (long long)st.st_mtime;
    g.hasCfg = stat((std::string(GAMECFG_DIR) + "/" + g.key + ".ini").c_str(), &st) == 0;
    g_games.push_back(std::move(g));
  }
  closedir(d);
  applySort();
}
static std::string coverPath(const Game &g) { return std::string(COVERS_DIR) + "/" + g.key + ".png"; }

// Covers decode a few per frame (below), then stay resident for the session so
// paging never reloads them; freed only when the launcher exits to chainload.
static int g_cover_budget = 1 << 30;

// Load a cover, downscaled so the whole library stays cheap to keep resident.
// Grid cells are ~270-300px wide; capping the long edge at 360 keeps it sharp
// while cutting each texture from ~2.2 MB (600x900) to ~0.8 MB.
static SDL_Texture *loadCoverTexture(const std::string &path) {
  SDL_Surface *s = IMG_Load(path.c_str());
  if (!s) return nullptr;
  const int MAXW = 360;
  if (s->w > MAXW) {
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, MAXW, s->h * MAXW / s->w, 32, SDL_PIXELFORMAT_RGBA32);
    if (d) { SDL_BlitScaled(s, nullptr, d, nullptr); SDL_FreeSurface(s); s = d; }
  }
  SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
  SDL_FreeSurface(s);
  if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);  // so the decode fade-in's alpha mod works
  return t;
}
// lazy-load a cover texture (once, within the frame's budget); keep null if none
static void ensureCover(Game &g) {
  if (g.triedCover) return;
  if (g_cover_budget <= 0) return;      // over budget this frame -> retry next frame
  g_cover_budget--;
  g.triedCover = true;
  g.cover = loadCoverTexture(coverPath(g));
  if (g.cover) g.coverAt = SDL_GetTicks();
}
static void reloadCover(Game &g) {       // force-reload now (post-download); no budget
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.triedCover = true;
  g.cover = loadCoverTexture(coverPath(g));
  if (g.cover) g.coverAt = SDL_GetTicks();
}

// Switch software keyboard (libnx swkbd, an overlay applet). true + fills out on OK.
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && out[0];
}

// Directory browser: navigate the SD, return the chosen folder (sdmc:/...) or "".
static std::string browseFolder(const std::string &start) {
  std::string cur = start;
  int sel = 0, top = 0;
  for (;;) {
    std::vector<std::string> dirs;
    DIR *d = opendir(cur.c_str());
    if (d) { struct dirent *e; while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      struct stat st;
      if (stat(join(cur, e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) dirs.push_back(e->d_name);
    } closedir(d); }
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
    int n = 2 + (int)dirs.size(); // 0="[use this]", 1="..", 2+=subdirs
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    bool redo = false;
    while (!redo) {
      SDL_Event e;
      navRepeat();
      while (SDL_PollEvent(&e)) {
        pumpStick(e);
        { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
          if(tk==TOUCH_TAP){
            if(ty>=SH-40){ return ""; }
            int fy0=150,frowH=42,fvis=(SH-fy0-70)/frowH;
            for(int r=0;r<fvis && top+r<n;r++){ int y=fy0+r*frowH;
              if(ty>=y-4 && ty<y+frowH-6){ sel=top+r; SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
            continue;
          }
        }
        if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
        switch (e.cbutton.button) {
          case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel = (sel + n - 1) % n; break;
          case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel = (sel + 1) % n; break;
          case BTN_CANCEL: return "";
          case BTN_CONFIRM:
            if (sel == 0) return cur;                                     // use this folder
            else if (sel == 1) { size_t s = cur.find_last_of('/'); cur = s > 5 ? cur.substr(0, s) : "sdmc:/"; sel = 0; top = 0; redo = true; }
            else { cur = join(cur, dirs[sel - 2]); sel = 0; top = 0; redo = true; }
            break;
        }
        if (sel < top) top = sel;
        int vis = (SH - 150 - 70) / 42; if (sel >= top + vis) top = sel - vis + 1; if (top < 0) top = 0;
      }
      if (redo) break;
      SDL_SetRenderDrawColor(g_ren, COL_BG.r, COL_BG.g, COL_BG.b, 255); SDL_RenderClear(g_ren);
      drawText(g_font_big, 70, 36, "Select game folder", COL_HI);
      drawText(g_font_sm, 70, 96, cur.c_str(), COL_DIM);
      int y0 = 150, rowH = 42, vis = (SH - y0 - 70) / rowH;
      for (int r = 0; r < vis && top + r < n; r++) {
        int i = top + r, y = y0 + r * rowH; bool c = (i == sel);
        if (c) fillRect(60, y - 4, SW - 120, rowH - 6, (SDL_Color){66,56,30,255});
        const char *lbl = i == 0 ? "[ Use this folder ]" : i == 1 ? "[ .. up ]" : dirs[i - 2].c_str();
        drawText(g_font, 90, y, lbl, c ? COL_VAL : (i < 2 ? COL_VAL : COL_TXT));
      }
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

// ---------------------------------------------------------------------------
// generic settings screen (list). Returns when the user backs out.
// binds==true -> A on a bind row triggers press-to-bind capture.
// ---------------------------------------------------------------------------
static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key, o.def);
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
// a gated option is inactive (greyed, non-adjustable) while its parent is off
static bool optEnabled(const Opt &o) {
  return !o.gateKey || strcmp(iniGet(o.gateKey, ""), o.gateOff) != 0;
}
static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); snprintf(out,n,"%s", i>=0?o.ch[i].label:iniGet(o.key,o.def)); }
  else if (o.type==OT_RANGE) snprintf(out,n,"%s", iniGet(o.key,o.def));
  else if (o.type==OT_TEXT){ const char *v=iniGet(o.key,o.def); snprintf(out,n,"%s", (v&&*v)?v:"(auto)"); }
  else if (o.type==OT_SUBMENU) snprintf(out,n,">");
}
static void optAdjust(const Opt &o, int dir) {
  if (!optEnabled(o)) return;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch; iniSet(o.key,o.ch[i].val); }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
}

// press-to-bind overlay: wait for a Switch button, return its token or "" (B=cancel)
static const char *captureButton(SDL_GameController *pad) {
  struct M { SDL_GameControllerButton b; const char *tok; };
  static const M map[] = {
    {SDL_CONTROLLER_BUTTON_B,"A"},{SDL_CONTROLLER_BUTTON_A,"B"},{SDL_CONTROLLER_BUTTON_Y,"X"},{SDL_CONTROLLER_BUTTON_X,"Y"}, // Nintendo labels
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,"L"},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,"R"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,"StickL"},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,"StickR"},
    {SDL_CONTROLLER_BUTTON_START,"Plus"},{SDL_CONTROLLER_BUTTON_BACK,"Minus"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,"Up"},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,"Down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,"Left"},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,"Right"},
  };
  (void)pad;
  Uint32 start = SDL_GetTicks();
  // drain any button still held from the A-press that opened this overlay
  SDL_Delay(120);
  SDL_Event e;
  while (SDL_PollEvent(&e)) { /* flush */ }
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        for (auto &m : map) if (e.cbutton.button == m.b) return m.tok;
      } else if (e.type == SDL_CONTROLLERAXISMOTION) { // ZL/ZR are analog triggers on Switch
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT  && e.caxis.value > 16000) return "ZL";
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && e.caxis.value > 16000) return "ZR";
      } else if (e.type == SDL_QUIT) return "";
    }
    int remain = 6 - (int)((SDL_GetTicks() - start) / 1000);
    if (remain <= 0) return ""; // timed out -> cancel, keep current binding
    SDL_SetRenderDrawColor(g_ren,10,12,18,255); SDL_RenderClear(g_ren);
    int pw=780,ph=210,px=(SW-pw)/2,py=(SH-ph)/2;
    fillRect(px,py,pw,ph,(SDL_Color){20,22,30,246});
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big,SW/2,py+50,"Press a button to bind", COL_HI);
    char sub[64]; snprintf(sub,sizeof(sub),"wait %ds to cancel", remain);
    drawTextC(g_font,SW/2,py+126,sub, COL_DIM);
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

// --- shared UI polish: eased selection highlight + a brief screen fade-in -------
static float g_hy = -1;        // animated highlight top (settings list); <0 = snap
static Uint32 g_fxT = 0;       // screen-entry timestamp for the fade-in
static void beginScreenFx(){ g_fxT = SDL_GetTicks(); g_hy = -1; }
static void drawFadeIn(){
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
// top-bar height, shared by the grid header (gridLayout y0) and the settings header
static int topBarH(){ return SW >= 1600 ? 112 : 80; }
// header band: same height + logo as the grid's top bar, with a centred title
static void drawHeader(const char *title, const char *ctx){
  int bandH = topBarH() - 4;                        // identical to renderGrid's band
  fillRect(0,0,SW,bandH,(SDL_Color){28,31,40,255});
  fillRect(0,bandH,SW,2,COL_SEL);
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  drawTextC(g_font_big,SW/2,(bandH-TTF_FontHeight(g_font_big))/2,title,COL_VAL);
  if (ctx&&*ctx) drawTextR(g_font_sm,SW-28,(bandH-TTF_FontHeight(g_font_sm))/2,ctx,COL_VAL);
}
// the centred settings column geometry (one source of truth for render + scroll)
static const int ROW_H = 46, LIST_Y0 = 118;
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int w = SW-180; if (w>980) w=980;
  *colW=w; *colX=(SW-w)/2; *labelX=*colX+40; *valX=*colX+w-40;
}
static int listVis(){ int v=(SH-LIST_Y0-72)/ROW_H; return v<1?1:v; }

static void renderSettings(int scr,int sel,int top,const char *ctx){
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
  const Screen &S=g_screens[scr];
  drawHeader(S.title, ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  int fh0=TTF_FontHeight(g_font);
  // eased highlight bar with a left accent, tracking the selected visible row.
  // The bar fills the row slot (inset 1px); row text is centred in the same slot
  // so the label sits dead-centre of the selector.
  float ty = (float)(LIST_Y0 + (sel-top)*ROW_H + 1);
  g_hy = (g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  fillRect(colX,(int)g_hy,colW,ROW_H-2,(SDL_Color){66,56,30,235});
  fillRect(colX,(int)g_hy,5,ROW_H-2,COL_SEL);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,y=LIST_Y0+r*ROW_H+(ROW_H-fh0)/2; bool cur=(i==sel); bool en=optEnabled(S.opts[i]);
    SDL_Color lc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_TXT);
    SDL_Color vc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_DIM);
    drawText(g_font,labelX,y,S.opts[i].label,lc);
    char v[96]; optValue(S.opts[i],v,sizeof(v));
    drawTextR(g_font,valX,y,v,vc);
  }
  if(S.n>vis){                                  // slim scrollbar at the column's right
    int trH=vis*ROW_H, trX=colX+colW+16, trY=LIST_Y0-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  drawFadeIn();
  SDL_RenderPresent(g_ren);
}

// Scrollable list popup to pick one of N values (used for settings with >2 choices).
static int dropdown(const char *title, const char *const *labels, int n, int cur) {
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  const int rowH = 52;
  int vis = (SH - 200) / rowH; if (vis < 1) vis = 1; if (vis > n) vis = n;
  beginScreenFx();
  for (;;) {
    SDL_Event e;
    navRepeat();
    while (SDL_PollEvent(&e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){ int pw=SW>760?760:SW-160,px=(SW-pw)/2,ly=(SH-(90+vis*rowH))/2+70;
          for(int r=0;r<vis&&top+r<n;r++){ int y=ly+r*rowH; if(ty>=y&&ty<y+rowH&&tx>=px&&tx<px+pw){ return top+r; } }
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return cur;
      }
      if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
    }
    fillRect(0,0,SW,SH,(SDL_Color){10,12,18,255});
    int pw = SW>760?760:SW-160, ph = 90 + vis*rowH, px=(SW-pw)/2, py=(SH-ph)/2;
    fillRect(px,py,pw,ph,(SDL_Color){24,26,34,255});
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+18, title, COL_VAL);
    int ly = py+70;
    for(int r=0;r<vis && top+r<n;r++){
      int i=top+r, y=ly+r*rowH; bool curr=(i==sel);
      if(curr){ fillRect(px+8,y,pw-16,rowH-4,(SDL_Color){66,56,30,235}); fillRect(px+8,y,5,rowH-4,COL_SEL); }
      drawText(g_font, px+34, y+(rowH-TTF_FontHeight(g_font))/2, labels[i], curr?COL_VAL:COL_TXT);
    }
    if(n>vis){ int trH=vis*rowH,trX=px+pw-12,trY=ly; fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      int thH=trH*vis/n,dn=(n-vis>0?n-vis:1); fillRect(trX,trY+(trH-thH)*top/dn,4,thH,COL_SEL); }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}
// Open the dropdown for an OT_CHOICE option and store the chosen value.
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  const char* labels[32]; int n = o.nch>32?32:o.nch;
  for(int i=0;i<n;i++) labels[i]=o.ch[i].label;
  int idx = dropdown(o.label, labels, n, choiceIdx(o));
  if(idx>=0 && idx<o.nch) iniSet(o.key, o.ch[idx].val);
}

// run one settings screen; may push into a submenu. `ctx` = per-game title or NULL.
static void runSettings(int scr, SDL_GameController *pad, const char *ctx) {
  const Screen &S=g_screens[scr];
  int sel=0,top=0;
  while(sel<S.n-1 && !optEnabled(S.opts[sel])) sel++;   // start on the first enabled row
  auto nav=[&](int dir){ for(int k=0;k<S.n;k++){ sel=(sel+dir+S.n)%S.n; if(optEnabled(S.opts[sel])) break; } };
  beginScreenFx();
  for(;;){
    SDL_Event e;
    navRepeat();
    while(SDL_PollEvent(&e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
        if(tk==TOUCH_SWIPE_L){ optAdjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ optAdjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }                     // tap the title bar or bottom = back
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=LIST_Y0+r*ROW_H;
            if(ty>=y && ty<y+ROW_H){ int ni=top+r; if(optEnabled(S.opts[ni])){ sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); } }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  optAdjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: optAdjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_SUBMENU){ runSettings(o.sub,pad,ctx); beginScreenFx(); }
          else if(o.type==OT_TEXT){                            // A: edit via on-screen keyboard
            if(optEnabled(o)){
              char buf[128];
              if(promptText(o.label, iniGet(o.key,o.def), buf, sizeof(buf))) iniSet(o.key,buf);
            }
            beginScreenFx();
          }
          else if(S.binds && o.type==OT_CHOICE && o.ch==C_btn){ // press-to-bind a button
            const char *tok=captureButton(pad);
            if(tok&&*tok) iniSet(o.key,tok);
            beginScreenFx();
          }
          else if(o.type==OT_CHOICE && o.nch>2 && optEnabled(o)){ optChoosePopup(o); beginScreenFx(); } // dropdown for >2 choices
          else optAdjust(o,1);
          break;
        }
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
    }
    renderSettings(scr,sel,top,ctx);
    SDL_Delay(8);
  }
}
// the settings root: a small menu of the screens. For GLOBAL settings (ctx==NULL)
// it also offers a "Game folder" row that opens the folder browser and writes
// Wrapper/GameDir; the caller re-scans when that path changes.
static void runSettingsRoot(SDL_GameController *pad, const char *ctx) {
  static const char *names[SCR_COUNT] = { "Graphics","Enhancements","Audio","Emulation / System","Network (experimental)","Controller" };
  static const int order[] = { SCR_EMU, SCR_GRAPHICS, SCR_AUDIO, SCR_NETWORK, SCR_CONTROLLER };
  const int nscr=(int)(sizeof(order)/sizeof(*order));
  bool global = !(ctx && *ctx);
  int folderRow = nscr, coversRow = nscr+1;   // trailing rows, global settings only
  int n = nscr + (global?2:0), sel=0;
  beginScreenFx();
  for(;;){
    SDL_Event e;
    navRepeat();
    while(SDL_PollEvent(&e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          for(int i=0;i<n;i++){ int y=150+i*58; if(ty>=y && ty<y+58){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CONFIRM:
          if(global && sel==folderRow){
            std::string start = storeGet(g_global,"Wrapper/GameDir",DEF_GAMEDIR);
            std::string picked = browseFolder(start.empty()?DEF_GAMEDIR:start);
            if(!picked.empty()) storeSet(g_global,"Wrapper/GameDir",picked.c_str());
          } else if(global && sel==coversRow){ downloadAllCovers(); }
          else runSettings(order[sel],pad,ctx);
          beginScreenFx();
          break;
        case BTN_CANCEL: return;
      }
    }
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    drawHeader(global ? "Settings" : "Per-game settings", global?nullptr:ctx);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int rowH=58, y0=150; int fh0=TTF_FontHeight(g_font);
    float ty=(float)(y0+sel*rowH+2);
    g_hy=(g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowH-4,(SDL_Color){66,56,30,235});
    fillRect(colX,(int)g_hy,5,rowH-4,COL_SEL);
    for(int i=0;i<n;i++){ int slot=y0+i*rowH, y=slot+(rowH-fh0)/2; bool cur=i==sel;
      if(global && i==folderRow){
        drawText(g_font,labelX,y,"Game folder",cur?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,toEmu(storeGet(g_global,"Wrapper/GameDir",DEF_GAMEDIR)).c_str(),cur?COL_VAL:COL_DIM);
      } else if(global && i==coversRow){
        drawText(g_font,labelX,y,"Download all covers",cur?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,"SteamGridDB",cur?COL_VAL:COL_DIM);
      } else {
        drawText(g_font,labelX,y,names[order[i]],cur?COL_VAL:COL_TXT);
        drawTextR(g_font,valX,y,">",cur?COL_VAL:COL_DIM);
      }
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

// ---------------------------------------------------------------------------
// toast (brief centered message with a render tick)
// ---------------------------------------------------------------------------
static void toast(const char *msg) {
  for (int f = 0; f < 2; f++) { // draw a couple frames so it shows
    SDL_SetRenderDrawColor(g_ren,10,12,18,255); SDL_RenderClear(g_ren);
    int pw=820,ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
    fillRect(px,py,pw,ph,(SDL_Color){20,22,30,245}); border(px,py,pw,ph,2,COL_HI);
    drawTextC(g_font,SW/2,py+46,msg,COL_TXT);
    SDL_RenderPresent(g_ren); SDL_Delay(10);
  }
}

// blocking warning box; returns when the user presses A/B (or the app closes).
static void modalMessage(const char *title, const std::vector<std::string> &lines) {
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }   // tap dismisses
      if (e.type == SDL_QUIT) return;
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    int pw = SW*3/4, ph = 150 + (int)lines.size()*40, px = (SW-pw)/2, py = (SH-ph)/2;
    fillRect(px,py,pw,ph,(SDL_Color){30,27,20,250});
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+34, title, COL_SEL);
    int y = py+108;
    for (auto &l : lines) { drawTextC(g_font, SW/2, y, l.c_str(), COL_TXT); y += 40; }
    drawTextC(g_font_sm, SW/2, py+ph-42, "Press A to continue", COL_DIM);
    SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

// blocking yes/no confirmation (red styling for destructive actions).
// A or tap-left = true (confirm); B or tap-right = false (cancel).
static bool confirmBox(const char *title, const std::vector<std::string> &lines) {
  int pw=SW*3/4, ph=200+(int)lines.size()*40, px=(SW-pw)/2, py=(SH-ph)/2;
  int bw=210, bh=56, bby=py+ph-bh-22, yesx=SW/2-bw-18, nox=SW/2+18;
  for(;;){
    SDL_Event e;
    navRepeat();
    while(SDL_PollEvent(&e)){
      pumpStick(e);
      // a destructive confirm: taps count ONLY on the explicit Yes/No buttons,
      // never a stray tap elsewhere on screen.
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_QUIT) return false;
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    fillRect(px,py,pw,ph,(SDL_Color){34,22,22,250});
    border(px,py,pw,ph,3,(SDL_Color){210,70,70,255});
    drawTextC(g_font_big,SW/2,py+34,title,(SDL_Color){235,120,120,255});
    int y=py+112; for(auto&l:lines){ drawTextC(g_font,SW/2,y,l.c_str(),COL_TXT); y+=40; }
    int fh=TTF_FontHeight(g_font);
    fillRect(yesx,bby,bw,bh,(SDL_Color){150,50,50,255}); border(yesx,bby,bw,bh,2,(SDL_Color){215,95,95,255});
    drawTextC(g_font,yesx+bw/2,bby+(bh-fh)/2,"Yes  (A)",COL_TXT);
    fillRect(nox,bby,bw,bh,(SDL_Color){48,54,64,255}); border(nox,bby,bw,bh,2,COL_DIM);
    drawTextC(g_font,nox+bw/2,bby+(bh-fh)/2,"No  (B)",COL_TXT);
    SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

// A PS2 BIOS dump must be user-supplied; true if bios/ holds at least one file.
static bool biosPresent() {
  DIR *d = opendir(BIOS_DIR);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    struct stat st;
    if (stat((std::string(BIOS_DIR) + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      found = true; break;
    }
  }
  closedir(d);
  return found;
}

// ---------------------------------------------------------------------------
// per-game menu (Start on a game): cover download, per-game settings, launch
// ---------------------------------------------------------------------------
static void downloadCover(Game &g) {
  std::string key = storeGet(g_global, "Wrapper/SteamGridDBKey", "");
  if (key.empty()) { // prompt once; stored in launcher.ini (no separate file)
    char buf[128];
    if (promptText("Enter your free SteamGridDB API key", "", buf, sizeof(buf))) {
      key = buf;
      storeSet(g_global, "Wrapper/SteamGridDBKey", buf);
      storeSave(g_global, LAUNCHER_INI);
    } else { toast("A SteamGridDB API key is required"); SDL_Delay(1200); return; }
  }
  toast("Downloading cover...");
  mkdir(COVERS_DIR, 0777);
  // covers/<key>.png -- griddb writes it; then reload the texture
  std::string sdmcCover = std::string(COVERS_DIR) + "/" + g.key + ".png";
  int rc = griddb_fetch_cover(key, g.title, sdmcCover);
  const char *m = rc==GRIDDB_OK?"Cover downloaded":
                  rc==GRIDDB_NO_KEY?"No API key":
                  rc==GRIDDB_NO_NET?"No internet connection":
                  rc==GRIDDB_NOT_FOUND?"No cover found for this title":"Download failed";
  if (rc==GRIDDB_OK) reloadCover(g);
  toast(m); SDL_Delay(1200);
}

// Batch: fetch a cover for every game in the folder that doesn't already have one.
// Each fetch blocks (network) but we poll for B / tap-cancel between games and show
// a live progress screen. Existing covers are skipped so re-runs are cheap.
static void downloadAllCovers() {
  std::string key = storeGet(g_global, "Wrapper/SteamGridDBKey", "");
  if (key.empty()) {
    char buf[128];
    if (promptText("Enter your free SteamGridDB API key", "", buf, sizeof(buf))) {
      key = buf; storeSet(g_global, "Wrapper/SteamGridDBKey", buf); storeSave(g_global, LAUNCHER_INI);
    } else { toast("A SteamGridDB API key is required"); SDL_Delay(1200); return; }
  }
  mkdir(COVERS_DIR, 0777);
  std::vector<int> todo;
  for (int i=0;i<(int)g_games.size();i++){ struct stat st; if(stat(coverPath(g_games[i]).c_str(),&st)!=0) todo.push_back(i); }
  if (todo.empty()) { toast("All covers already downloaded"); SDL_Delay(1200); return; }
  int total=(int)todo.size(), done=0, ok=0, fail=0; bool cancel=false;
  for (int k=0;k<total && !cancel;k++){
    Game &g = g_games[todo[k]];
    SDL_Event e; while(SDL_PollEvent(&e)){ pumpStick(e);       // allow cancel between games
      if(e.type==SDL_CONTROLLERBUTTONDOWN && e.cbutton.button==BTN_CANCEL) cancel=true;
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=SH-90) cancel=true; }
    }
    if(cancel) break;
    // progress screen, drawn BEFORE the blocking fetch so the current title shows
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    drawHeader("Download all covers", nullptr);
    drawTextC(g_font, SW/2, SH/2-96, ("Downloading  "+std::to_string(done+1)+" / "+std::to_string(total)).c_str(), COL_VAL);
    drawTitleCell(SW/2, SW-260, SH/2-44, g.title, true, COL_TXT);
    int bw=SW-360, bx=180, by=SH/2+16, bh=26;
    fillRect(bx,by,bw,bh,(SDL_Color){40,44,54,255}); border(bx,by,bw,bh,2,COL_DIM);
    fillRect(bx,by, total?bw*done/total:0, bh, COL_SEL);
    char st[64]; snprintf(st,sizeof(st),"%d downloaded    %d failed",ok,fail);
    drawTextC(g_font_sm, SW/2, by+46, st, COL_DIM);
    drawTextC(g_font_sm, SW/2, SH-46, "B / tap here: cancel", COL_DIM);
    SDL_RenderPresent(g_ren);
    int rc = griddb_fetch_cover(key, g.title, coverPath(g));
    if(rc==GRIDDB_OK){ ok++; reloadCover(g); } else fail++;
    done++;
  }
  char msg[96]; snprintf(msg,sizeof(msg),"Covers: %d downloaded, %d failed%s",ok,fail,cancel?" (cancelled)":"");
  toast(msg); SDL_Delay(1600);
}

// returns 1 = launch this game, 0 = back
// Pick an image for the forwarder icon: the game's cover or SteamGridDB icons.
static bool pickIcon(Game &g, char *outPath, size_t outSize) {
  std::string base = std::string(DATA_DIR) + "/forwarders", tmp = base + "/iconpick";
  mkdir(base.c_str(),0777); mkdir(tmp.c_str(),0777);
  if(DIR*d=opendir(tmp.c_str())){ struct dirent*e; while((e=readdir(d))) if(e->d_name[0]!='.') remove((tmp+"/"+std::string(e->d_name)).c_str()); closedir(d); }
  std::vector<std::string> paths; struct stat st;
  { std::string cp=coverPath(g); if(stat(cp.c_str(),&st)==0) paths.push_back(cp); }
  std::string key = storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(!key.empty()){
    SDL_SetRenderDrawColor(g_ren,10,12,18,255); SDL_RenderClear(g_ren);
    drawHeader("Choose an icon", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Fetching icons from SteamGridDB...", COL_TXT);
    SDL_RenderPresent(g_ren);
    int nf=griddb_fetch_icons(key,g.title,tmp,14);
    for(int i=0;i<nf;i++){ char p[300]; snprintf(p,sizeof(p),"%s/gicon_%d.png",tmp.c_str(),i); paths.push_back(p); }
  }
  if(paths.empty()){ toast("No icon found - add a SteamGridDB key or download a cover first"); SDL_Delay(1800); return false; }
  int n=(int)paths.size(); std::vector<SDL_Texture*> tex(n,nullptr);
  for(int i=0;i<n;i++) tex[i]=IMG_LoadTexture(g_ren,paths[i].c_str());
  // Adaptive grid: up to 5 columns, cell sized so every row fits on screen.
  int cols=n<5?n:5; if(cols<1)cols=1;
  int rows=(n+cols-1)/cols, gap=18, top=150, bot=40;
  int cw=(SW-80-(cols-1)*gap)/cols, ch=(SH-top-bot-(rows-1)*gap)/rows;
  int cell=cw<ch?cw:ch; if(cell>200)cell=200; if(cell<90)cell=90;
  int x0=(SW-(cols*cell+(cols-1)*gap))/2, y0=top;
  int sel=0, chosen=-1; bool done=false; beginScreenFx();
  while(!done){
    SDL_Event e; navRepeat();
    while(SDL_PollEvent(&e)){ pumpStick(e);
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=(sel+1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel+cols)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel-cols+n)%n; break;
        case BTN_CONFIRM: chosen=sel; done=true; break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    drawHeader("Choose an icon", g.title.c_str());
    for(int i=0;i<n;i++){ int r=i/cols,c=i%cols, x=x0+c*(cell+gap), y=y0+r*(cell+gap);
      if(i==sel) fillRect(x-6,y-6,cell+12,cell+12,COL_SEL);
      fillRect(x,y,cell,cell,(SDL_Color){24,26,34,255});
      if(tex[i]){ SDL_Rect d{x,y,cell,cell}; SDL_RenderCopy(g_ren,tex[i],nullptr,&d); }
      else drawTextC(g_font_sm,x+cell/2,y+cell/2,"?",COL_DIM);
    }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  for(auto t:tex) if(t) SDL_DestroyTexture(t);
  if(chosen>=0 && chosen<n){ snprintf(outPath,outSize,"%s",paths[chosen].c_str()); return true; }
  return false;
}

// Create + install a HOME shortcut: single window with icon picker + name/author fields.
static void forwarderWizard(Game &g) {
  char name[256]; snprintf(name,sizeof(name),"%s",g.title.c_str());
  char author[128]; snprintf(author,sizeof(author),"%s","NetherSX2");
  char icon[300]={0};
  { struct stat st; std::string cp=coverPath(g);
    if(stat(cp.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",cp.c_str()); }
  SDL_Texture *iconTex = icon[0] ? IMG_LoadTexture(g_ren, icon) : nullptr;

  const int ix=110, iy=176, isz=280;                        // icon rect (left)
  const int rx=ix+isz+70; int rw=SW-rx-90;                  // right column
  const int nameY=196, authY=290, createY=406, fieldH=64, createH=58;
  int sel=0; bool done=false; beginScreenFx();              // 0 icon, 1 name, 2 author, 3 create

  auto edit=[&](const char *hdr, char *buf, size_t sz){ char b[256]; if(promptText(hdr, buf, b, sizeof(b)) && b[0]) snprintf(buf,sz,"%s",b); };
  auto build=[&](){
    if(!icon[0]){ toast("Pick an icon first"); SDL_Delay(1200); return; }
    SDL_SetRenderDrawColor(g_ren,10,12,18,255); SDL_RenderClear(g_ren);
    drawHeader("Creating HOME shortcut", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Building + installing forwarder...", COL_TXT);
    SDL_RenderPresent(g_ren);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    char err[256]={0}; bool ok=forwarder_create(g.key,name,author,icon,err,sizeof(err));
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(ok){ toast("HOME shortcut installed"); SDL_Delay(1800); done=true; }
    else modalMessage("Shortcut failed", { err[0]?err:"Unknown error", "", "A HOME forwarder needs sigpatches on your CFW." });
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel==0){ char p[300]; if(pickIcon(g,p,sizeof(p))){ snprintf(icon,sizeof(icon),"%s",p); if(iconTex)SDL_DestroyTexture(iconTex); iconTex=IMG_LoadTexture(g_ren,icon); } beginScreenFx(); }
    else if(sel==1) edit("Shortcut name", name, sizeof(name));
    else if(sel==2) edit("Author", author, sizeof(author));
    else build();
  };

  while(!done){
    SDL_Event e; navRepeat();
    while(SDL_PollEvent(&e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(tx>=ix&&tx<ix+isz&&ty>=iy&&ty<iy+isz){ sel=0; activate(); }
          else if(ty>=nameY-6&&ty<nameY+fieldH){ sel=1; activate(); }
          else if(ty>=authY-6&&ty<authY+fieldH){ sel=2; activate(); }
          else if(ty>=createY-6&&ty<createY+createH){ sel=3; activate(); }
          else if(ty>=SH-40) done=true;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=0; break;                 // icon is on the left
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if(sel==0) sel=1; break;      // back to the fields
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel==0)?3:(sel==1?3:sel-1); break;  // cycle the right column
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel==0)?1:(sel==3?1:sel+1); break;
        case BTN_CONFIRM: activate(); break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    drawHeader("Create HOME shortcut", g.title.c_str());
    if(sel==0) fillRect(ix-6,iy-6,isz+12,isz+12,COL_SEL);
    fillRect(ix,iy,isz,isz,(SDL_Color){24,26,34,255});
    if(iconTex){ SDL_Rect d{ix,iy,isz,isz}; SDL_RenderCopy(g_ren,iconTex,nullptr,&d); }
    else drawTextC(g_font_sm,ix+isz/2,iy+isz/2,"(no icon)",COL_DIM);
    drawTextC(g_font_sm, ix+isz/2, iy+isz+20, "Icon", sel==0?COL_VAL:COL_DIM);
    auto field=[&](int idx,int y,const char*label,const char*val){ bool cur=sel==idx;
      if(cur){ fillRect(rx-10,y-6,rw+20,fieldH,(SDL_Color){66,56,30,235}); fillRect(rx-10,y-6,5,fieldH,COL_SEL); }
      drawText(g_font_sm, rx, y, label, cur?COL_VAL:COL_DIM);
      drawText(g_font, rx, y+26, val, cur?COL_VAL:COL_TXT); };
    field(1,nameY,"Name",name);
    field(2,authY,"Author",author);
    { bool cur=sel==3;
      fillRect(rx-10,createY-6,rw+20,createH, cur?(SDL_Color){44,86,44,240}:(SDL_Color){30,46,32,200});
      if(cur) fillRect(rx-10,createY-6,5,createH,COL_SEL);
      drawTextC(g_font, rx+rw/2, createY+12, "Create shortcut", cur?COL_VAL:(SDL_Color){150,225,150,255}); }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  if(iconTex) SDL_DestroyTexture(iconTex);
}

static int perGameMenu(Game &g, SDL_GameController *pad) {
  const char *items[] = { "Launch", "Per-game settings", "Rename game", "Download cover (SteamGridDB)", "Create HOME shortcut", "Clear per-game settings", "Delete game (remove from SD)" };
  int n=7, sel=0;
  // load this game's override store
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  storeLoad(g_game, gp.c_str());
  beginScreenFx();
  for(;;){
    SDL_Event e;
    navRepeat();
    while(SDL_PollEvent(&e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
        if(tk==TOUCH_TAP){
          if(ty>=SH-40){ return 0; }
          for(int i=0;i<n;i++){ int y=210+i*56; if(ty>=y-6 && ty<y+50){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(pad,g.title.c_str());   // edits only write changed keys into g_game
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            storeSave(g_game, gp.c_str());
            g.hasCfg = !g_game.kv.empty();
            beginScreenFx();
          }
          else if(sel==2){                          // rename (software keyboard)
            char buf[128];
            if(promptText("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==3){ downloadCover(g); beginScreenFx(); }
          else if(sel==4){ forwarderWizard(g); beginScreenFx(); }
          else if(sel==5){ g_game.kv.clear(); remove(gp.c_str()); g.hasCfg=false; toast("Per-game settings cleared"); SDL_Delay(700); beginScreenFx(); }
          else if(sel==6){                          // delete the game file from SD entirely
            if(confirmBox("Delete game?", { g.title, "", "This permanently deletes the game file from",
                                            "the SD card. This cannot be undone." })){
              std::string disc = "sdmc:" + g.path;
              remove(disc.c_str());                 // the disc image
              remove(coverPath(g).c_str());         // its cached cover
              remove(gp.c_str());                   // its per-game settings
              storeRemove(g_titles, g.key.c_str()); storeSave(g_titles, TITLES_INI);  // rename entry
              storeRemove(g_recent, g.key.c_str()); storeSave(g_recent, RECENT_INI);  // play history
              toast("Game deleted"); SDL_Delay(800);
              return 2;                              // tell the caller to re-scan the folder
            }
          }
          break;
      }
    }
    // render: cover on left, menu on right
    SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
    g_cover_budget = 1;         // single cover here -- allow it to load
    ensureCover(g);
    int cw=300,chh=450,cx=90,cy=(SH-chh)/2;
    fillRect(cx+5,cy+7,cw,chh,(SDL_Color){0,0,0,60}); fillRect(cx+2,cy+3,cw,chh,(SDL_Color){0,0,0,75});  // cover shadow
    if(g.cover){ SDL_SetTextureAlphaMod(g.cover,255); SDL_SetTextureColorMod(g.cover,255,255,255);  // clear any grid dim/fade
      SDL_Rect d={cx,cy,cw,chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d); border(cx,cy,cw,chh,2,COL_DIM); }
    else { fillRect(cx,cy,cw,chh,(SDL_Color){40,44,54,255}); border(cx,cy,cw,chh,2,COL_DIM); drawTextC(g_font,cx+cw/2,cy+chh/2,"NO COVER",COL_DIM); }
    drawText(g_font_big,cx+cw+70,120,g.title.c_str(),COL_TXT);
    int mx=cx+cw+64, mw=SW-mx-70;                        // eased highlight bar + left accent
    float ty=(float)(210+sel*56-6);
    g_hy=(g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    fillRect(mx,(int)g_hy,mw,48,(SDL_Color){66,56,30,235});
    fillRect(mx,(int)g_hy,5,48,COL_SEL);
    for(int i=0;i<n;i++){ int y=210+i*56; bool cur=i==sel;
      SDL_Color rc = (i==n-1) ? (SDL_Color){228,120,120,255} : COL_TXT;   // delete row = red
      drawText(g_font,cx+cw+94,y,items[i],cur?COL_VAL:rc);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

// ---------------------------------------------------------------------------
// extraction + chainload
// ---------------------------------------------------------------------------
// Atomic + checked extraction: write to <dst>.tmp, verify every fwrite, fsync,
// rename into place, and confirm the final size == source. Returns false on any
// error (partial SD write, full card) so the caller never chainloads a corrupt
// core/renderer. Skips work if <dst> already exists at the right size -- UNLESS
// `force` (needed when the packed file changed but kept the same length, e.g. a
// shader that differs between core builds).
static bool extractFromRomfs(const char *src, const char *dst, bool force=false) {
  struct stat ss, ds;
  if (stat(src,&ss)!=0) return false;
  if (!force && stat(dst,&ds)==0 && ds.st_size==ss.st_size) return true;
  std::string tmp = std::string(dst) + ".tmp";
  FILE *in=fopen(src,"rb"), *out=fopen(tmp.c_str(),"wb");
  if(!in||!out){ if(in)fclose(in); if(out)fclose(out); return false; }
  static char buf[1<<16]; size_t n; bool ok=true;
  while((n=fread(buf,1,sizeof(buf),in))>0){ if(fwrite(buf,1,n,out)!=n){ ok=false; break; } }
  if(ferror(in)) ok=false;
  fclose(in);
  if(fclose(out)!=0) ok=false;                     // catches deferred write errors
  if(!ok){ remove(tmp.c_str()); return false; }
  remove(dst);                                     // rename won't overwrite on fatfs
  if(rename(tmp.c_str(), dst)!=0){ remove(tmp.c_str()); return false; }
  return stat(dst,&ds)==0 && ds.st_size==ss.st_size;
}

// Recursively mirror a romfs subtree to an sdmc destination (mkdir dirs, copy
// files via extractFromRomfs). `force` overwrites size-matching files too.
static bool extractTree(const std::string &src, const std::string &dst, bool force) {
  mkdir(dst.c_str(), 0777);
  DIR *d = opendir(src.c_str());
  if (!d) return false;
  bool ok = true;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    std::string s = src + "/" + e->d_name, t = dst + "/" + e->d_name;
    struct stat st;
    if (stat(s.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      ok = extractTree(s, t, force) && ok;
    else
      ok = extractFromRomfs(s.c_str(), t.c_str(), force) && ok;
  }
  closedir(d);
  return ok;
}

// The data files (shaders, GameIndex.yaml) DIFFER between the 4248 and 3668
// builds -- the wrong shaders render black -- so both sets are packed in romfs.
// Extract the one matching `build`; a marker file skips it when already current.
static const char *RES_MARKER = "sdmc:/switch/nethersx2/resources/.nethersx2_build";
static bool ensureResources(const std::string &build) {
  char cur[16] = {0};
  FILE *f = fopen(RES_MARKER, "r");
  if (f) { if (!fgets(cur, sizeof(cur), f)) cur[0] = 0; fclose(f); }
  struct stat st;
  bool present = stat((std::string(RESOURCES_DIR) + "/GameIndex.yaml").c_str(), &st) == 0;
  if (trim(cur) == build && present) return true;       // already the right version
  toast("Extracting NetherSX2 resources (one-time)...");
  mkdir(RESOURCES_DIR, 0777);
  bool ok = extractTree(std::string("romfs:/res/") + build, RESOURCES_DIR, true);
  if (ok) { FILE *w = fopen(RES_MARKER, "w"); if (w) { fprintf(w, "%s\n", build.c_str()); fclose(w); } }
  return ok;
}

// The emulator .nro is the one artifact we recompile, and a rebuild can change it
// while keeping the same byte length -- which extractFromRomfs's size-match skip
// would miss. So gate re-extraction on the launcher build stamp: re-extract a
// variant only when the launcher was rebuilt since that variant was last written.
// build_all.sh always recompiles this file, so __DATE__/__TIME__ changes per build.
// Per-variant marker (gl/vk) so switching renderer after a rebuild can't leave a
// stale binary. Costs one ~15 MB copy per rebuilt variant, not one per launch.
static const char *BUILD_STAMP = __DATE__ " " __TIME__;
static bool ensureEmu(const char *src, const char *dst, const std::string &rtag) {
  std::string marker = std::string(DATA_DIR) + "/.emu_build_" + rtag;
  char cur[48] = {0};
  FILE *f = fopen(marker.c_str(), "r");
  if (f) { if (!fgets(cur, sizeof(cur), f)) cur[0] = 0; fclose(f); }
  struct stat st;
  if (trim(cur) == BUILD_STAMP && stat(dst, &st) == 0) return true; // current build on SD
  if (!extractFromRomfs(src, dst, /*force=*/true)) return false;
  FILE *w = fopen(marker.c_str(), "w");
  if (w) { fprintf(w, "%s\n", BUILD_STAMP); fclose(w); }
  return true;
}

// ---------------------------------------------------------------------------
// cover grid (main screen). Returns the chosen game index to launch, or -1 quit.
// ---------------------------------------------------------------------------
// Adaptive grid layout: fixed 2 rows, cover size derived to fit the height, and
// the column count derived from the width (so more, smaller covers on a bigger
// screen). One source of truth for both rendering and navigation.
struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  bool big = SW >= 1600;
  g.gapx = big?24:18; g.gapy = big?18:14; g.titleH = big?30:24;
  int topBar = big?112:80, footer = big?54:38; // header band holds the logo

  g.rows = 2;
  g.y0 = topBar;
  int availH = SH - topBar - footer;
  g.chh = (availH - (g.rows-1)*g.gapy - g.rows*(g.titleH+8)) / g.rows;
  if(g.chh < 120) g.chh = 120;
  g.cw = g.chh*2/3;                       // 2:3 covers
  int margin = big?60:40;
  g.cols = (SW - 2*margin + g.gapx) / (g.cw + g.gapx);
  if(g.cols < 1) g.cols = 1;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  return g;
}
// touch hit-test: which game cell (cover+title) contains pixel (px,py), or -1.
// Mirrors renderGrid's cell geometry exactly.
static int gridHitTest(int px,int py,int top){
  GLay L=gridLayout(); int n=(int)g_games.size();
  int rowStride=L.chh+L.titleH+8+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+L.titleH+8) return idx;
  }
  return -1;
}
// title under a cover: fits -> centered; too long + not selected -> "...";
// too long + selected -> horizontal marquee (ping-pong) clipped to the cell.
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    std::string t=title;
    while(!t.empty() && textW(f,(t+"...").c_str())>cellW) t.pop_back();
    t+="...";
    drawTextC(f,cx,y,t.c_str(),col);
    return;
  }
  SDL_Rect clip={x0,y-2,cellW,(f?TTF_FontHeight(f):26)+8};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-cellW;
  float t=(SDL_GetTicks()%5000)/5000.0f;   // 5s cycle
  float pp = t<0.5f ? t*2.f : (1.f-t)*2.f;  // 0..1..0
  drawText(f,x0-(int)(pp*span),y,title.c_str(),col);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

// right-anchored text bounded to maxW; if it overflows, clip to the box and
// ping-pong scroll so the whole string stays readable (header folder path).
static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;        // 0..1..0
  drawText(f,x0-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void renderGrid(int sel,int top,const char*gamedirLabel){
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255); SDL_RenderClear(g_ren);
  g_cover_budget = 3;   // decode at most 3 covers this frame -> no page-flip hitch
  if(sel>=0 && sel<(int)g_games.size()) ensureCover(g_games[sel]); // selected one first
  GLay L=gridLayout();
  int n=(int)g_games.size(), per=L.cols*L.rows;
  int pages=n?(n+per-1)/per:1, page=n?(sel/per)+1:1;
  // header: subtle band + logo (no wordmark -- the logo is the branding)
  int bandH = L.y0 - 4;
  fillRect(0,0,SW,bandH,(SDL_Color){28,31,40,255});
  fillRect(0,bandH,SW,2,COL_SEL);
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  char pinfo[160]; snprintf(pinfo,sizeof(pinfo),"%d / %d    \xc2\xb7    Page %d / %d    \xc2\xb7    Sort: %s",n?sel+1:0,n,page,pages,SORT_NAME[g_sort]);
  drawTextC(g_font,SW/2,(bandH-TTF_FontHeight(g_font))/2,pinfo,COL_VAL);
  // folder path: keep it clear of the centred page/sort info; scroll if too long
  int pinfoRight=SW/2+textW(g_font,pinfo)/2;
  int folderMaxW=(SW-34)-(pinfoRight+24);
  drawScrollTextR(g_font_sm,SW-34,(bandH-TTF_FontHeight(g_font_sm))/2,folderMaxW,gamedirLabel,COL_DIM);

  int rowStride=L.chh+L.titleH+8+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c;
    if(idx>=n) continue;
    Game&g=g_games[idx];
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    bool cur=(idx==sel);
    ensureCover(g);
    fillRect(x+4,y+6,L.cw,L.chh,(SDL_Color){0,0,0,55});   // drop shadow -> depth on the flat bg
    fillRect(x+2,y+3,L.cw,L.chh,(SDL_Color){0,0,0,70});
    if(g.cover){
      Uint32 el=SDL_GetTicks()-g.coverAt; Uint8 fa=el<180?(Uint8)(255*el/180):255;   // fade in as it decodes
      SDL_SetTextureAlphaMod(g.cover,fa);
      SDL_SetTextureColorMod(g.cover,cur?255:150,cur?255:150,cur?255:150);
      SDL_Rect d={x,y,L.cw,L.chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d);
    }
    else { fillRect(x,y,L.cw,L.chh,(SDL_Color){40,44,54,255}); drawTextC(g_font_sm,x+L.cw/2,y+L.chh/2-8,"NO COVER",COL_DIM); }
    border(x,y,L.cw,L.chh,1,(SDL_Color){12,13,18,255});   // crisp edge separates art from bg
    fillRect(x,y,L.cw,1,(SDL_Color){255,255,255,26});     // faint top bevel
    if(cur){ const int G=6;                                // soft static selection glow (amber)
      for(int i=G;i>=1;i--){ Uint8 a=(Uint8)(150*(G-i+1)/G); border(x-2-i,y-2-i,L.cw+4+2*i,L.chh+4+2*i,1,(SDL_Color){255,170,0,a}); }
      border(x-2,y-2,L.cw+4,L.chh+4,2,COL_SEL);
    }
    if(g.region>0 && g_flag[g.region]){          // region flag, top-left corner (small badge)
      int fw=L.cw*26/100; if(fw>30)fw=30; if(fw<16)fw=16; int fh=fw*2/3;
      SDL_Rect fd={x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[g.region],nullptr,&fd);
      border(x+6,y+6,fw,fh,1,(SDL_Color){10,12,18,255});
    }
    if(g.hasCfg){ int ds=L.cw/11<12?12:L.cw/11; fillRect(x+L.cw-ds-8,y+8,ds,ds,COL_SEL); border(x+L.cw-ds-8,y+8,ds,ds,2,(SDL_Color){10,12,18,255}); }
    drawTitleCell(x+L.cw/2, L.cw, y+L.chh+6, g.title, cur, cur?COL_VAL:COL_DIM);
  }
  if(n==0) drawTextC(g_font,SW/2,SH/2,"No games found -- press X for Settings > Game folder",COL_DIM);
  // control hints with real button glyphs; each item doubles as a touch target.
  FootItem foot[] = {
    { g_gA, "Launch", FA_LAUNCH }, { g_gY, "Sort", FA_SORT },
    { g_gX, "Settings", FA_SETTINGS }, { g_gPlus, "Game Menu", FA_OPTIONS },
    { g_gL, "", FA_PAGEL }, { g_gR, "Page", FA_PAGER }, { g_gB, "Quit", FA_QUIT },
  };
  drawFooterHints(foot, 7, SH-26);
  SDL_RenderPresent(g_ren);
}

// Page-based grid navigation (pages of cols*rows games). Crossing a page edge
// turns the WHOLE page (both rows). dx: -1 left / +1 right; dy: -1 up / +1 down.
static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){ // right: next cell, or turn to the next page (same row, first col)
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){ // left: prev cell, or previous page (same row, last col)
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){ // down: within the page only (no page change on the vertical edges)
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){ // up: within the page only
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

// Jump a whole page (L/R shoulders), keeping the cell position.
static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv){
  (void)argc;(void)argv;
  // launcher's own path, so HOME forwarders chainload the right NRO.
  extern std::string g_forwarderSelfPath;
  if(argc>=1 && argv[0] && argv[0][0]) g_forwarderSelfPath=argv[0];
  romfsInit();
  griddb_global_init();
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)!=0) return 1;
  TTF_Init();
  IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG);
  if(appletGetOperationMode()==AppletOperationMode_Console){ SW=1920; SH=1080; }
  g_win=SDL_CreateWindow("NetherSX2",0,0,SW,SH,SDL_WINDOW_FULLSCREEN);
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  SDL_GetRendererOutputSize(g_ren,&SW,&SH);
  { SDL_Surface *ls=IMG_Load("romfs:/logo.png"); if(ls){ g_logo=SDL_CreateTextureFromSurface(g_ren,ls); SDL_FreeSurface(ls); } }
  makeFlags();   // region-flag badges (rendered once, reused per game)

  SDL_GameController *pad=nullptr;
  for(int i=0;i<SDL_NumJoysticks();i++) if(SDL_IsGameController(i)){ pad=SDL_GameControllerOpen(i); break; }
  g_pad=pad;

  plInitialize(PlServiceType_User);
  PlFontData fd;
  if(R_SUCCEEDED(plGetSharedFontByType(&fd,PlSharedFontType_Standard))){
    int sc = SH>=1080?1:0;
    g_font_sm =TTF_OpenFontRW(SDL_RWFromConstMem(fd.address,(int)fd.size),1, sc?26:20);
    g_font    =TTF_OpenFontRW(SDL_RWFromConstMem(fd.address,(int)fd.size),1, sc?32:26);
    g_font_big=TTF_OpenFontRW(SDL_RWFromConstMem(fd.address,(int)fd.size),1, sc?52:40);
  }
  makeGlyphs();   // button-icon textures for the control hints (needs g_font_sm)

  // First-launch bootstrap: create the SD folder skeleton and, if launcher.ini is
  // absent, seed it with defaults and save it, so config persists from the first
  // run. bios/ starts empty -- the user supplies the BIOS.
  mkdir("sdmc:/switch", 0777);
  mkdir(DATA_DIR, 0777);    mkdir(COVERS_DIR, 0777);  mkdir(CORES_DIR, 0777);
  mkdir(GAMECFG_DIR, 0777); mkdir(DEF_GAMEDIR, 0777);
  mkdir(BIOS_DIR, 0777);    mkdir(RESOURCES_DIR, 0777);

  struct stat bst;
  bool firstRun = (stat(LAUNCHER_INI, &bst) != 0);
  storeLoad(g_global, LAUNCHER_INI);
  storeLoad(g_titles, TITLES_INI);
  storeLoad(g_recent, RECENT_INI);
  { int sm = atoi(storeGet(g_global,"Wrapper/SortMode","0")); if(sm>=0 && sm<SORT_COUNT) g_sort = sm; }
  if (firstRun) {
    // Seed a fresh launcher.ini with defaults + a blank SteamGridDB key line (so
    // the user can paste their key straight into the file). First launch only --
    // never rewrite an existing config, which would clobber a key already set.
    g_active = &g_global;
    storeSet(g_global, "Wrapper/GameDir", DEF_GAMEDIR);
    storeSet(g_global, "Wrapper/SteamGridDBKey", "");
    commitAll();                        // write every managed setting at its default
    storeSave(g_global, LAUNCHER_INI);  // create launcher.ini immediately
  }
  std::string gameDir = storeGet(g_global, "Wrapper/GameDir", DEF_GAMEDIR);
  scanGames(gameDir);

  // A PS2 BIOS is the one thing the launcher can't provide -- warn up front if the
  // bios/ folder is empty so the user knows to add one (games won't boot without it).
  if (!biosPresent())
    modalMessage("No PS2 BIOS found", {
      "Copy a PS2 BIOS dump into:",
      toEmu(BIOS_DIR),
      "",
      "Games will not boot until you add one." });

  int sel=0, top=0, rows=1;
  bool running=true, launch=false;
  std::string launchKey; // sanitized key of the game being launched (per-game cfg)

  // Forwarder headless boot: a HOME shortcut launches us as "<launcher>.nro -g <game key>".
  // Find that game and go straight to launch, skipping the grid UI.
  for(int ai=1; ai+1<argc; ai++) if(strcmp(argv[ai],"-g")==0){
    std::string wantKey=argv[ai+1];
    for(auto &gm:g_games) if(gm.key==wantKey){
      recordPlayed(gm.key); storeSet(g_global,"EmuCore/DiscPath",gm.path.c_str());
      launchKey=gm.key; launch=true; running=false; break;
    }
    break;
  }

  while(running){
    GLay L=gridLayout();
    int cols=L.cols; rows=L.rows;

    SDL_Event e;
    navRepeat();
    while(SDL_PollEvent(&e)){
      pumpStick(e);
      if(e.type==SDL_QUIT){ running=false; break; }
      if(e.type==SDL_CONTROLLERDEVICEADDED && !pad){ pad=SDL_GameControllerOpen(e.cdevice.which); g_pad=pad; continue; }
      { int tx=0,ty=0,n=(int)g_games.size(); TouchKind tk=touchFeed(e,&tx,&ty);   // touchscreen
        if(tk==TOUCH_SWIPE_L||tk==TOUCH_SWIPE_R){ sel=gridPage(sel,tk==TOUCH_SWIPE_L?+1:-1,cols,rows,n); top=n?(sel/(cols*rows))*rows:0; continue; }
        if(tk==TOUCH_TAP){
          int fa=footTapAct(tx,ty);
          if(fa==FA_NONE){ int hit=gridHitTest(tx,ty,top);
            if(hit>=0){
              if(hit==sel && n){ recordPlayed(g_games[sel].key); storeSet(g_global,"EmuCore/DiscPath",g_games[sel].path.c_str()); launchKey=g_games[sel].key; launch=true; running=false; }
              else sel=hit;                       // first tap selects, second launches
            }
          } else {                                 // footer glyph tapped -> reuse the button handler
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN;
            switch(fa){
              case FA_LAUNCH:   a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break;
              case FA_SORT:     a.cbutton.button=SDL_CONTROLLER_BUTTON_X; SDL_PushEvent(&a); break;
              case FA_OPTIONS:  a.cbutton.button=SDL_CONTROLLER_BUTTON_START; SDL_PushEvent(&a); break;
              case FA_SETTINGS: a.cbutton.button=BTN_SETTINGS; SDL_PushEvent(&a); break;
              case FA_PAGEL:    sel=gridPage(sel,-1,cols,rows,n); break;
              case FA_PAGER:    sel=gridPage(sel,+1,cols,rows,n); break;
              case FA_QUIT:     running=false; break;
            }
          }
          top=n?(sel/(cols*rows))*rows:0;
          if(!running) break;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int n=(int)g_games.size();
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=gridNav(sel,-1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=gridNav(sel,+1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=gridNav(sel,0,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=gridNav(sel,0,+1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  sel=gridPage(sel,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: sel=gridPage(sel,+1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_X:              // Y (Nintendo): cycle listing order
          if(n){
            std::string keep=g_games[sel].key;
            g_sort=(g_sort+1)%SORT_COUNT;
            char sb[8]; snprintf(sb,sizeof(sb),"%d",g_sort);
            storeSet(g_global,"Wrapper/SortMode",sb); storeSave(g_global,LAUNCHER_INI);
            applySort();
            sel=0; for(int i=0;i<n;i++) if(g_games[i].key==keep){ sel=i; break; }  // keep selection
          }
          break;
        case BTN_CONFIRM:
          if(n){ recordPlayed(g_games[sel].key); storeSet(g_global,"EmuCore/DiscPath",g_games[sel].path.c_str()); launchKey=g_games[sel].key; launch=true; running=false; }
          break;
        case SDL_CONTROLLER_BUTTON_START:
          if(n){ int r=perGameMenu(g_games[sel],pad);
            if(r==1){ recordPlayed(g_games[sel].key); storeSet(g_global,"EmuCore/DiscPath",g_games[sel].path.c_str()); launchKey=g_games[sel].key; launch=true; running=false; }
            else if(r==2){ scanGames(gameDir); sel=0; top=0; } }   // game deleted -> re-scan
          break;
        case BTN_SETTINGS: {                       // X: settings (incl. the game folder)
          std::string oldDir=gameDir;
          g_active=&g_global; runSettingsRoot(pad,nullptr);
          storeSave(g_global,LAUNCHER_INI);        // persist now -- don't rely on a clean exit
          std::string newDir=storeGet(g_global,"Wrapper/GameDir",gameDir.c_str());
          if(newDir!=oldDir && !newDir.empty()){ gameDir=newDir; scanGames(gameDir); sel=0; top=0; }
          break;
        }
        case BTN_CANCEL: running=false; break;
      }
      top = n ? (sel/(cols*rows))*rows : 0; // page-aligned: a page turn swaps both rows
    }
    char label[300]; snprintf(label,sizeof(label),"Folder: %s", toEmu(gameDir).c_str());
    renderGrid(sel,top,label);
    // warm the rest of the library into RAM with any leftover per-frame budget so
    // navigating to an unvisited page is instant (no eviction -> no reloading).
    for(auto &g:g_games){ if(g_cover_budget<=0) break; ensureCover(g); }
    SDL_Delay(8);
  }

  // persist the launcher's own global store (its canonical file; the emulator
  // never reads/writes launcher.ini, so per-game overlays can't pollute it).
  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global, LAUNCHER_INI);

  // Prepare the chainload while SDL is still up (so an extraction failure can be
  // shown). Extract the chosen core + renderer from romfs to SD (atomic/checked),
  // build the emulator's nethersx2.ini = global + this game's per-game overrides
  // + the resolved core path; only chainload if BOTH extracted cleanly.
  bool willChain = false;
  std::string emuNro;
  if(launch && envHasNextLoad()){
    std::string build = storeGet(g_global,"Wrapper/CoreBuild","4248");
    if(build!="4248" && build!="3668") build="4248";                 // only bundled builds
    std::string rtag = strcmp(storeGet(g_global,"EmuCore/GS/Renderer","14"),"12")==0 ? "gl" : "vk";
    mkdir(DATA_DIR,0777); mkdir(CORES_DIR,0777);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);   // clock up: extraction is faster
    std::string csrc="romfs:/cores/emucore_"+build+".so";
    std::string cdst=std::string(CORES_DIR)+"/libemucore_"+build+".so";
    std::string esrc="romfs:/emu/NetherSX2_nx_"+rtag+".nro";
    std::string edst=std::string(DATA_DIR)+"/NetherSX2_nx_"+rtag+".nro";
    emuNro="sdmc:/switch/nethersx2/NetherSX2_nx_"+rtag+".nro";
    bool haveCore=extractFromRomfs(csrc.c_str(),cdst.c_str());
    bool haveEmu =ensureEmu(esrc.c_str(),edst.c_str(), rtag);   // re-extract only on a launcher rebuild
    bool haveRes =ensureResources(build);   // shaders/GameIndex for THIS build (or black screen)
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);     // back to normal before chainload
    if(haveCore){
      std::string v="/switch/nethersx2/cores/libemucore_"+build+".so";
      storeSet(g_global,"Wrapper/CoreSo",v.c_str());
    }
    if(!launchKey.empty()){                                          // overlay per-game overrides
      std::string gp=std::string(GAMECFG_DIR)+"/"+launchKey+".ini";
      Store og; storeLoad(og, gp.c_str());
      for(auto &e:og.kv) storeSet(g_global,e.k.c_str(),e.v.c_str());
    }
    storeSave(g_global, EMU_INI);
    if(haveCore && haveEmu && haveRes) willChain=true;
    else if(!haveRes) { toast("Could not extract NetherSX2 resources (SD full?)"); SDL_Delay(1800); }
    else { toast("Could not extract emulator files (SD full?)"); SDL_Delay(1800); }
  }

  // teardown
  for(auto &g:g_games) if(g.cover) SDL_DestroyTexture(g.cover);
  if(g_logo) SDL_DestroyTexture(g_logo);
  if(g_font) TTF_CloseFont(g_font);
  if(g_font_sm) TTF_CloseFont(g_font_sm);
  if(g_font_big) TTF_CloseFont(g_font_big);
  plExit();
  if(pad) SDL_GameControllerClose(pad);
  IMG_Quit(); TTF_Quit();
  SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
  griddb_global_exit();

  if(willChain) envSetNextLoad(emuNro.c_str(), emuNro.c_str());
  return 0;
}
