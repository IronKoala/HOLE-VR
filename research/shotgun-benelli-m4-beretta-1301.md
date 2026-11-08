# VR shotgun reference: Benelli M4 and Beretta 1301

Research for the HOLE VR shotgun, 2026-11-03. The owner has not unlocked the
shotgun yet, so nothing here has been tried in the headset. No mod code yet.

The pistol is built to work like a real Glock 19. This document does the same
groundwork for the shotgun: how the two best-known combat semi-auto shotguns
really work, what HOLE's own shotgun is made of, which VR games load shells
well, and a proposed VR spec.

## Short version

- **HOLE's shotgun is the same kind of gun as the M4 and the 1301.** The game
  files name it `Tombo12` (`BP_SG`). Its parts are a gas tube, a bolt carrier,
  a bolt catch, a shell carrier (the lifter) and a tube magazine with 2, 5, 7 or
  9 shell options. It is a gas-operated semi-auto with a bolt that locks open on
  empty, which is exactly the M4 / 1301 pattern.
- **Both real guns share one operating pattern.** Fire, and gas drives the bolt
  back. The spent hull is thrown out. One shell is let out of the tube onto the
  lifter, and the bolt chambers it on the way forward. After the last shell
  the bolt stays open.
- **Reload from empty is "drop one in the port, hit the release".** The
  manuals for both guns do it this way: lock the bolt open, put one shell
  through the ejection port, press the bolt release, then fill the tube. On the
  M4 the tube cannot even be loaded until the bolt is closed.
- **Racking a loaded semi-auto does not feed the next shell.** Pulling the
  handle on a gun with a shell in the chamber throws that shell out and closes
  on an empty chamber. A shell only comes out of the tube when the trigger has
  dropped the hammer, or when you press the drop lever (M4) or carrier stop
  (1301). This is the biggest difference from a pump gun.
- **Loading the tube is one shell at a time, pushed past a shell stop.** The
  shell goes up through the loading port under the gun and is pushed forward
  until its rim clicks past the stop. If you don't push it far enough, it
  pops back out.
