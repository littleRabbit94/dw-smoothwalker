# SmoothCam - UE4SS (DWSmoothCam)

A smoother third-person camera for The Blood of Dawnwalker: the camera trails your character and catches up
smoothly while turning stays instant. Distance, height, shoulder and FOV can be set per situation, with
presets on `V`, on/off on `O` and a shoulder swap on `N`.

## Requirements

- The Blood of Dawnwalker, Steam build 25232147. On any other build the mod stays inactive.
- UE4SS at commit `97b7e501`, from either:
  - Vercadi's UE4SS for Dawnwalker, rc6 (Nexus mod 18), or
  - Framecore's UE4SS for BoD, 2b, Performance or Compatibility profile (Nexus mod 283).
- Optional: Dawnwalker Mod Menu (Nexus mod 271) for the in-game settings page. Without it, edit
  `config/smoothcam.ini`.

## Install (players)

Download from the Nexus page: https://www.nexusmods.com/thebloodofdawnwalker/mods/TBD

## Build

Needs Visual Studio 2022 (MSVC), CMake 3.22 or later, and an RE-UE4SS checkout at `97b7e501` with its
submodules and build prerequisites (see RE-UE4SS's own build instructions). The DLL links against
`UE4SS.dll`, so the checkout must match the loader's commit.

```
git -C <RE-UE4SS> checkout 97b7e501
cmake -S . -B <build dir> -G "Visual Studio 17 2022" -DDW_RE_UE4SS_SOURCE_DIR=<RE-UE4SS>
cmake --build <build dir> --config Game__Shipping__Win64 --target DWSmoothCam --parallel
```

The first build compiles UE4SS too. The output is copied to `mod/dlls/main.dll` (and `main.pdb`). Use
`--parallel`, not `-- /m`: Git Bash rewrites `/m` into a path. To deploy, close the game and copy `mod/`
to `Dawnwalker/Binaries/Win64/ue4ss/Mods/DWSmoothCam/`.

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Superbuild: RE-UE4SS from `DW_RE_UE4SS_SOURCE_DIR`, then the `DWSmoothCam` target |
| `src/` | The DLL source: `dllmain.cpp`, `config.hpp`, `mode_tuning.hpp`, `smoothing.hpp` |
| `mod/` | Exactly what ships under `ue4ss/Mods/DWSmoothCam/`: `enabled.txt`, `LICENSE`, `mod_settings.ini` (Mod Menu page), `config/smoothcam.ini`; `dlls/` is build output |
| `release/` | `Build-Package.py`, the Nexus page description and metadata, `example-preset/` |
| `docs/design.md` | How the mod works, the game's camera, measurements and version history |

## Packaging

```
python release/Build-Package.py
```

Checks versions, the Mod Menu manifest against `smoothcam.ini`, DLL freshness and the example preset, then
writes `release/dist/SmoothCam-UE4SS-<version>.zip` and `release/dist/SmoothCam-Example-Preset-<version>.zip`.

## Making presets

Copy `release/example-preset/Template.ini` into `ue4ss/Mods/DWSmoothCam/config/presets/` under a new name,
change the `name` line and the values, and restart the game. Every key is commented with its range and
default. The preset format and rules (37 keys, clamping, the 50 drop-in limit) are in `docs/design.md`,
"Presets".

## License

GPL-3.0-or-later. See `LICENSE`.

## Credits

Built on [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) by the UE4SS-RE team (MIT).
