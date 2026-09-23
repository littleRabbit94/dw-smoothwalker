# Smoothwalker - Third Person Camera

A third-person camera mod for The Blood of Dawnwalker that replaces the game's camera lag with a custom
implementation that can be configured into presets using pure ini files or the optional in-game Mod Setting Menu.
Distance, height, shoulder and FOV can be set per mode, with a shoulder swap on `V`. Keybinds for turning
the mod on/off and cycling presets can be set in the ini for convenience.

## Requirements

- The Blood of Dawnwalker, Steam build 1.0.5 (25232147).
- UE4SS at commit `97b7e501`, from either:
  - Vercadi's UE4SS for Dawnwalker, rc6 (Nexus mod 18), or
  - Framecore's UE4SS for BoD, 2c or 2b, Performance or Compatibility profile (Nexus mod 283).
- Optional: Mod Setting Menu (Nexus mod 271) for the in-game settings page. Without it, edit
  `config/smoothwalker.ini`.

For other mods: the camera API, docs/api.md.

## Install (players)

Download from the Nexus page: https://www.nexusmods.com/thebloodofdawnwalker/mods/613

## Build

Needs Visual Studio 2022 (MSVC), CMake 3.22 or later, and an RE-UE4SS checkout at `97b7e501` with its
submodules and build prerequisites (see RE-UE4SS's own build instructions). The DLL links against
`UE4SS.dll`, so the checkout must match the loader's commit.

```
git -C <RE-UE4SS> checkout 97b7e501
cmake -S . -B <build dir> -G "Visual Studio 17 2022" -DDW_RE_UE4SS_SOURCE_DIR=<RE-UE4SS>
cmake --build <build dir> --config Game__Shipping__Win64 --target DWSmoothwalker --parallel
```

The first build compiles UE4SS too. The output is copied to `mod/dlls/main.dll` (and `main.pdb`). Use
`--parallel`, not `-- /m`: Git Bash rewrites `/m` into a path. To deploy, close the game and copy `mod/`
to `Dawnwalker/Binaries/Win64/ue4ss/Mods/DWSmoothwalker/`. An installed `config/smoothwalker.ini` holds
the player's settings: when a new setting is added, add its line there instead of overwriting the file. The Mod Menu page
will not open while a `ConfigKey` is missing from it.

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Superbuild: RE-UE4SS from `DW_RE_UE4SS_SOURCE_DIR`, then the `DWSmoothwalker` target |
| `src/` | The DLL source: `dllmain.cpp` (hook, follow, settings, player discovery), `config.hpp` (settings and presets), `mode_tuning.hpp` (camera position in the game's modes, aiming, combat and traversal state), `smoothing.hpp` (math) |
| `mod/` | Exactly what ships under `ue4ss/Mods/DWSmoothwalker/`: `enabled.txt`, `LICENSE`, `mod_settings.ini` (Mod Menu page), `config/smoothwalker.ini`; `dlls/` is build output |
| `release/` | `Build-Package.py`, the Nexus page description and metadata, `example-preset/` |
| `docs/design.md` | How the mod works, the game's camera, measurements and version history |

## Packaging

```
python release/Build-Package.py
```

Checks versions, the Mod Menu manifest against `smoothwalker.ini`, DLL freshness and the example preset, then
writes `release/dist/Smoothwalker-<version>.zip` and `release/dist/Smoothwalker-Example-Preset-<version>.zip`.

## Making presets

Copy `release/example-preset/Template.ini` into `ue4ss/Mods/DWSmoothwalker/config/presets/` under a new name,
change the `name` line and the values, and restart the game. Every key is commented with its range and
default. The preset format and rules (41 keys, clamping, the 54 drop-in limit) are in `docs/design.md`,
"Presets". Switches are not preset keys; a preset that sets them is ignored on those lines.

To start from a camera you tuned in game, set the Preset picker on the Mod Menu page to a slot marked
`(empty)` and press Apply: the mod writes your live settings to `config/presets/Slot N.ini`. Keep tuning with
that slot picked and every Apply saves into it. Copy the file under any other name to turn it into a drop-in;
`Slot 1.ini` to `Slot 6.ini` belong to the player's slots and are overwritten.

## Checking a change in game

By default `UE4SS.log` stays quiet: the load line, the changes you make (toggle, presets, shoulder swap,
settings applied), and any warnings or errors. With `log_stats = 1` the mod also logs the per-frame hook cost
every 5 s. With `log_verbose = 1` (ini only, not on the Mod Menu page) it adds cyan detail: player discovery,
offsets, camera mode layouts, and combat/traversal camera changes, including camera position apply's own
duration (`camera position applied: ... ms`): after the first apply of a session it should read about 0 ms,
and a number in the tens means something walks the object array on a key press again. With `debug_overlay = 1`
(or a key named in `debug_key`) a panel at the top right of the screen shows the live follow, the game's
camera modes, the last snap and the API claim; see `docs/design.md`, "Debug overlay".

## License

GPL-3.0-or-later. See `LICENSE`.

## Credits

Built on [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) by the UE4SS-RE team (MIT).
Inspired by SmoothCam for Skyrim
