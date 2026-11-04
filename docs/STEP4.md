# Step 4: menus, HUD, VR fixes, performance, installer

Status and design notes for the polish step. Findings come from the class
dump of the shipping game (`hole-vr/dump/game_classes.txt`).

## Done in this branch

- **Installer** (`installer/`, builds `HOLE-VR-Setup.exe`). One exe with the
  mod embedded. Finds HOLE through Steam's library list, installs `dxgi.dll`,
  backs up any other `dxgi.dll` (ReShade etc.) and restores it on uninstall,
  refuses to run while the game is open, and can move the `hole-vr` settings
  folder to the Recycle Bin. Also scriptable:
  `HOLE-VR-Setup.exe /install|/uninstall [/purge] [/path=<HOLE folder>] [/quiet]`.
- **Version resource** on `dxgi.dll` (ProductName "HOLE VR"), so the
  installer can tell our DLL from other mods that also use the name.
- **Render fixes** (`src/vr/render_fixes.*`). Every second, every camera,
  post-process component and post-process volume gets these forced to a
  neutral value with their override flag set, so they lose however the engine
  blends them:
  motion blur, depth of field, vignette, lens flares, chromatic aberration,
  film grain. The player's `SC2D_PlayerCamera` scene capture (the game's
  "Capture2D" option) is stopped, because it renders the level a second time.
  Each fix has its own switch in `hole-vr.ini`; new settings are appended to an
  existing ini automatically.

- **Menus as panels in the world** (`src/vr/world_menu.*`). While a menu is
  open (the game shows its mouse cursor), every top-level `UIM_*` widget the
  game puts on the screen is moved into a `WidgetComponent` on a small actor
  we spawn, 1.6 m in front of the head and 1.8 m wide (1920x1080 layout). It
  is rendered by the engine in both eyes. The menu is attached to the panel
  before it is removed from the screen, so the game never sees it destroyed.
  A `WidgetInteractionComponent` is placed 3 cm in front of wherever the
  controller ray hits a panel and traces 6 cm, so nothing else in the world
  blocks it; trigger is the left mouse button, the stick scrolls. Beam and dot
  are OpenXR quad layers. Menus are let go when the game reports them closed
  (`BPI_UIControl.IsOpened`) or when the cursor goes away. `UIM_*` widgets
  already showing during play stay on the flat HUD. Runs from
  `stereo::SetGameFrameCallback` (game thread, every rendered frame, also
  while paused). `menu_mode=window` brings back the flat-screen panel, which
  is also the automatic fallback.
- **Menu zoom, outlines and the HUD on the forearm** (`world_menu.cpp`).
  A menu on the arm is measured once it has been drawn on its panel: each
  widget's cached geometry (where Slate drew it, read through
  `SlateBlueprintLibrary`) gives the part of the 1920x1080 layout that shows
  something, and the panel is zoomed (up to `menu_fit_max_zoom`) so that part
  fills the menu's space on the arm. The menu is see-through for the frame
  or two this takes. Every TextBlock in a menu gets a thin black font outline
  (`menu_text_outline`) so it reads over a bright scene. The in-level HUD
  (`UIM_HUD_C`, from `BP_Player_C.HUD`) goes on its own panel at the menu's
  spot and fades in when the arm faces you (`hud_on_forearm`); the crosshair
  dot and waypoint marker are hidden there. While the death screen is up the
  player's scene capture runs again (`render_fixes::AllowPlayerCapture`) for
  its picture of the killer.
- **Bigger menu text that still fits** (`world_menu.cpp`, `GrowText`).
  Once a menu has been drawn on its panel, every text on screen is measured at
  the game's size, then gets a bigger font: `menu_text_scale` (1.5x), small
  print up to `menu_text_min_px` (32; the death screen's 12-point lines get
  2.5x), titles no bigger than `menu_text_max_px` (64), never more than
  `menu_text_max_scale` (2.5x). Once the engine has laid the menu out again,
  each text on screen is checked: letters that need more room than the text's
  slot (desired size over its local size), a text running past a box around
  it (up to the first ScrollBox, which may scroll) by more than it did at the
  game's size, two texts covering more of each other than they did (as a
  share of the smaller, so stacked glow texts don't count), or a text running
  over a picture beside it in the same box (an icon, a line) or out of one it
  sat inside. Those texts, and every text in a box one ran out of, step back
  by 0.85 until all fit (up to 8 rounds). A text that isn't on screen yet
  (another tab, a line the death screen shows later) stays at the game's size:
  twice a second `TextWatch` measures and grows texts that came into view, and
  checks grown texts again when their words change (desired size moved).
  Every 3 s the menu is searched for texts the game added. Growing texts that
  were never measured left them unchecked, which is what overlapped in the
  level menu and the death screen (2026-11-03). Each menu's texts are logged
  once with their size and place. A narrow menu gets a taller panel of the
  same area (`menu_fit_shape`). The head-in-wall fade never covers a forearm
  menu (`comfort.cpp`).
