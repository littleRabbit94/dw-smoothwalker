# Nexus page metadata for Smoothwalker - Third Person Camera

Game page: https://www.nexusmods.com/thebloodofdawnwalker (game id 9719, domain
`thebloodofdawnwalker`). Source: `src/` (DLL) and `mod/` (mod folder).
Archive: `Build-Package.py` (version from `ModVersion` in `dllmain.cpp`).

Posted: https://www.nexusmods.com/thebloodofdawnwalker/mods/613 (mod 613). Nexus strips backslashes in the Files-tab description; use forward slashes there.

Reference page: FreeCam - UE4SS (mod 350),
category Utilities, tags Utilities for Modders, Utilities for Players, Camera, Quality of Life,
AI-Generated Content (read 2026-09-16).

## Page fields

| Field | Value |
|---|---|
| Mod name | `Smoothwalker - Third Person Camera` |
| Summary | `A third-person camera that trails your character while turning stays instant, and eases off when you aim. Distance, height, shoulder and FOV per situation. Shoulder swap on V; presets and on/off in ini or Mod Menu. Needs UE4SS and Steam build 25232147.` |
| Version | `0.10.0` |
| Category | `Utilities`, beside FreeCam (mod 350) |
| Tags | `Camera`, `Quality of Life`, `Utilities for Players`, `AI-Generated Content` |
| Adult content | No |
| Requirements | Two entries, either one. UE4SS for Dawnwalker: https://www.nexusmods.com/thebloodofdawnwalker/mods/18 (Vercadi). Note field: `Or Framecore's UE4SS for BoD. Use file Dawnwalker-UE4SS-v1.2.1-rc6-build25232147 (version 1.3).` Current MAIN file on 2026-09-16. UE4SS for BoD: https://www.nexusmods.com/thebloodofdawnwalker/mods/283 (Framecore). Note field: `Or Vercadi's UE4SS for Dawnwalker. Version 2c or 2b, Performance or Compatibility profile.` Its UE4SS.dll is the official RE-UE4SS experimental CI build of 97b7e501 (PE timestamp 2026-09-02 02:02:56 UTC, inside the "Make Experimental Release" run on that commit); tested 2026-09-16 on both profiles. |
| Optional | Dawnwalker Mod Menu: https://www.nexusmods.com/thebloodofdawnwalker/mods/271 (Nexus title "Mod Setting Menu", mmarcussa, 1.0.6.2 on 2026-09-16). Note field: `Adds the in-game settings page. Without it, edit config/smoothwalker.ini.` |
| Description | `nexus-description.bbcode` |

## Files tab

| Field | Value |
|---|---|
| File name | `Smoothwalker` |
| Version | `0.10.0` |
| Category | Main Files |
| File | `dist/Smoothwalker-0.10.0.zip`, 404,578 bytes, built 2026-09-24 |
| Description | `Close the game, then extract into the folder that holds the ue4ss folder (Dawnwalker/Binaries/Win64, the one with Dawnwalker.exe). The archive carries the folder path, so the mod lands in ue4ss/Mods/DWSmoothwalker by itself. Needs UE4SS (mod 18 or mod 283) and Steam build 25232147. No mods.txt edit.` |

Optional file:

| Field | Value |
|---|---|
| File name | `Smoothwalker Example Preset` |
| Version | `0.10.0` |
| Category | Optional Files |
| File | `dist/Smoothwalker-Example-Preset-0.10.0.zip`, 1,382 bytes, built 2026-09-24 |
| Description | `One preset, Over the Shoulder: a closer camera further out over the shoulder, with a quicker follow to match. I commented every key with its range and default, so it doubles as a template for your own. Extract into Dawnwalker/Binaries/Win64 like the main file; it lands in ue4ss/Mods/DWSmoothwalker/config/presets. Restart the game, then pick it from the Mod Menu or with preset in the ini. Needs the main file.` |

Source: `example-preset/`. The build checks it holds a name and all 41 preset keys, each once, inside
the Mod Menu range and on its step.

Second optional file:

| Field | Value |
|---|---|
| File name | `Smoothwalker Example API Mod` |
| Version | `0.10.0` |
| Category | Optional Files |
| File | `dist/Smoothwalker-Example-API-Mod-0.10.0.zip`, 14,659 bytes, built 2026-09-24 |
| Description | `For mod authors. A complete UE4SS Lua mod that uses the Smoothwalker camera API: F9 toggles a zoom layer, F10 a dutch-angle layer, F11 claims the camera and releases it with a glide, and the console command swexample view/layers/owner prints the reads. Extract into Dawnwalker/Binaries/Win64 like the main file; it lands in ue4ss/Mods/SmoothwalkerExample with its own enabled.txt, no mods.txt edit. Read Scripts/main.lua for the game-thread route and the checks for a missing or unloaded Smoothwalker; copy from it freely (GPL-3.0-or-later). Needs the main file. Not for players: remove it when you are done, it binds F9, F10 and F11.` |

