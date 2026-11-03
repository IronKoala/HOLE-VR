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
  `hole-vr/panel-<menu>-<n>.png` through `KismetRenderingLibrary.ExportRenderTarget`;
  the death screen saves one per page. Each costs a short hitch, so at most
  `menu_snapshots` (2) per menu per run. The headset picture
  (`capture-forearm-<menu>.bmp`) now waits until you look at the arm.
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
