# Built-in files

Everything in this folder (except this README) is built into dxgi.dll. When the
mod starts, any of these files missing from the player's `hole-vr` folder is
written there, so a fresh install gets the same hand poses, holster spots and
magazine seats as the playtester's PC. A file the player already has is never
touched, so poses they save in the grip editor and holsters they move win.

- `grips/*.ini`: hand poses from the in-headset grip editor (one per gun,
  grab spot and magazine).
- `holsters.ini`: holster spots placed in the headset (VR tab > Move holsters).
- `mag-seats.ini`: where a seated magazine sits in each gun, learned in game.

Not here on purpose: `body-measure.ini` (each player's own arm measurement)
and `hole-vr.ini` (written from the defaults in src/core/config.cpp).

To update: copy the newer file from the playtester's
`HOLE/HOLE/Binaries/Win64/hole-vr` folder over the one here and rebuild.