Source: `example-consumer/`. The build syntax-checks `Scripts/main.lua` with `luac -p` and ships it with
an empty `enabled.txt` and the LICENSE.

Contents: `dlls/main.dll` (no PDB), `mod_settings.ini`, `config/smoothwalker.ini`, empty
`enabled.txt`, `LICENSE`. No exe or installer. No presets folder is shipped: the mod creates
`config/presets/` at startup, so an update never touches saved slots or installed presets.

## Permissions

| Question | Answer |
|---|---|
| Others can use assets without permission | Yes |
| Others can upload as-is / convert / modify | Yes, with credit |
| Asset use in mods you sell | No |
| Donation Points | Decline |

Credit field: `GPL-3.0-or-later. Built on UE4SS by the UE4SS-RE team (MIT). Runs on Vercadi's and Framecore's UE4SS packages for this game.`

## Images

| Image | File | Uploaded URL |
|---|---|---|
| Page header, 1300x372 | `dist/header-1300x372-coen.png` | (fill in) |
| Thumbnail, 1600x900 | `dist/thumbnail-coen.png` | (fill in) |
| Gallery | Wanted: the same spot on Balanced and on Cinematic; a sprint with distance raised; aiming over the other shoulder | |

Nexus's header upload crops to 1300x372. How the two images were built (sources, fonts and their
licenses, layer positions) is in `art/NOTES.md`, which is not tracked, like the rest of `art/` and `dist/`.

## Changelog tab

Plain text, no BBCode. `Build-Package.py` reads the first version in this block and fails if it
differs from `ModVersion` and `[Mod] Version`: add the new entry on top.

```
0.10.0 - Camera API, combat and traversal follow, debug overlay

- Camera API for other UE4SS Lua mods: a Smoothwalker table in every mod's Lua state with view, live and enabled reads, one offset/rotation/FOV layer per mod, and claim/release so a mod can take the camera over and hand it back with a cut or a glide. Reference in docs/api.md on GitHub; a complete example mod is on the Files tab.
- Follow in combat and in traversal (claw ride, anti-grav, shadowstep): combat_follow, traversal_follow, combat_rotation and traversal_rotation set how much of the trail and the turning smoothing stays while those cameras are up, easing in and out. All default to 100, which is the old behaviour. In presets and on the menu page.
- Plays along with mods that change the game's camera modes. A distance, FOV or look limit another mod writes is kept as the base, Smoothwalker's settings apply on top, and UE4SS.log says which mode and value. Checked before every apply, at unload and for classes loaded late. Before, such a value was overwritten.
- Which camera modes take the look limits (pitch_min, pitch_max) is now fixed by name: the modes the game ships at -60 / 40 (exploring, sprinting, focus, the shadowstep base). Before it was judged on the values read at startup, which another mod could change first.
- Debug overlay: debug_overlay (Debug group on the menu page) or debug_key shows a panel at the top right with the live follow, the game's camera type and modes, the lag, the last snap and the API owner. Off by default. It shows through photo mode and a hidden HUD; switch it off for screenshots.
- Ctrl+R (mod reload) no longer crashes the game with Smoothwalker loaded. Tested with 8 reloads in a row.
- max_lag_h or max_lag_v at 0 switches that axis's trail off entirely.
- Settings keys added by an update are written into an existing config/smoothwalker.ini at startup, so an old file keeps working.
- Logging: log_trace is gone; log_verbose (ini only) adds the detail lines. The default log keeps the load line, your own changes, warnings and errors.
- Updating: close the game and extract over the old folder. The archive replaces config/smoothwalker.ini, so save your look to a slot first, or keep a copy of the file and put it back; slots and installed presets survive.

0.9.0 - Crouch, turning, profiles

- Crouching no longer pops the camera up. The follow tracks the bottom of the capsule, which does not move in a crouch, and the height change is eased at your vertical follow rate, so the dip runs with the game's own timing. The dip still leads the crouch animation when you crouch while stopping; the shipped camera does the same.
- Turning follow speed 1 no longer reverses the camera halfway through a spin: the smoothed turn now trails by at most a quarter turn and always catches up the way you turned.
- Focus mode has its own camera position group (Camera: Focus on the menu page, focus_ keys in the ini and in presets) instead of sharing combat's. Settings files and presets from before take their combat values until you change them.
- Camera distance goes down to 25 % (was 50). Below about 50 the character fades as the camera enters the game's clipping radius, so that is a choice, not a limit.
- Save slots are profiles. Pick a slot marked (empty) to save your settings into it; while that slot is shown, every Apply saves into it. Pick Custom to stop. The separate Save picker is gone. Built-in presets and installed ones never change.
- New ini switch log_trace for per-frame crouch traces in UE4SS.log (off by default, not on the menu page).
- Updating: close the game and extract over the old folder. The archive replaces config\smoothwalker.ini, so save your look to a slot first and copy the file if you want your keys and switches back; slots survive.

0.8.0 - First release

- The camera trails your character and catches up smoothly, horizontal and vertical each at its own rate and response curve, with a soft lag limit. Turning stays instant unless smoothed turning is on.
- While you aim, the trail and any smoothed turning ease down to 30% so the view stays on your crosshair, and ease back afterwards. aiming_follow sets how much stays.
- Stays in front of walls the game pulls its camera in for; snaps back after cutscenes, loads, the free camera and teleports.
- Camera position per situation (exploring, sprinting, combat and focus, aiming, claw ride and anti-grav): distance, height, shoulder offset and field of view. The game's own camera lag is switched off while the mod is on, so the follow is the only lag. Also look up/down limits and a glide time for position changes.
- Presets Tight, Balanced and Cinematic, each with its own follow feel and camera position, plus six save slots you can name and up to 54 presets from other authors dropped into config/presets. A preset carries follow, turning, the look limits and camera position; not the on/off switches, the shoulder side, banners, the snap thresholds or keys.
- V swaps shoulders. Cycling presets and switching the mod on and off can be bound to keys too, and ship unbound, as a blank key means no key. None of them costs a frame: eight presses in a 20-second capture, worst frame 23 ms against an 18 ms average, no stutters.
- Settings in config/smoothwalker.ini apply within a quarter second while the game runs. Optional page for Dawnwalker Mod Menu covering every setting except the key bindings, with a Preset entry that shows which preset your settings match.
- Built for UE4SS 97b7e501: Vercadi's rc6 (mod 18) or Framecore's 2c or 2b (mod 283, Performance or Compatibility profile). Steam build 25232147 only. On any other build the mod stays inactive and says so in UE4SS.log.
```

