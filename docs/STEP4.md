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
