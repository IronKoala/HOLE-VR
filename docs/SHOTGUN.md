# Shotgun: smart reload and loading by hand

Built 2026-11-07, before the owner had the shotgun unlocked, so **none of
this has run in the headset yet**. Spec and owner decisions:
research/shotgun-benelli-m4-beretta-1301.md (sections 4.6 to 4.8, 5.1, 7).

## How it plays

- **Caddy.** With the shotgun out, the magazine pouch (wherever it was placed
  on the body) is the shell caddy. It shows the shells a grab would give,
  crimps down.
- **Smart reload.** Close an empty hand on the caddy and you get 4 shells if
  the tube is missing 4 or more, 2 if it is missing 2 or 3, 1 if it is
  missing 1. Nothing (a short buzz) if the tube is full. The chamber does not
  count. Four are held as a quad (two pairs, the first pair on top under the
  thumb), two as a twin (one above the other), one on its own.
- **Loading.** Bring the shells to the loading port under the receiver, crimps
  towards the muzzle, with the other hand than the one on the grip. When the
  first shell's crimp is near the tube's mouth it catches, and the hand's push
  moves it along the tube. One push loads two: the first clicks in 5.5 cm into
  the push, the second at 12.5 cm. Each click is the game's own shell-in
  sound, a tick in the hand that gets firmer as the tube fills, and one shell
  in the game's tube. After a pair the next pair moves under the thumb for the
  next push. Stop after the first and the second waits at the port.
- **Tube full.** The shell stops 1 cm in with the game's "tube full" sound and
  a firm buzz.
- **Empty gun.** With the bolt locked open the tube is locked (owner's
  choice), and the caddy gives one shell. Drop it into the ejection port on
  the right side; then B (or a full rack) chambers it. Then top off the tube.
- **Letting go.** At the caddy the shells go back. Anywhere else they fall to
  the floor and disappear after a few seconds.
- **Buttons.** A does nothing on the shotgun (the tube is part of the gun).
  B is the bolt release, also with an empty tube.
- **Hand poses.** The hand holding shells has three poses for the grip
  editor: `shells.quad`, `shells.twin` and `shells.single` (`-right` for the
  right hand). Until one is saved the hand stays on the controller.

## Settings (hole-vr.ini)

| Key | Default | What |
|---|---|---|
| `shotgun_smart_reload` | 1 | 0 = always `shotgun_shells_per_grab` (1 to 4) |
| `shotgun_empty_gun_single` | 1 | one shell for the ejection port on an empty, open gun |
| `shotgun_tube_needs_closed_bolt` | 1 | the tube is locked while the bolt is open |
| `shotgun_port_capture_cm`, `_deg` | 4, 35 | how close the crimp must come to the tube's mouth, and how straight |
| `shotgun_push_let_go_cm` | 5 | the hand this far off the tube's line lets go |
| `shotgun_first_shell_cm`, `shotgun_second_shell_cm` | 5.5, 12.5 | where in the push each shell clicks in |
| `shotgun_blocked_cm` | 1 | how far a refused shell goes in |
| `shotgun_ejection_port_cm` | 4 | how close to the ejection port a shell drops in |
| `shotgun_hold_forward_cm`, `_right_cm`, `_up_cm` | 0 | move the shells in the hand |

## How it talks to the game

- One shell in = `OneReloadMaxShell` set to 1, `ReloadShell_TakeNewShell`,
  `ReloadShell_InsertShell_Begin`, `ReloadShell_InsertShell_End`, old value
  back (`game::Gun::LoadOneShell`). The game makes exactly one shell and loads
  it, so the counts, the sound and the lifter are the game's. Nothing is
  loaded if any of these is missing or the game makes other than one shell.
- The tube's mouth is measured at first hold: the front of the lifter
  (`GunParts_ShellCarrier`) at the height of the tube (`MagazineObj`). The
  ejection port is the gun's own `EjectShellDirection`. Both are logged
  (`controls: shotgun BP_SG_C: loading port mouth ...`).
- The shells in the hand and at the caddy are the mod's own actors of the
  gun's `AmmoClass`, with collision off. Their shape (which way the crimp
  points) is measured from the mesh and logged once.
- Pure logic: src/vr/interact/shell_load.* (tests/test_shell_load.cpp).
  Game side: `ShellHandling` and friends in src/vr/controls.cpp.

## First run checklist

1. Log lines `controls: holding BP_SG_C (shotgun ...)`, `controls: shotgun
   BP_SG_C: loading port mouth ...`, `controls: shotgun tube N/M ...` and
   `controls: shell BP_12Gauge_...: 7.0 cm long ...`: are the numbers sane?
2. Does the caddy show shells, and the right number?
3. Do the shells sit in the hand the right way round (brass towards the
   thumb)? If not, the shell's measured axis is backwards.
4. Does a push catch at the port? `controls: shells near the loading port`
   lines say how far off the crimp was.
5. Does each click add one to the tube (`controls: shell 1 of the push in:
   tube N/M`)?
6. Empty gun: port load, then B.
