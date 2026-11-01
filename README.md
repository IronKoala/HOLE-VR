# HOLE VR

A native VR mod for **HOLE** (Unreal Engine 5.5), built from scratch without UEVR.

The goal is a game that feels like it was made for VR: proper stereo rendering
through the engine's own stereo path, tracked hands, physical weapon handling
(manual reloads, slide racking, partial racks to check the chamber), hand poses
and full-body IK.

**Status:** step 1 of 4, the foundation. The mod loads with the game, finds the
engine's internals and writes a dump of the game's classes. There is no VR
output yet. See [docs/PLAN.md](docs/PLAN.md) for the roadmap.

## Install

Run `HOLE-VR-Setup.exe` and click **Install**. It finds HOLE through Steam,
puts the mod next to the game, and keeps a backup of any other `dxgi.dll`
that was already there (ReShade and similar tools use the same name).
Run it again and click **Uninstall** to remove the mod and restore that backup.

Manual install, if you prefer:

1. Copy `dxgi.dll` into `HOLE\HOLE\Binaries\Win64\` (next to `HOLE-Win64-Shipping.exe`).
2. Start the game normally from Steam.
3. A `hole-vr` folder appears next to the DLL with `hole-vr.log`, `hole-vr.ini`
   and a `dump` folder.

To uninstall, delete `dxgi.dll` and the `hole-vr` folder.

## Build

Cross-compiles from Linux with MinGW-w64 (also builds with MSVC):

```sh
sudo apt install mingw-w64 cmake
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# output: build/dxgi.dll and build/HOLE-VR-Setup.exe (installer with the DLL embedded)
```

## How it works

- `src/proxy` makes the mod a `dxgi.dll` the game loads from its own folder.
  Every dxgi export is forwarded to the real System32 DLL.
- `src/ue` finds the engine's name table (`FNamePool`) and object list
  (`GUObjectArray`) by their data layout rather than code signatures, then
  works out reflection offsets by checking them against `Object` and `Actor`.
  That keeps the mod working across game patches.
- `src/ue/dumper.cpp` writes every class, struct, property and function to
  `hole-vr\dump\`. Press **Ctrl+F9** in a level to refresh it.
