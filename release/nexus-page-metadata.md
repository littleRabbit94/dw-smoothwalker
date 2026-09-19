# Nexus page metadata for SmoothWalker - Third Person Camera

Game page: https://www.nexusmods.com/thebloodofdawnwalker (game id 9719, domain
`thebloodofdawnwalker`). Source: `src/` (DLL) and `mod/` (mod folder).
Archive: `Build-Package.py` (version from `ModVersion` in `dllmain.cpp`).

Not posted yet. Nexus strips backslashes in the Files-tab description; use forward slashes there.

Reference page: FreeCam - UE4SS (mod 350),
category Utilities, tags Utilities for Modders, Utilities for Players, Camera, Quality of Life,
AI-Generated Content (read 2026-09-16).

## Page fields

| Field | Value |
|---|---|
| Mod name | `SmoothWalker - Third Person Camera` |
| Summary | `A third-person camera that trails your character while turning stays instant, and eases off when you aim. Distance, height, shoulder and FOV per situation. Shoulder swap on V; presets and on/off in ini or Mod Menu. Needs UE4SS and Steam build 25232147.` |
| Version | `0.8.0` |
| Category | `Utilities`, beside FreeCam (mod 350) |
| Tags | `Camera`, `Quality of Life`, `Utilities for Players`, `AI-Generated Content` |
| Adult content | No |
| Requirements | Two entries, either one. UE4SS for Dawnwalker: https://www.nexusmods.com/thebloodofdawnwalker/mods/18 (Vercadi). Note field: `Or Framecore's UE4SS for BoD. Use file Dawnwalker-UE4SS-v1.2.1-rc6-build25232147 (version 1.3).` Current MAIN file on 2026-09-16. UE4SS for BoD: https://www.nexusmods.com/thebloodofdawnwalker/mods/283 (Framecore). Note field: `Or Vercadi's UE4SS for Dawnwalker. Version 2b, Performance or Compatibility profile.` Its UE4SS.dll is the official RE-UE4SS experimental CI build of 97b7e501 (PE timestamp 2026-09-02 02:02:56 UTC, inside the "Make Experimental Release" run on that commit); tested 2026-09-16 on both profiles. |
| Optional | Dawnwalker Mod Menu: https://www.nexusmods.com/thebloodofdawnwalker/mods/271 (Nexus title "Mod Setting Menu", mmarcussa, 1.0.6.2 on 2026-09-16). Note field: `Adds the in-game settings page. Without it, edit config/smoothwalker.ini.` |
| Description | `nexus-description.bbcode` |

## Files tab

| Field | Value |
|---|---|
| File name | `SmoothWalker` |
| Version | `0.8.0` |
| Category | Main Files |
| File | `dist/SmoothWalker-0.8.0.zip`, 271,404 bytes, built 2026-09-17 |
| Description | `Close the game, then extract into the folder that holds the ue4ss folder (Dawnwalker/Binaries/Win64, the one with Dawnwalker.exe). The archive carries the folder path, so the mod lands in ue4ss/Mods/DWSmoothWalker by itself. Needs UE4SS (mod 18 or mod 283) and Steam build 25232147. No mods.txt edit.` |

Optional file:

| Field | Value |
|---|---|
| File name | `SmoothWalker Example Preset` |
| Version | `0.8.0` |
| Category | Optional Files |
| File | `dist/SmoothWalker-Example-Preset-0.8.0.zip`, 1,401 bytes, built 2026-09-17 |
| Description | `One preset, Over the Shoulder: a closer camera further out over the shoulder, with a quicker follow to match. I commented every key with its range and default, so it doubles as a template for your own. Extract into Dawnwalker/Binaries/Win64 like the main file; it lands in ue4ss/Mods/DWSmoothWalker/config/presets. Restart the game, then pick it from the Mod Menu or with preset in the ini. Needs the main file.` |

Source: `example-preset/`. The build checks it holds a name and all 37 preset keys, each once, inside
the Mod Menu range and on its step.

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
0.8.0 - First release

- The camera trails your character and catches up smoothly, horizontal and vertical each at its own rate and response curve, with a soft lag limit. Turning stays instant unless smoothed turning is on.
- While you aim, the trail and any smoothed turning ease down to 30% so the view stays on your crosshair, and ease back afterwards. aiming_follow sets how much stays.
- Stays in front of walls the game pulls its camera in for; snaps back after cutscenes, loads, the free camera and teleports.
- Camera position per situation (exploring, sprinting, combat and focus, aiming, claw ride and anti-grav): distance, height, shoulder offset and field of view. The game's own camera lag is switched off while the mod is on, so the follow is the only lag. Also look up/down limits and a glide time for position changes.
- Presets Tight, Balanced and Cinematic, each with its own follow feel and camera position, plus six save slots you can name and up to 54 presets from other authors dropped into config/presets. A preset carries follow, turning, snap thresholds and camera position; not the on/off switches, the shoulder side, banners or keys.
- V swaps shoulders. Cycling presets and switching the mod on and off can be bound to keys too, and ship unbound, as a blank key means no key. None of them costs a frame: eight presses in a 20-second capture, worst frame 23 ms against an 18 ms average, no stutters.
- Settings in config/smoothwalker.ini apply within a quarter second while the game runs. Optional page for Dawnwalker Mod Menu covering every setting except the key bindings, with a Preset entry that shows which preset your settings match.
- Built for UE4SS 97b7e501: Vercadi's rc6 (mod 18) or Framecore's 2b (mod 283, Performance or Compatibility profile). Steam build 25232147 only. On any other build the mod stays inactive and says so in UE4SS.log.
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
- Log lines in the description are inside `[code]`: `[DWSmoothWalker]` outside one parses as a BBCode tag.
