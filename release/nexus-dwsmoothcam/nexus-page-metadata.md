# Nexus page metadata for SmoothCam - UE4SS

Game page: https://www.nexusmods.com/thebloodofdawnwalker (game id 9719, domain
`thebloodofdawnwalker`). Source: `cpp/DWSmoothCam/src/` (DLL) and `ue4ss/DWSmoothCam/` (mod folder).
Archive: `Build-Package.py` (version from `ModVersion` in `dllmain.cpp`).

Not posted yet. Nexus strips backslashes in the Files-tab description; use forward slashes there.

Reference page: FreeCam - UE4SS (mod 350, `release/nexus-dwfreecam/`), category Utilities, tags
Utilities for Modders, Utilities for Players, Camera, Quality of Life, AI-Generated Content (read
2026-09-16).

## Page fields

| Field | Value |
|---|---|
| Mod name | `SmoothCam - UE4SS` |
| Summary | `A smoother third-person camera. It trails your character while turning stays instant, with distance, height, shoulder and FOV per situation. Presets on V, on/off on O, shoulder swap on N. Optional Mod Menu page. Needs UE4SS rc6, Steam build 25232147.` |
| Version | `0.7.4` |
| Category | `Utilities`, beside FreeCam (mod 350) |
| Tags | `Camera`, `Quality of Life`, `Utilities for Players`, `AI-Generated Content` |
| Adult content | No |
| Requirements | UE4SS for Dawnwalker: https://www.nexusmods.com/thebloodofdawnwalker/mods/18 (Vercadi). Note field: `File Dawnwalker-UE4SS-v1.2.1-rc6-build25232147 (version 1.3). The DLL is built against this loader.` Current MAIN file on 2026-09-16. |
| Optional | Dawnwalker Mod Menu: https://www.nexusmods.com/thebloodofdawnwalker/mods/271 (Nexus title "Mod Setting Menu", mmarcussa, 1.0.6.2 on 2026-09-16). Note field: `Adds the in-game settings page. Without it, edit scripts/config/smoothcam.ini.` |
| Description | `nexus-description.bbcode` |

## Files tab

| Field | Value |
|---|---|
| File name | `SmoothCam - UE4SS` |
| Version | `0.7.4` |
| Category | Main Files |
| File | `dist/SmoothCam-UE4SS-0.7.4.zip`, 235,947 bytes, built 2026-09-16 |
| Description | `Close the game, then extract into the folder that holds the ue4ss folder (Dawnwalker/Binaries/Win64, the one with Dawnwalker.exe). The archive carries the folder path, so the mod lands in ue4ss/Mods/DWSmoothCam on its own. Needs UE4SS rc6 (mod 18) and Steam build 25232147. No mods.txt edit.` |

Contents: `dlls/main.dll` (no PDB), `mod_settings.ini`, `scripts/config/smoothcam.ini`, empty
`enabled.txt`, `LICENSE`. No exe or installer. `presets.ini` is not shipped: the mod creates it on
the first slot save, so an update never overwrites saved slots.

## Permissions

| Question | Answer |
|---|---|
| Others can use assets without permission | Yes |
| Others can upload as-is / convert / modify | Yes, with credit |
| Asset use in mods you sell | No |
| Donation Points | Decline |

Credit field: `GPL-3.0-or-later. Built on UE4SS by the UE4SS-RE team (MIT). Requires Vercadi's UE4SS build for this game.`

## Images

| Image | Status |
|---|---|
| Header, 1600x900 | Not made. `release/nexus-dwfreecam/Make-Header.py` is marked retired; capture in game (Steam F12) |
| Gallery | Wanted: the same spot on Balanced and on Cinematic; a sprint with distance raised; aiming over the other shoulder |

## Changelog tab

Plain text, no BBCode. `Build-Package.py` reads the first version in this block and fails if it
differs from `ModVersion` and `[Mod] Version`: add the new entry on top.

```
0.7.4 - First release

- The camera trails your character and catches up smoothly, horizontal and vertical each at its own rate and response curve, with a soft lag limit. Turning stays instant unless smoothed turning is on.
- Stays in front of walls the game pulls its camera in for; snaps back after cutscenes, loads, the free camera and teleports.
- Camera position per situation (exploring, sprinting, combat and focus, aiming, claw ride and anti-grav): distance, height, shoulder offset and field of view. Also the game's own camera lag, look up/down limits and a glide time for position changes.
- Presets Tight, Balanced and Cinematic plus six save slots. V cycles them with a banner naming each one, O switches the follow on and off, N swaps shoulders.
- Settings in scripts/config/smoothcam.ini apply within a quarter second while the game runs. Optional page for Dawnwalker Mod Menu covering every setting except the key bindings.
- Built for Vercadi's UE4SS rc6 and Steam build 25232147. On any other build the mod stays inactive and says so in UE4SS.log.
```

## Maintenance

- Version: `ModVersion` in `dllmain.cpp`. It also appears in `[Mod] Version` in `mod_settings.ini`,
  the newest changelog entry above (the build checks all three), and in places the build does not
  check: the BBCode log-line `[code]` block and the version rows in the tables above.
- `Build-Package.py` fails when a `ConfigKey` in `mod_settings.ini` is missing from `smoothcam.ini`
  or appears twice, or a shipped value is outside `Minimum`/`Maximum` or not in `PresetValues`. Any
  of those stops the Mod Menu page opening at all, not just one row.
- It also fails when `main.dll` is older than the newest file in `cpp/DWSmoothCam/src`. A `git
  checkout` touches source mtimes, so after switching branches rebuild before packaging.
- The description's Settings spoiler lists the shipped defaults by hand; after changing a default
  in `smoothcam.ini`, update it.
- Game build: the DLL hooks `RebelCameraComponent` vtable slot 214 (`docs/design.md`, "The
  per-frame hook") and refuses to hook when that slot is not overridden. A game patch that moves the
  slot leaves the mod inactive rather than crashing; the build, the page requirements and the Known
  limits line then need the new build number.
- Loader: a new Vercadi UE4SS release on mod 18 means rebuilding against its UE4SS base commit
  (`.modding/tools/RE-UE4SS-src`), then updating the Requirements row and the description.
- Log lines in the description are inside `[code]`: `[DWSmoothCam]` outside one parses as a BBCode tag.