## Maintenance

- Version: `ModVersion` in `dllmain.cpp`. It also appears in `[Mod] Version` in `mod_settings.ini`,
  the newest changelog entry above (the build checks all three), and in places the build does not
  check: the BBCode log-line `[code]` block and the version rows in the tables above.
- `Build-Package.py` fails when a `ConfigKey` in `mod_settings.ini` is missing from `smoothwalker.ini`
  or appears twice, or a shipped value is outside `Minimum`/`Maximum` or not in `PresetValues`. Any
  of those stops the Mod Menu page opening at all, not just one row.
- It also fails when `main.dll` is older than the newest file in `src/`. A `git
  checkout` touches source mtimes, so after switching branches rebuild before packaging.
- The description's Settings spoiler lists the shipped defaults by hand; after changing a default
  in `smoothwalker.ini`, update it. `aiming_follow` is a setting but not a preset key: it belongs in the
  Settings spoiler and must stay out of the preset authors' key list.
- Voice: the description is first person and short, a hobbyist engineer explaining a utility they built, technical but casual. Say what
  was measured and why a choice was made; no sales language, no em dashes.
- The description links the GitHub repo: make it public (after the hygiene sweep) before posting, or
  drop the "Source" section.
- Game build: the DLL hooks `RebelCameraComponent` vtable slot 214 (`docs/design.md`, "The
  per-frame hook") and refuses to hook when that slot is not overridden. A game patch that moves the
  slot leaves the mod inactive rather than crashing; the build, the page requirements and the Known
  limits line then need the new build number.
- Loader: a new UE4SS release on mod 18 or mod 283 means checking its UE4SS commit (the UE4SS.log
  banner, or the DLL's build time against RE-UE4SS CI runs), rebuilding against it (the RE-UE4SS
  checkout named by `DW_RE_UE4SS_SOURCE_DIR`) and retesting on it, then updating the Requirements rows
  and the description. If the two packages move to different commits, ship one main file per loader.
  Framecore's Performance profile turns off BeginPlay, EndPlay and LoadMap; the player discovery in
  `docs/design.md` "Loader profiles" is what makes that work.
- The config lives in `config/`, not `scripts/config/`: UE4SS starts a Lua mod for any mod folder with a
  `scripts` subfolder and logs a red "main.lua not found" error when there is no script. Do not add a
  `scripts` folder to this mod.
- Log lines in the description are inside `[code]`: `[DWSmoothwalker]` outside one parses as a BBCode tag.