- **VR controls (owner decisions 2026-11-03 applied):** right trigger fires,
  right **B** is the bolt release (same as the pistol's slide release).
  Shells are loaded by hand into the port under the gun. Emergency loads go
  through the ejection port on the right. The charging handle works like the
  pistol slide, with a partial pull to check the chamber.
- **Smart reload (owner, 2026-11-07).** The shell caddy hands you as many
  shells as the tube needs: 4 when it is missing 4 or more, 2 when it is
  missing 2 or 3, and 1 when it is missing 1. Four are held in the
  competition "quad" grip and two in the "twin" grip, and they go into the
  tube **two per push**, the way 3-gun shooters load. See 2.5 and 4.6-4.7.
- **Owner's choices:** racking works like a pump and always loads the next
  shell (unlike the real guns). The tube is locked while the bolt is open.
  Smart reload replaces the old "four per grab". There is no safety. Riding
  the handle does not stop the bolt. See section 7.

## 1. What HOLE's shotgun is made of

The shotgun class is not in the class dump because it has never been loaded on
the owner's PC. These facts come from two places. One is the game's pak index
(`HOLE-Windows.utoc`, asset names only). The other is the class dump of
`BP_GunBase_C`, the parent of every gun, which already has the shell
functions.

### Parts (asset names)

| Part | Asset | What it likely is |
|---|---|---|
| Receiver | `SG_Tombo12_01_Receiver` | frame |
| Bolt carrier | `SG_Tombo12_02_BoltCarrier` | moving bolt group with the charging handle |
| Bolt catch | `SG_Tombo12_03_BoltCatch` | holds the bolt open on empty; the base class has `Bolt(Slide)Catch` lock angles |
| Barrel | `SG_Tombo12_04_Barrel` | |
| Handguard | `SG_Tombo12_07_HandGuard` | support hand goes here |
| Stock | `SG_Tombo12_08_Stock` | |
| Grip | `SG_Tombo12_09_Grip` | pistol grip |
| Trigger, trigger guard | `SG_Tombo12_10_Trigger`, `_11_TriggerGuard` | |
| Gas tube | `SG_Tombo12_12_GasTube` | gas operation, like both real guns |
| Carrier | `SG_Tombo12_13_Carrier` | shell lifter (`GunParts_ShellCarrier`, rotated by `TL_RotateShellCarrier` up to `ShellCarrierOpenAngle`) |
| Slit | `SG_Tombo12_14_Slit` | unknown; check when unlocked |
| Tube magazines | `BPC_Mag_SG_12GA_2R/5R/7R/9R`, meshes `SG_Magazine2R..9R` | fixed tube with 2, 5, 7 or 9 shells (workbench choice) |
| Sights and parts | `BPC_IronSight_SG`, `BPC_DotSight_SG`, `BPC_HoloSight_SG`, `BPC_Scope_2x_SG`, `BPC_Scope_4x_SG`, `BPC_Suppressor_SG`, `SG_Choke`, `SG_MuzzleDevice`, `BPC_WeaponLight_Long_SG`, `BPC_LaserSight_Box_SG` (+ green), `SG_TopRail`, `SG_SideRail_Long` | attachments (see the attachments thread) |

Ammo: `BP_12Gauge_Buckshot`, `BP_12Gauge_HPSlug`, `BP_12Gauge_Flechette`,
`BP_12Gauge_DragonsBreath`, plus an AP shot projectile mesh. Each shell has a
closed and an opened (fired) hull mesh. `BP_AmmoBase_C` has `IsShotShell`,
`ShotShell` and `OpenShotShell` for this.

Sounds the game already has: `S_GunControl_SG001_BoltBack`,
`BoltForward_InAmmo` / `_EmptyAmmo`, `BoltRelease_InAmmo` / `_EmptyAmmo`,
`DryFire`, `Reload_Shell` (a shell going in) and `Reload_Blocking` (a full
tube refusing a shell), `S_GunFire_12GA` (+ suppressed), `DropShell_12GA`.

Animations: `ALS_SG_UClamp_Reload`, `ALS_SG_UClamp_BoltCatchRelease`,
`ALS_SG_UClamp_BoltCharge_B`, `ALS_SG_UClamp_Pose`. "UClamp" is the support
hand cupped under the handguard. The game's own empty reload ends with a
bolt-catch release, like the real guns.

### Shell functions in the gun base class (class dump)

| Function | Notes |
|---|---|
| `Reload_Start`, `OpenShellCarrier`, `RotateShellCarrier(SmoothUp, ResetDown)` | starts a shell reload and tips the lifter |
| `ReloadShell_TakeNewShell` → `Gun_ShellCreate(CreateShellNum)` | creates shells in the hand (`ShellObj_Temp` map, `GetShellNumInHand`) at `AmmoPickHandSocketName`. `OneReloadMaxShell` caps how many per grab |
| `ReloadShell_InsertShell_Begin` / `_End` → `Gun_ShellLoad(out IsLastShell)` | moves one shell from the hand into the tube |
| `DestroyShellsInHand` | cleans up shells left in the hand |
| `Gun_ChargeBolt(IsTriggerReleased)`, `Gun_ReleaseBolt`, `Chambering_BoltCatchRelease` | same bolt calls the pistol uses |
| `BPI_Magazine.ChamberingOneRound`, `CarrierUpShell(loc, rot)` | tube to lifter to chamber |
| State: `AmmoInChamber`, `AmmoObj_InChamber`, `IsBolt(Slide)Stopped`, `IsBoltOpened`, `IsBoltCatchRelease`, `MagazineType`, `BoltType`, `GunMagCapacity`, `GunProjectileAmount` | |

The player has `Reload_TakeNewShell`, `Reload_InsertShell_Begin/End` and
`OpenShellCarrier`, and the weapon manager has `Notify_` versions. These are
the hooks the flat-screen reload animation calls, and the same pattern the
mod already drives for pistol magazines.

## 2. How the real guns work

### 2.1 The pattern both guns share

1. **Fire.** The trigger drops the hammer and the shell fires. Gas is tapped
   from the barrel just ahead of the chamber and pushes a piston, which
   drives the bolt carrier back.
2. **Unlock, extract, eject.** The rotating bolt head turns out of its locking
   lugs. The extractor pulls the spent hull and it is thrown out of the
   ejection port on the right.
3. **Feed.** The firing cycle trips the shell stop (the "carrier latch",
   "retaining tooth" or "shell stop"), so exactly one shell slides out of the
   tube onto the carrier (lifter). The lifter tips it up in line with the
   chamber.
4. **Chamber and lock.** The recoil spring drives the bolt forward. It
   strips the shell into the chamber and the head rotates to lock. The
   hammer stays cocked for the next shot.
5. **Empty.** If the tube is empty, nothing lands on the lifter, and the bolt
   catch holds the bolt open. This is the "slide lock" of a shotgun.

A shell only leaves the tube when the trigger has dropped the hammer, or when
the shooter presses the drop lever (M4) or carrier stop (1301). So working
the handle by hand on a loaded gun throws out the chambered shell but does
not load the next one. Lucky Gunner says the same of the 1301: a shell
reaches the lifter only through the trigger or the carrier stop button, and
ejecting a shell by hand does not load a new one. The Athlon review of the M4
gives the same two ways to chamber from the tube: press the drop lever, or pull
the trigger and then run the bolt.

### 2.2 Benelli M4 (US military M1014)

**Action.** ARGO ("auto-regulating gas-operated"). Two stainless,
self-cleaning pistons sit just ahead of the chamber, fed by two gas ports just
forward of the forcing cone. They push straight back on the bolt carrier, with
no op-rods. The bolt is a carrier plus a rotating head with two lugs that lock
into a barrel extension. It was Benelli's first gas gun, and it won the US
Joint Service Combat Shotgun trial in 1998. No piston swap is needed between
light and heavy loads, though some very light reduced-recoil loads may not
cycle it.

**Size.** 3.82 kg, 470 mm (18.5 in) barrel, collapsible stock on M4/M1014
versions. The tube holds 4 to 7 shells depending on market and tube, and
extension tubes are sold for more.

**Controls**

| Control | Where | What it does |
|---|---|---|
| Safety | Cross-bolt at the **rear** of the trigger guard, reversible | Red ring visible = ready to fire |
| Bolt release ("carrier control button") | Right side of the receiver, just below the ejection port | Closes a locked-open bolt |
| Cartridge drop lever | Sticks out under the frame on the right, just forward of the trigger guard, reachable by the trigger finger | Lets one shell out of the tube onto the carrier. Its red dot shows the hammer is cocked. Pressed before pulling the bolt on an empty tube, it lets the bolt lock open |
| Carrier latch | Inside the loading port | Holds shells in the tube. Pressed with the index finger (gun upside down, carrier pushed in), it releases one shell per press to unload |
| Cocking handle | Right side of the bolt, removable | Racks the bolt |

**Loading (owner's manual).**
- The tube can only be loaded with the hammer cocked, because the carrier
  latch only retains shells then. Athlon adds that the bolt must be fully
  forward to unlock the lifter. In practice you can't fill the tube while the
  bolt is locked open.
- Push each shell fully into the tube and the carrier latch catches it.
- *Direct loading.* Open the bolt, put a shell into the chamber through the
  ejection port, and close the bolt.
- *Indirect loading.* Hold the bolt open, press the drop lever so the first
  shell drops onto the carrier, then let the bolt go to chamber it.
- *Swap the chambered shell* (for example to a slug). Open the bolt so the
  chambered shell is thrown out, then either put the new one in through the
  port, or press the drop lever to feed the next one from the tube.

**Bolt hold-open.** The bolt locks open after the last shell from the tube.
It "can't be locked open as long as there's a shell in the tube" unless the
drop lever is used.

**Unloading.** Open the bolt and the chambered shell is ejected. For the tube,
turn the gun over, push the carrier in, and press the carrier latch once per
shell. Or repeat the drop-lever swap until empty.

**Quirk.** "Cruiser ready" means a full tube, an empty chamber and the hammer
down, done by pulling the trigger on the empty chamber. Racking then feeds,
because the trigger pull has already tripped the release.

### 2.3 Beretta 1301 Tactical

**Action.** B-Link (Blink) gas system: an annular short-stroke piston
around the magazine tube, with an elastic scraper seal that cleans the
cylinder as it cycles (the "B-Link Plus" version). Beretta claims it cycles 36%
faster than other semi-auto shotguns. The bolt head rotates and has two
locking lugs.

**Size.** 4+1 standard, 7-shell tubes on LE / Gen 2 Enhanced versions, and 10
on the Competition Pro.

**Controls**

| Control | Where | What it does |
|---|---|---|
| Safety | Large cross-bolt near the **front** of the trigger guard, reversible | Red ring visible = ready to fire |
| Bolt release | Oversized button on the right side, below the charging handle | Front: closes a locked-open bolt. Rear half (Gen 1), or lifted from the front (Gen 2), with the carrier lifted: lets shells out of the tube to unload |
| Carrier stop button | At the base of / behind the carrier (lifter) | Puts one shell from the tube onto the lifter. With the tube empty it lets the bolt lock back when pulled |
| Shell stop | Inside the mouth of the tube | Holds shells in. Can be pressed directly through the loading port to unload |
| Charging handle | Oversized, right side of the bolt | Racks the bolt |
| Lifter ("Pro Lifter" on Mod.2) | Under the receiver | Older lifters must be pushed up to load the tube. The Pro Lifter rises by itself when the bolt closes |

**Loading (Beretta manual, same order as the M4).** Press the carrier stop,
pull the bolt back until it locks, put the safety on, put the first shell into
the chamber through the ejection port, press the release and let the bolt
close, then load the tube through the loading gate. Each shell is pushed in
until the rim passes the shell stop. If it isn't pushed far enough, it pops
back out (Lucky Gunner). On older lifters a popped-out shell can land on the lifter and block the
tube until the bolt is run.

**Bolt hold-open.** "When the last cartridge has been fired, the breech bolt
remains in the unlocked position, showing that the magazine is empty." The
emergency reload is a port load followed by the bolt release.

**Unloading (manual).** Press the carrier stop, safety on, pull the bolt back
to eject the chambered shell. Lift the carrier and press the release once per
shell to let each one out. Decock by holding the bolt about 2 cm back and
pulling the trigger.

**Quirks worth simulating or avoiding**
- *Shell dump.* Pressing the bolt release on a loaded gun without moving the
  carrier lets two shells out onto the carrier at once. The gun jams until
  cleared by hand. Shrouds and the Gen 2 release exist to stop this.
- *Riding the handle.* Fingers in the path of the bolt handle can stop the bolt
  short of closing. Keep the hand clear and let it slam.
- *Blocking the lifter.* The lifter sticks out under the receiver. A hand or
  rest pressing on it stops the next shell feeding.
- *Ghost loading.* A ninth shell can be squeezed onto the lifter, behind a
  chambered shell, with a full tube.

### 2.4 Side by side

| | Benelli M4 | Beretta 1301 Tactical |
|---|---|---|
| Gas system | ARGO, two pistons beside the chamber | B-Link, annular piston around the tube |
| Bolt | Rotating head, 2 lugs | Rotating head, 2 lugs |
| Safety | Cross-bolt, rear of trigger guard | Cross-bolt, front of trigger guard |
| Bolt release | Button, right side below the port | Oversized button, right side below the handle |
| Feed one shell by hand | Cartridge drop lever (front right of the trigger guard) | Carrier stop button (base of the lifter) |
| Unload the tube | Carrier latch in the loading port | Rear of the bolt release (carrier lifted), or the shell stop in the port |
| Locks open on empty | Yes | Yes |
| Reload from empty | Port load, then bolt release, then tube | Same |
| Racking a loaded gun | Ejects, does not feed | Ejects, does not feed |
| Tube | 4-7 (+ extensions) | 4-7, 10 on Competition |

### 2.5 How competition shooters load: twin ("load two") and quad

The owner asked to start the reload from how 3-gun competitors load. They
load shells in **pairs**, two per push, from belt caddies:

- **Caddies.** Belt-mounted carriers hold shells in pairs stacked one above
  the other. Quad caddies hold blocks of four (two pairs), usually worn on
  the belt at about 45°, all shells facing the same way. Right-handed
  shooters wear them with the brass towards the left side of the body
  (Athlon, Gun Carrier).
- **Twin grip ("load two").** Two shells stacked two-high. The top shell is
  held by its brass and the lower one by the hull near the crimp. The thumb
  sits on top of the brass of the top shell and the fingers curl round both
  to keep them lined up (Outdoor Life). This is the owner's second photo.
- **Quad grip.** Four shells as two stacked pairs, wrapped by the fingers
  with the thumb on the brass of the top shell of the first pair, "since
  those will go in first". Athlon says to think of it as two dual loads. This is
  the owner's first photo. Guns.com describes the same block split in the
  hand: two shells between the fingers and two in the palm and thumb.
- **The gun.** The loading port is turned to face up or towards the loading
  hand. For weak-hand loading the gun is rolled clockwise and pinned under
  the strong arm or over the strong shoulder, with the strong hand still on
  it. Strong-hand loaders hold the gun by the fore-end instead (Guns.com, Gun
  Carrier).
- **The push.** Crimp end first, shells as parallel to the tube as possible.
  One firm, fast forward sweep across the port with slight downward pressure
  drives the whole pair into the tube: the first shell goes in and the second
  follows it in the same stroke. The side of the thumb pushes the last brass
  in, and the hand carries on past the port like a throw. "Do not follow your
  thumb into the tube" or it gets pinched (Athlon, Gun Carrier, Outdoor Life).
- **Second pair.** The fingers keep the second pair curled while the first
  goes in. Then the thumb moves to its brass and the same sweep loads it
  (Athlon, Guns.com).
- **Mistakes.** A tentative push leaves the second shell half in the tube.
  Letting the pair spread apart or tilt makes the load fail. Dropped shells are
  normal while learning ("yard sales"). Competition guns have widened,
  chamfered loading ports to make this easier.
- **Pace.** A practiced shooter dismounts, loads 10 shells, remounts and
  fires an aimed shot in under 10 seconds, about 8 with practice (Outdoor
  Life).

## 3. VR games that load shells well

| Game | What it does well | Source |
|---|---|---|
| Half-Life: Alyx | Each grab from the backpack gives **two** shells, so a 7-shell reload takes 4 reaches instead of 7. A distinct **"tube full"** sound stops you wasting shells. Custom hand pose for pushing a shell in, with smooth pose blends | Road to VR "Best Shotguns in VR" pt 2; Combine OverWiki |
| Arizona Sunshine 2 | You must actually **slide** the shell in, not just touch it to the gun. Generous on position. Haptics only in the hand doing the work. Criticised for having no full-tube cue | Road to VR pt 1 |
| H3VR (owner has it) | Shells go in one at a time through the gate. Chamber loading through the port is normal. Several shells can be **palmed** in one hand (touch another shell + button) and loaded one after another. Low-pressure shells like Dragon's Breath may fail to cycle a semi-auto | H3VR wiki "gun basics"; Steam discussion |
| F.E.A.R. VR (owner's study) | A shell in the support hand has **priority** over the fore-end grip, and partial pulls are cancelled by letting go or by tracking loss | VR Systems Reference 07 (FE-C controls) |
| RE2 VR scripts (owner's study) | Separate pump and shell settings. The game's own reload animation is **suppressed** so it can't finish while the hand is still holding a part | VR Systems Reference 07 |
| Max Payne 2 VR (owner's study) | Has to arbitrate support grip against slide or pump grab, the same problem we have on the pistol | VR Systems Reference 13 |

Lessons we take: give more than one shell per grab, add a clear "tube full"
cue, make the player push the shell in (with forgiving alignment), give the
held shell priority over the support grip, and never let the game's
animation fight the hands.

## 4. Proposed VR spec for HOLE's shotgun

Same rules as the pistol. The gun works like the real thing and there are no
canned reload animations. Either hand can do the support-hand jobs. No
body-motion gestures: pushing a shell into the gun is handling an object,
like seating a magazine, not a gesture. All distances are starting values
for the ini and need tuning in the headset.

### 4.1 What the mod tracks

The bolt position (0 = closed, 1 = fully back), whether the bolt is locked
open, the chamber (empty, live or fired), the lifter (empty or holding a
shell), and the tube count (the game's). The owner chose pump-like feeding,
so no "feed armed" flag is needed: every full bolt cycle feeds from the tube
if it has a shell.

### 4.2 Holding and firing

- Right hand on the pistol grip. Left hand on the handguard (the game's
  "U-clamp"). This reuses the rifle's two-hand support grip and virtual stock.
  Shotgun recoil values are heavier, and recoil never moves the camera.
- Right trigger fires one shell per press. Bolt cycle and feed follow 2.1.
  On the last shell the bolt locks open with the `BoltRelease_EmptyAmmo` /
  `BoltForward_EmptyAmmo` sounds.
- Trigger on an empty chamber: `DryFire` click.
- No manual safety (owner decision). The game has none either.

### 4.3 Charging handle

- Grab within 4 cm of the handle with either hand (normally the left,
  reaching under or over the receiver). The hand is locked to the bore axis
  and travel is the game's `Bolt(Slide)_BackLength`.
- Partial pull (30-90%) is a **chamber check**: you see the brass of the
  chambered shell and nothing changes when you let go.
- Full pull (90% or more) throws out the chambered shell (live shells are
  thrown out unfired). On the way forward:
  - If the tube has a shell, it goes onto the lifter and is chambered, like
    a pump gun. (Owner decision. A real M4 or 1301 would close on an empty
    chamber here, see 2.1.)
  - If the tube is empty, the bolt locks open.
- Letting the handle ride forward slowly still closes the bolt fully (owner
  decision: the real guns' "stops short" quirk is off).

### 4.4 Bolt release (right B)

B closes a locked-open bolt. If a shell is on the lifter or in the chamber, it
chambers with `BoltRelease_InAmmo`. Otherwise the bolt closes on empty. Same
button as the pistol's slide release.

### 4.5 Right A

Not used on the shotgun. Because racking always feeds, the M4's drop lever
(or the 1301's carrier stop) has no job to do, so it is left out.

### 4.6 Smart reload: shells in the hand

- **Where from.** A shell caddy on the belt, default left hip in front of the
  magazine pouch, angled about 45° like a competition quad caddy, and movable
  with the holsters system. It is always visible and can be grabbed any time.
- **How many (owner, 2026-11-07).** When the empty hand closes on the caddy,
  the mod checks how many shells the tube is missing (capacity minus shells
  in the tube; the chamber doesn't count):

  | Tube missing | You get | Held as |
  |---|---|---|
  | 4 or more | 4 | quad grip (owner's photo 1) |
  | 2 or 3 | 2 | twin grip (owner's photo 2) |
  | 1 | 1 | single shell |
  | 0 | nothing | short buzz, the tube is full |

  On a 2-shell tube a full reload is one twin. On a 9-shell tube it is
  quad, quad, then a single.
- **Empty gun, bolt locked open (my addition).** The tube is locked until the
  bolt closes (decision 2), so a quad would be useless here. The caddy gives
  **1** shell for the port load. After B closes the bolt on it, the next
  grabs follow the table. Say if you'd rather always get the table's number.
- **Reserve.** Never more than you have left. If the reserve is short, you
  get the largest of 4, 2 or 1 that it covers (3 left gives a twin, then a
  single).
- **The count is taken at the grab.** The caddy only gives shells to an empty
  hand. Opening the hand at the caddy puts held shells back.
- **Letting go away from the gun** drops the shells. They return to reserve
  after 2 seconds, so nothing is lost (`shotgun_dropped_shells_return=1`).
- **Hold layouts.** The shells sit in the hand as in the photos: brass
  towards the thumb, crimps out past the little-finger side, with the pair
  that goes in first under the thumb. The owner sets the finger poses for
  quad, twin, single, thumb push and port drop in the in-headset editor, like
  the magazine poses. Where the shells sit in the hand is data the owner can
  nudge too.
- Settings: `shotgun_smart_reload=1` (0 = always 4),
  `shotgun_empty_gun_single=1`.

### 4.7 Loading the tube

- **Loading port.** The opening under the receiver, in front of the trigger
  guard, learned at pickup from `GunParts_ShellCarrier` and the tube's attach
  point.
- **Two per push.** The pair under the thumb (or the single shell) is the
  one that loads. A quad is two pairs and a twin is one pair. A single loads
  on its own.
- **Capture.** The leading shell's nose comes within 3 cm of the port mouth,
  base towards the stock, and the shells within 35° of the tube axis. From
  then on the hand only moves the pair along the tube axis (it is released if
  the hand strays 5 cm off the axis).
- **One sweep loads the pair.** Push forward and the leading shell clicks in
  at 5.5 cm (a 70 mm shell's rim past the stop). The second shell drops into
  the port behind it and clicks in at 12.5 cm in the same stroke. Each shell
  plays `Reload_Shell`, gives a haptic tick in the pushing hand, and puts one
  shell into the game's tube (`ReloadShell_InsertShell_Begin/End`), so a pair
  is two quick ticks. The hand can carry on past the port like the
  competition "throw". The shells stay in the tube and the thumb never gets
  pinched.
- **Second pair.** After the first pair of a quad goes in, the second pair
  moves under the thumb on its own, ready for the next sweep. A quad is two
  sweeps.
- **Stopping halfway.** If the sweep stops after the first shell, the second
  shell stays in the hand at the front, ready for its own push. This is
  kinder than a real gun, where it would be left half in the tube. If you let
  go before the first shell clicks, that shell pops back out and drops (real
  shell-stop behaviour). Pull back while still holding and the shells come
  out with the hand.
- **Full tube.** A shell that doesn't fit stops after 1 cm with
  `Reload_Blocking` and a firmer buzz: the "tube full" cue. If only one slot
  is left during a pair, the first shell goes in and the second blocks and
  stays in the hand. Smart reload makes this rare.
- **Gun angle.** Roll the gun so the port faces the loading hand, as
  competitors do. Capture works in the gun's own frame, so any roll works.
  The default is weak-hand loading (left hand loads, right hand keeps the
  gun). Strong-hand loading needs the gun held by the fore-end in the left
  hand, which the mod doesn't support yet.
- **Bolt locked open.** By default (`shotgun_tube_needs_closed_bolt=1`) the
  lifter is locked and the tube refuses shells, as on the M4. You port-load
  first. Set it to 0 to allow tube loading with the bolt open.
- **Spring feel.** Haptic strength on each push rises with the number of
  shells already in the tube.

### 4.8 Emergency reload from empty (port load)

With the bolt locked open, bring a shell within 4 cm of the **ejection port
on the right** and push it in or let it go there. It drops onto the lifter
and shows in the open action. Press B (or pull the handle fully back and let
go) and the bolt slams it home. Then top off the tube through the bottom. To
make this easy with the gun in the right hand, roll the gun left so the port
faces up, as real shooters do.

### 4.9 Unloading

- **Chamber.** A full pull on the handle throws the shell out. It can be
  caught or picked up.
- **Tube.** Put the empty left index finger into the loading port and press
  the **left trigger**: one shell comes out into the left hand per press
  (the carrier latch). Inside the port this wins over the weapon light.

### 4.10 Which job the left hand does

Fixed order, and a job is kept until the grip is released (the lesson from
Max Payne 2 VR and F.E.A.R. VR):

1. Holding shells: loading port or ejection port only. Never the support grip.
2. Charging handle (hand within 4 cm of it).
3. Carrier latch (empty index in the loading port + left trigger).
4. Support grip on the handguard.
5. Weapon light (left trigger near the gun, as now).

### 4.11 Feedback

| Event | Sound (game's own) | Haptics |
|---|---|---|
| Shell clicks into the tube | `Reload_Shell` | short tick, pushing hand |
| Tube full | `Reload_Blocking` | firm buzz, pushing hand |
| Bolt back / forward | `BoltBack`, `BoltForward_InAmmo` / `_EmptyAmmo` | handle hand |
| Bolt release | `BoltRelease_InAmmo` / `_EmptyAmmo` | both hands, strong |
| Shot | `S_GunFire_12GA` | both hands, gun hand stronger |
| Dry fire | `DryFire` | gun hand, light |
| Spent hull | `DropShell_12GA` on landing | none |

### 4.12 Controls with the shotgun in hand

| Input | Does |
|---|---|
| Right trigger | Fire |
| Right A | Not used |
| Right B | Bolt release |
| Right stick click | Kick (unchanged) |
| Left grip on handguard | Support hold, virtual stock |
| Left grip on charging handle | Rack, chamber check |
| Left grip at shell caddy | Smart reload: take 4, 2 or 1 shells |
| Left trigger, index in loading port | Carrier latch: take one shell out |
| Left trigger near the gun elsewhere | Weapon light (unchanged) |

### 4.13 Not in the first version

Ghost loading (an extra shell on the lifter), the shell-dump malfunction,
mixed ammo types in one tube (HOLE loads one ammo type per gun from the
workbench), Dragon's Breath failing to cycle, decocking with the bolt held
2 cm back, and the M4's "can't load the tube with the hammer down" edge case.
All are possible later if the owner wants them. Left out by owner decision:
a manual safety, the drop lever, the real "racking doesn't feed" rule, and
riding the handle stopping the bolt short.

## 5. How it hooks into the game

The same approach as the pistol magazine:

- **Taking shells (smart reload):** read `GetCurrentAmmoInfo`
  (`AmmoInMagazine`, `MagazineCapacity`) to get the missing count, pick 4, 2
  or 1, then `Gun_ShellCreate(N)`. `ReloadShell_TakeNewShell` alone creates
  `OneReloadMaxShell` shells, so it is the fallback if a direct
  `Gun_ShellCreate` call misbehaves. The mod lays the shells out in the quad,
  twin or single hold and attaches those actors
  to the VR hand every frame and checks they still exist each tick (the
  `LiveObject` lesson from the pouch magazines).
- **Each shell in:** `ReloadShell_InsertShell_Begin` + `_End` (`Gun_ShellLoad`),
  with the tube count compared before and after.
- **Port load:** the game has no "put a shell in the chamber" call. Plan:
  load the shell into the tube (`Gun_ShellLoad`) and immediately
  `Gun_ReleaseBolt`, which chambers from the tube. The net result is the same
  and the visible shell is the one in the port.
- **Bolt:** `Gun_ChargeBolt(false)` at the back of the stroke and
  `Gun_ChargeBolt(true)` on release, `Gun_ReleaseBolt` for B, then
  `SetIsBoltCharging(false)`, as on the pistol. The owner chose pump-like
  feeding, which is most likely what the game's `ChargeBolt(true)` already
  does. The weapon lab confirms it. If it doesn't feed, the mod chambers from
  the tube with `BPI_Magazine.ChamberingOneRound`.
- **Lifter:** write the `GunParts_ShellCarrier` rotation directly (0 to
  `ShellCarrierOpenAngle`), or call `RotateShellCarrier`.
- **Animations:** suppress `ALS_SG_UClamp_Reload_Montage`,
  `ALS_SG_UClamp_BoltCatchRelease_Montage` and `ALS_SG_UClamp_BoltCharge_B`,
  so no canned reload animation plays on the VR body (the pistol rule). How
  best to block them is checked in the weapon lab.
- **Unloading the tube:** `BPI_Magazine.ChamberingOneRound` removes one
  round. **Unknown:** how to give it back to the player's reserve. Check
  before promising unload-to-pouch.

### 5.1 What the game's own script answered (2026-11-07)

The script dump of `BP_GunBase_C` (run of 2026-11-03, `logs/run-1402/script/`)
shows what each shell function really does:

- `Gun_ShellCreate` takes **no count**. It makes
  `min(tube capacity - shells in the tube, OneReloadMaxShell)` shells and
  attaches them to the player's hand socket. The shotgun's hand offsets are
  for two shells, so the flat-screen reload most likely takes two at a time.
- `Gun_ShellLoad` loads **every** shell in the hand into the tube at once
  (`LoadSingleAmmo` for each).
- `ReloadShell_InsertShell_Begin` only plays the shell-in sound.
  `ReloadShell_InsertShell_End` runs `Gun_ShellLoad`, moves the lifter and
  tells the weapon manager.
- `ReloadMag_InsertMag_Block` plays the "tube full" sound.
- **There is no spare-shell count.** `Gun_ShellCreate` makes shells from
  nothing (the inventory holds only items and discs), so shells never run
  out and the "reserve" rule in 4.6 has nothing to do.
- A dropped shell (`Ammo_Drop`) turns on its physics and falls.

So the build loads one shell at a time: it sets `OneReloadMaxShell` to 1,
calls `ReloadShell_TakeNewShell` and `InsertShell_Begin/End`, and puts the
old value back. The shells drawn in the hand are the mod's own actors of the
gun's ammo class.

## 6. Checks when the owner unlocks the shotgun

1. Press Ctrl+F9 in a level with the shotgun to refresh the dump. This gets
   the real `BP_SG_C` layout and part components.
2. Log at first pickup: part list and sizes, `Bolt(Slide)_BackLength` and
   `_StopLength`, `ShellCarrierOpenAngle`, `OneReloadMaxShell`,
   `GunMagCapacity`, `MagazineType`, `BoltType`, `GunProjectileAmount`.
3. Weapon lab trace of one flat-screen shell reload from empty and one
   top-off. Record the order of `Reload_Start`, `OpenShellCarrier`, `TakeNewShell`,
   `InsertShell_*` and `BoltCatchRelease`, and whether `ChargeBolt` feeds.
4. For smart reload: answered by the script dump (5.1).
5. Built ahead of time on 2026-11-07 (docs/SHOTGUN.md). The first run's log
   lines starting `controls: shotgun` and `controls: shell` give the numbers
   from checks 2 and 3.

## 7. Owner decisions (2026-11-03, smart reload 2026-11-07)

1. **Feeding: pump-like.** A full rack always throws out the chambered shell
   and loads the next one from the tube, unlike the real M4 and 1301. No drop
   lever on A.
2. **Tube locked while the bolt is open: yes.** From empty you port-load and
   press B, then fill the tube, as both manuals do it.
3. **Shells per grab: smart reload (2026-11-07).** 4 if the tube is missing
   4 or more, 2 if it is missing 2-3, 1 if it is missing 1. Held in the quad
   and twin grips from the owner's photos and loaded two per push. Replaces
   the earlier "four per grab".
4. **Manual safety: none.**
5. **Riding the handle stops the bolt short: off.** The bolt always closes
   fully.

## Sources

Real guns:
- Benelli M4 Tactical owner's manual (English), operating sections pp. 40-78:
  [emanuals.org page 3](https://emanuals.org/en/Benelli-M4-Tactical-Shotgun/3),
  [page 4](https://emanuals.org/en/Benelli-M4-Tactical-Shotgun/4)
- [Athlon Outdoors: Benelli M4 Tactical 12ga review](https://www.athlonoutdoors.com/article/benelli-m4-tactical-12ga/)
- [American Rifleman: Benelli M4 Super 90, Combat Modernity](https://www.americanrifleman.org/content/benelli-m4-super-90-combat-modernity/)
- [Wikipedia: Benelli M4](https://en.wikipedia.org/wiki/Benelli_M4)
- [Benelli USA forum: M1014 "indirect" loading procedure](https://forums.benelliusa.com/topic/25331-m1014-%E2%80%9Cindirect%E2%80%9D-loading-procedure/),
  [procedure to unload an M4](https://forums.benelliusa.com/topic/16774-procedure-to-unload-a-m4/),
  [M4 loading/charging mechanism](https://forums.benelliusa.com/topic/11005-looking-to-better-understand-the-m4-loadingcharging-mechanism/)
- [Beretta semi-auto shotgun user manual (2025)](https://dam.beretta.com/content/dam/beretta-usa/user-manuals/EN-SEMIAUTO-2025.pdf)
- [Beretta 1301 Competition manual p. 14](https://www.manualowl.com/m/Beretta/1301-Competition/Manual/491079?page=14)
- [Beretta: B-Link technology](https://beretta.com/en/technology/products/b-link)
- [Lucky Gunner: Beretta 1301 tips every owner should know](https://www.luckygunner.com/lounge/beretta-1301-tips-every-owner-should-know/amp/)
- [Lucky Gunner: The People's Shotgun, part 2](https://www.luckygunner.com/lounge/beretta-1301-tactical-peoples-shotgun-part-2/)
- [Lucky Gunner: Beretta 1301 Tactical update](https://www.luckygunner.com/lounge/beretta-1301-tactical-update/)
- [Shooting Illustrated: Beretta 1301 Tactical review](https://www.shootingillustrated.com/articles/2017/9/8/review-beretta-1301-tactical-shotgun/)
- [Gun Digest: Beretta 1301 Tactical review](https://gundigest.com/?p=219822)
- [Wikipedia: Beretta 1301](https://en.wikipedia.org/wiki/Beretta_1301)

Competition loading:
- [Athlon Outdoors: shotgun quad loading](https://www.athlonoutdoors.com/article/shotgun-quad-loading/)
- [Guns.com: speed reloads, shotgun quad loads 101](https://www.guns.com/news/speed-reloads-shotgun-quad-loads-101)
- [Gun Carrier: competition shotgun loading techniques](https://guncarrier.com/competition-shotgun-loading-techniques/)
- [Outdoor Life: how to "load two" shotgun shells](https://outdoorlife.com/articles/guns/2014/12/rec-shooting-how-load-two-shotgun-shells)
- Owner's reference photos (2026-11-07): a quad grip with belt quad caddies,
  and a competitor holding a twin

VR games:
- [Road to VR: The Best Shotguns in VR, part 1](https://roadtovr.com/inside-xr-design-best-shotguns-in-vr-what-makes-them-great-feedback/)
  and [part 2](https://roadtovr.com/inside-xr-design-best-shotguns-in-vr-what-makes-them-great-feedback/2/)
- [Combine OverWiki: Shotgun (Half-Life: Alyx)](https://www.combineoverwiki.net/wiki/Shotgun_(Half-Life:_Alyx))
- [H3VR wiki: gun basics](https://redlib.ssps.io/r/H3VR/wiki/index/gunbasics),
  [Steam discussion on palming rounds](https://steamcommunity.com/app/450540/discussions/0/3491891042507347286)
- Owner's VR Systems Reference study: 07-mod-dossiers.md (F.E.A.R. VR, RE2),
  13-late-discovery-dossiers.md (Max Payne 2 VR)

Game files (owner's PC, read 2026-11-03):
- `HOLE\HOLE\Content\Paks\HOLE-Windows.utoc` (asset names)
- `HOLE\HOLE\Binaries\Win64\hole-vr\dump\game_classes.txt` and `objects.txt`
  (`BP_GunBase_C`, `BP_AmmoBase_C`, `BP_MagazineBase_C`, `BPI_Magazine_C`,
  `BP_Player_C`, `BPC_WeaponManager_C`)
