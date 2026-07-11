<div align=center>

<img src="extras/banner.png" alt="Banner" width="30%">

</div>
<h1 align=center>NetherSX2 · Switch Port</h1>

A wrapper/port of NetherSX2 to the Nintendo Switch.
It loads the original Android emulator core `libemucore.so`, 
patches it, and runs it inside a minimal Android-like
environment natively.

Everything ships as a **single `NetherSX2.nro`**: it bundles both emulator cores
(Patched `4248` + Classic `3668`), both renderer backends (OpenGL + Vulkan/NVK)
and each build's data files, and extracts the ones you pick to the SD card at
launch. An SDL cover-art launcher chooses the game, the renderer, and per-game
settings, then chainloads the emulator.

No emulator core, BIOS, or game assets are included in this repository.

### How to install

1. Copy `NetherSX2.nro` into `/switch/` on your SD card.
2. Put your own PS2 BIOS dump into `/switch/nethersx2/bios/`.

The launcher creates the rest of the folder tree on first run:

```
/switch/NetherSX2.nro
/switch/nethersx2/
  bios/         <- your PS2 BIOS dump           (you supply)
  resources/    <- shaders / GameIndex / fonts  (auto-extracted per core)
  covers/       <- cover art (<name>.png)
  launcher.ini  <- the launcher's saved config
```

### Notes

This will not run in applet/album mode — it needs the full memory of a game
override. Launch it by holding **R** while opening an installed title, or use a
forwarder.

A PS2 BIOS dump is required and must come from your own console; the launcher
warns at startup when `bios/` is empty.

Support Vulkan / OpenGL and the core version Patched / Classic

### How to build

Install the devkitPro Switch toolchain and portlibs:

```sh
pacman -S devkitA64 switch-tools libnx switch-sdl2 switch-sdl2_ttf \
          switch-sdl2_image switch-curl switch-mesa switch-libdrm_nouveau
```

The Vulkan renderer links a vendored Mesa NVK build under `vulkan/`. From a
devkitPro msys2 login shell, build everything — both cores, both renderers, and
the launcher — into the single `NetherSX2.nro`:

```sh
./build_all.sh
```

### Credits

* The AetherSX2 / PCSX2 developers for the emulator.
* The NetherSX2 maintainers for the patched Android builds.
* fgsfds for the Switch so-loader groundwork reused here.
* TheOfficialFloW for the original Android so-loader lineage.
* Dantiicu for Switch Vulkan driver.
* Slluxx for IconGrabber.

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with the AetherSX2 or PCSX2 developers. No BIOS,
game images, or the emulator core are distributed here; you must supply your own
BIOS dump and legally-owned game images. We do not condone piracy.

Unless noted otherwise, the source in this repository is under the MIT License
(see LICENSE).
