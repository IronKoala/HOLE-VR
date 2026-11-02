# HOLE VR mod

Native VR mod for HOLE (UE 5.5, D3D12). **Never use or depend on UEVR**; the
owner tried it and rejected it. Roadmap and feature list: docs/PLAN.md.

- Build: `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` (needs `mingw-w64`). Output `build/dxgi.dll` and `build/HOLE-VR-Setup.exe`.
- Loaded as a dxgi.dll proxy from `HOLE/Binaries/Win64/`. Logs go to `hole-vr/hole-vr.log` beside it.
- MinGW has no SEH: read game memory only through `mem::Read`/`ReadOr`, which check readability first.
- Find engine data by layout and validate it at runtime; avoid hardcoded addresses and unverified offsets.
- The owner playtests and directs features but does not write code, so explain results in plain language.
