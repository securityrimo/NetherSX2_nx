#!/usr/bin/env bash
# Build the whole NetherSX2 Switch app from this one project folder:
#   - the emulator (this dir) in both renderers -> NetherSX2_nx_{vk,gl}.nro
#   - bundle those + the two cores into the launcher addon's romfs
#   - the SDL launcher (launcher/) -> the single all-in-one NetherSX2.nro
#
# Run from an msys2 devkitPro login shell:
#   /c/msys64/usr/bin/bash -lc './build_all.sh'
set -e
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64

APP="$(cd "$(dirname "$0")" && pwd)"   # the emulator project (this folder)
ROOT="$(dirname "$APP")"               # parent, holding the extracted core APKs
# Where the extracted NetherSX2 core APKs (NetherSX2-v2.2n-{4248,3668}/) live.
# Defaults to the parent folder; override with CORES_DIR=... for a different layout.
CORES_DIR="${CORES_DIR:-$ROOT}"

echo "==== emulator: Vulkan (NVK) ===="
cd "$APP"
make clean >/dev/null 2>&1
make RENDERER=VK
cp -f NetherSX2_nx.nro NetherSX2_nx_vk.nro

echo "==== emulator: OpenGL ===="
make clean >/dev/null 2>&1
make
cp -f NetherSX2_nx.nro NetherSX2_nx_gl.nro

echo "==== bundle cores + emulator binaries into the launcher romfs ===="
mkdir -p "$APP/launcher/romfs/cores" "$APP/launcher/romfs/emu"
cp -f "$CORES_DIR/NetherSX2-v2.2n-4248/lib/arm64-v8a/libemucore.so" "$APP/launcher/romfs/cores/emucore_4248.so"
cp -f "$CORES_DIR/NetherSX2-v2.2n-3668/lib/arm64-v8a/libemucore.so" "$APP/launcher/romfs/cores/emucore_3668.so"
cp -f "$APP/NetherSX2_nx_vk.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_vk.nro"
cp -f "$APP/NetherSX2_nx_gl.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_gl.nro"

# Per-build resources (assets/: shaders, GameIndex.yaml, fonts, cheats, ...). These
# DIFFER between the two v2.2n builds (esp. the GS shaders), so pack BOTH sets and
# let the launcher extract the one matching the chosen core. Only v2.2n is bundled
# (no v2.1). Drop the Android ART profiles (dexopt/) -- useless on Switch.
echo "==== bundle per-build resources into the launcher romfs ===="
for b in 4248 3668; do
  rd="$APP/launcher/romfs/res/$b"
  rm -rf "$rd"; mkdir -p "$rd"
  cp -rf "$CORES_DIR/NetherSX2-v2.2n-$b/assets/." "$rd/"
  rm -rf "$rd/dexopt"
done

echo "==== forwarder stub (built in-tree from launcher/fwd/) ===="
make -C "$APP/launcher/fwd" clean >/dev/null 2>&1
make -C "$APP/launcher/fwd"

echo "==== launcher (SDL2 addon) ===="
cd "$APP/launcher"
make clean >/dev/null 2>&1
make

# Leave a single .nro in the project root and drop the intermediate emulator
# builds (they're already baked into the launcher's romfs) -- so there's exactly
# one file to copy to the SD card.
mv -f "$APP/launcher/NetherSX2.nro" "$APP/NetherSX2.nro"
rm -f "$APP/NetherSX2_nx.nro" "$APP/NetherSX2_nx_vk.nro" "$APP/NetherSX2_nx_gl.nro"

echo
echo "Done. The only file to copy:"
ls -la "$APP/NetherSX2.nro"
echo
echo "SD layout:  sdmc:/switch/NetherSX2.nro  +  /switch/nethersx2/{bios,resources,games}/"
