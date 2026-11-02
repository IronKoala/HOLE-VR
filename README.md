# HOLE VR

A native VR mod for **HOLE** (Unreal Engine 5.5), built from scratch without UEVR.

The goal is a game that feels like it was made for VR: proper stereo rendering
through the engine's own stereo path, tracked hands, physical weapon handling
(manual reloads, slide racking, partial racks to check the chamber), hand poses
and full-body IK.

**Status:** step 2 of 4, stereo rendering and head tracking (first headset
build, untested on hardware). See [docs/PLAN.md](docs/PLAN.md) for the roadmap.

## Install (development builds)

1. Copy `dxgi.dll` into `HOLE\HOLE\Binaries\Win64\` (next to `HOLE-Win64-Shipping.exe`).
2. Start SteamVR, put the headset on, then start the game normally from Steam.
3. A `hole-vr` folder appears next to the DLL with `hole-vr.log`, `hole-vr.ini`
   and a `dump` folder.

Keys: **Ctrl+F8** recenters the view, **Ctrl+F9** refreshes the class dump.
Set `vr_enabled=0` in `hole-vr.ini` to play flat with the mod installed.

To uninstall, delete `dxgi.dll` and the `hole-vr` folder.

## Build

Cross-compiles from Linux with MinGW-w64 (also builds with MSVC):

```sh
sudo apt install mingw-w64 cmake
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# output: build/dxgi.dll
```

## How it works

- `src/proxy` makes the mod a `dxgi.dll` the game loads from its own folder.
  Every dxgi export is forwarded to the real System32 DLL.
- `src/ue` finds the engine's name table (`FNamePool`) and object list
  (`GUObjectArray`) by their data layout rather than code signatures, then
  works out reflection offsets by checking them against `Object` and `Actor`.
  That keeps the mod working across game patches.
- `src/vr/stereo.cpp` starts the game with `-emulatestereo`, which makes UE
  create its built-in side-by-side stereo device, then swaps that device's
  virtual functions for ours: headset pose per eye, the headset's asymmetric
  field of view, and its render resolution.
- `src/vr/openxr.cpp` talks to SteamVR through OpenXR (loading the active
  runtime directly) and copies the two halves of the eye texture into the
  headset's swapchains from the game's own `Present`.
- `src/ue/dumper.cpp` writes every class, struct, property and function to
  `hole-vr\dump\`. Press **Ctrl+F9** in a level to refresh it.