- **Panel pictures** (`SnapshotPanel`). A second after a menu is fitted (and
  after texts came into view), the panel's own 1920x1080 picture is saved as
  `hole-vr/panel-<menu>-<n>.exr` (OpenEXR: the panel's render target is half-float) through
  `KismetRenderingLibrary.ExportRenderTarget` (about 100 ms each);
  the death screen saves one per page. Each holds the game thread for 81 to
  112 ms (a visible hitch in the headset), so since the 2026-11-04 outside
  review they are off unless `menu_snapshot_diagnostics=1`, and then at most
  `menu_snapshots` (2) per menu per run. The headset picture
  (`capture-forearm-<menu>.bmp`) now waits until you look at the arm.
- **Panel placement** (`PanelOrigin`, 2026-11-03 evening). The panel keeps
  the engine's middle pivot and is moved so FitContent's box sits on the arm.
  `SetPivot` changed where the laser's hit landed in the layout but not the
  picture (the engine had built the panel around the old pivot), so clicks
  landed off by the box's distance from the screen centre: one item low in the
  pause menu, above the task giver's Get Reward buttons. A menu the game fills
  after it opens (the task cards) is fitted again in its first 3 s, and menus
  that fill the whole screen get at least `menu_fit_min_zoom` (1.3), except
  the death screen, which keeps its fitted size and the game's text sizes
  (owner: "they dont need to be bigger"; its grown total ran into Retry).
- **Text checks, second round.** Fully see-through texts are ignored (each
  game button has a large invisible "Lock" text that made the real label shrink).
  Every widget around a text is checked (from a list up, only across it), and
  every picture in the menu: a text mustn't newly cover one, or run out of one
  it sat inside. A check waits until the engine has laid out the new sizes
  (desired heights match), and one more check runs 0.6 s after the texts fit.
- **Text checks, third round** (10:47 run). When the engine's layout never
  matched the expected text heights (credits), the check gave up after 40
  frames and kept every size unchecked: the names ran over the logo and past
  the screen edge. Now it checks the layout as it is, and from then on for that
  menu. Filled borders and buttons (widgets named like one) count as pictures:
  a label inside one stays inside it ("Return Home" ran out of its button under
  the level select's preview), and text clear of a picture at the game's size
  must stay clear by 3 px (no share allowance). A text that runs further off
  the screen is pulled back, and if growing pushed one picture or button into
  another (the death screen's total line under Retry), every grown text steps
  back.
- **Menu button toggles** (`ui_panel.cpp`, `ClosePauseMenu`). With the pause
  menu on top, the menu button runs the pause menu's Return button.
- **No menu blur on the death screen**: the blur smeared the kill shot (a
  scene capture of your body a few metres away).
- **Menu brightness follows the game's exposure** (`UpdateBrightness`,
  `render_fixes::ReadExposure`). The game uses manual exposure on the player's
  post-process (`BP_Player.PostProcess`) and raises its bias in dark areas
  (`Exposure_BaseBrightness`, set per area by `LCBP_LocalPostProcess`). Flat
  menus never see that, but the arm menus are in the world: in a dark area the
  death screen's kill shot came out pure white and its orange buttons pale
  yellow (headset picture, 2026-11-03 10:08 run), while the same menus looked
  right in lit rooms. The menus and the HUD are tinted down by as many stops as
  the exposure is above `menu_full_brightness_at` (-1), at least
  `menu_dimmest` (0.03), times `menu_brightness` (1).
  `menu_follow_exposure=0` turns it off. Each menu that opens logs a
  `menus drawn at ... brightness` line with the exposure settings of every
  camera and post-process object. The first version only dimmed past the
  brightest place seen in the run (`menu_exposure_ref=0`,
  `menu_brightness_min=0.1`; those lines stay in players' ini files, unread).
  That wasn't enough (11:00 run): the exposure is about +1.1 stops in lit rooms
  (bias 11, physical camera EV100 9.9) and +2.6 in the dark death area (bias
  12.5), and the headset picture showed the panels about four times too bright
  even in lit rooms (the level select's brown Enter button came out yellow; a
  panel value of 0.55 showed as 0.94). Measured against the panel pictures,
  the menus look like their own colours at about 0.4 of a 1:1 exposure, so the
  reference is -1 stops: about 0.23 brightness in lit rooms, 0.08 in the dark.
- **Parts moved clear of pictures** (`SeparateParts`, `WidenRowGaps`;
  `menu_move_parts_apart`). Two overlaps the owner kept seeing are in the
  game's own layout, not from bigger text: the level select's left column
  (LEVEL, OFFICE, Return Home) runs 15 px under the divider line beside the
  preview, and the credits' names row runs over the bottom of the logo, with
  only 30 px between one name and the next role. Once a menu's texts fit,
  each text that crosses a picture it isn't inside (at least 40x40, or a
  divider line at most 6 px thin) moves the widget it belongs to, just below
  the one it shares with the picture, the shortest way clear by its render
  translation: left, right, up or down, at most 120 layout pixels, 16 px clear
  of the picture and of everything else, and on the screen. Parts lined up on
  one edge in the same box move with it (a column stays a column). Rows of
  three or more texts and nothing else get gaps under twice the text height
  widened to that (gaps of 4 px or less are a role and its name, kept). The
  menu is measured for its zoom three frames later, at the new layout. Skipped
  on the death screen. Each move is logged (`ran into ...: moved`).
- **Kill shot** (`WatchDeathScreen`). The death screen's WidgetSwitcher page is
  watched; on each change the kill shot view (`UIP_DeadReview`) is described in
  the log (what hides it, its pictures and where they come from, the
  `LCBP_DeadReviewRoom` camera and its render target). While it shows, a camera
  that only takes pictures when asked is asked four times a second.
- **The hole** (`hole_watch.cpp`). The level menu and the extract results come
  from `LCBP_Hole_C`, which waits `WaitTimeLimit` seconds after you use it
  (`WaitTimeTemp`, `IsPlayerInside` against `AreaRadius`). Its state, countdown,
  distance and fall are logged as `hole:` lines, and both hands tick softly
  each second while it counts (`hole_buzz`).

## Game UI inventory

Menus are UMG widgets added to the viewport, which a headset can't show
correctly. Top-level screens: `UIM_Title`, `UIM_Pause`, `UIM_Option`,
`UIM_WeaponCustom`, `UIM_Status`, `UIM_Task`, `UIM_Results`, `UIM_YesNo`,
`UIM_Loading`, `UIM_AreaSelect`, `UIM_LanguageSelect`, `UIM_Credit`.
They already support gamepad focus navigation through `BPI_UIControl`
(`GetFocusWidget`, `InitializeFocus`), which gives us a thumbstick fallback.

HUD is `UIM_HUD_C` (held at `BP_Player_C.HUD`), made of parts:

| Part | What it shows | VR placement |
|---|---|---|
| `HUDP_Crosshair`, `HUDP_Dot` | crosshair | hidden; aim with the sights |
| `HUDP_Ammo`, `HUDP_ReloadHint`, fire mode | rounds in chamber and mag, jam/bolt hints | wrist panel |
| `SetHUDHP` / `HUDP_DebugHP` | health | wrist panel |
| `HUDP_Interact`, `HUDP_InteractText` | interaction prompts | small panel at the item, else low in view |
| `HUDP_Popup`, `HUDP_OBJ`, `HUDP_SaveProgress` | pickups, objectives, saving | lazy-follow panel in view |
| `HUDP_NavIcon`, `HUDP_Fridge`, `HUDP_CashData` | waypoint, wave progress, money | waypoint in world; others wrist |

`UpdateAmmo(chamber, mag)` and `SetReloadHint(...)` give us the data for the
wrist panel without reading the widgets.

## Plan for world-space panels (needs steps 2 and 3)

1. Move each open screen widget out of the viewport into a `WidgetComponent`
   (spawned through step 3's `ue::Call`), which renders it into its own
   texture at a fixed resolution.
2. Copy that texture into an OpenXR swapchain and submit it as a quad layer
   through step 2's `xr::Listener::AddLayers`. Quad layers are composited by
   SteamVR after the game's image, so text stays sharp and is not blurred by
   anti-aliasing or post-process.
3. Menus: a panel about 1.5 m in front of you when opened, staying put while
   open. Controller laser drives the Slate cursor; trigger clicks.
4. HUD: wrist panel on step 3's `hands::Get(...).wrist` pose, shown when the
   wrist turns toward the face; prompts and popups on a lazy-follow panel.

## Other VR problems found

- Camera shake, recoil camera kick, lean/mantle roll, damage flash timeline and
  ADS FOV zoom: owned by step 3 (comfort).
- `RetroEffect` option and `TL_SelfHitEffect`: likely post-process materials.
  Check in the headset whether either is screen-space; if so add a switch.
- `UIP_BlurBG` (menu background blur) is fine once menus are on panels.

## Performance plan

- Console variables through `KismetSystemLibrary.ExecuteConsoleCommand`
  (step 3's call API), applied from `hole-vr.ini` presets.
- Keep `r.ScreenPercentage` at 100 until step 2 has checked the eye texture
  size on hardware; resolution is then controlled through OpenXR.
- Candidates: Lumen GI/reflections off or low (the game's DGI option),
  shadow quality, TAA instead of TSR, fewer translucency passes.
- Instanced stereo can't be turned on at runtime (its shaders are not in the
  shipped build), so each eye costs a full scene pass. That makes the extra
  scene capture fix above one of the biggest wins.
