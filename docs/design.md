# SmoothWalker: design and measurements

## Overview

A third-person camera for The Blood of Dawnwalker, in the style of Skyrim's SmoothCam, written as a C++
UE4SS mod. It lags the character pivot the game's camera view is built around, so following trails the
character while orbiting stays instant, and it tunes distance, height, shoulder and FOV inside the
game's own camera modes.

Everything here was measured on Steam build 25232147 under UE4SS `97b7e501` (Vercadi's rc6 unless noted),
2026-09-16.

## Build

Source in `src/`, superbuild `CMakeLists.txt` against an RE-UE4SS checkout at `97b7e501` (rc6's base,
cache variable `DW_RE_UE4SS_SOURCE_DIR`), output copied to `mod/dlls/main.dll` (git-ignored). Build:

```
cmake --build <build dir> --config Game__Shipping__Win64 --target DWSmoothWalker --parallel
```

`--parallel`, not `-- /m`: Git Bash rewrites `/m` into a path and MSBuild refuses it. A loaded
`main.dll` is locked, so deploying needs the game closed; C++ mods load at startup only.

## How it works

### The per-frame hook

`GetCameraView` is vtable slot 214 (verified 2026-09-16, rc6, build 25232147).

`RebelCameraComponent -> CameraComponent -> SceneComponent`. `RebelCameraComponent` overrides
`UCameraComponent::GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView)` at vtable **slot 214**
(offset `0x6B0`): engine RVA `0x17D2548`, Rebel RVA `0x17D143C`.

- **Found by count.** A pass-through stub (`inc [counter]; jmp [original]`) per differing slot: slot 214 ran
  52-58/s while walking at a ~16 ms frame and 0/s paused. Slot 216 ran ~12 per frame, slots 0, 135, 136
  a few times a minute.
- **Confirmed by content.** A typed probe called the original, then read the first members of
  `DesiredView`: `Location` (3 doubles), `Rotation` (Pitch, Yaw, Roll doubles), `FOV` (float, offset 48).
  FOV read 90 walking, 95 sprinting (the Sprint mode's `DefaultFieldOfView`) and blend values between;
  location sat ~250 cm behind and ~75 cm above the pawn; yaw matched `PlayerCameraManager:GetCameraRotation()`;
  `DeltaTime` matched the frame time.
- **It runs on task-graph worker threads.** The calling thread ID changed on nearly every 2 s sample. One
  call per frame, in sequence, but not on the game thread: a hook may transform `DesiredView` with its own
  state only, never call into UObjects or UE4SS.
- **No RTTI between vtables in this exe.** A vtable length found by "entries point into code" runs into the
  next class's table (SceneComponent read 968 slots, CameraComponent 434). Compare named slots, not lengths.
- **The swap.** Slot 214 is swapped for a function that calls the original `GetCameraView`, then edits
  `DesiredView.Location` / `Rotation`. Before swapping it checks that slot 214 differs from
  `CameraComponent`'s, and refuses to hook otherwise. The destructor puts the original back, so a hot reload
  cannot leave the vtable pointing into an unloaded DLL.

### Only the player's camera

The hook runs on task-graph workers, so it never calls into UObjects. The game thread publishes two atomics:
the player's `FollowCamera` and `RootComponent`. In 0.4.0 `BeginPlay` post caught `BP_PlayerController_C`
(FName compare, no object scan); `EndPlay` and `LoadMap` pre cleared it; an `EngineTick` post reads
`Controller.Pawn` at a cached offset and republishes when the pawn changes. 0.4.0 ran `FindFirstOf` only
when the toggle key was pressed with no controller known (hot reload mid-game); 0.8.0 finds the controller
without relying on BeginPlay, EndPlay or LoadMap (see "Loader profiles").
`FindFirstOf("BP_PlayerCharacter_C")` is wrong here: it can return a cutscene template pawn.

### Pivot

`ComponentToWorld` is not reflected. On the root (capsule, no parent) its translation equals
`RelativeLocation`, so the mod scans the component for that triple followed by a (1, 1, 1) scale 0x20
later and requires exactly one match. Measured: translation at **0x200**, `RelativeLocation` at 0x140.
A failed scan (a pawn at the origin matches twice) is retried for the same pawn after 2 s, doubling to 60 s;
it used to leave smoothing off for that pawn's whole life.

### Follow

A smoothed pivot trails the capsule: horizontal and vertical rates, each with a response curve over lag
distance and a leash. The camera becomes `smoothed pivot + (game camera - pivot)`, so orbiting stays
instant and only following lags. Optional rotation smoothing slerps the view and swings the arm to match.

- **Aiming** (`aiming_follow`, percent, default 30; not a preset key). A trail and smoothed turning behind
  the crosshair read as input lag. The camera component has no "active mode" call, but every mode has
  `GetState()` (`ECameraModeState`: BlendingIn 0, Active 1, BlendingOut 2, Popped 3; checked live), and the
  tuner already holds the player's live modes with their group. Each engine tick it calls `GetState` on the
  live aiming-group modes only (none while there is none) and publishes one atomic flag. The hook eases a
  0-1 factor toward it (rate 8/s on the world delta, about a third of a second) and scales only what is
  shown: the shown lag, and the shown rotation slerped from smoothed toward the game's. The smoothed pivot
  and rotation run on underneath, so the trail is back when the aim is lowered, with no edge either way. A
  cut snaps the factor. Guessing from the arm length was rejected: a close `exploration_distance` reads
  the same as aiming.
- **Soft leash** (`soft_leash = 1`, 0.5.0): the internal lag may run to 3x `max_lag`, and the camera shows
  `max_lag * tanh(lag / max_lag)`, so reaching the limit has no edge. With `soft_leash = 0` the leash is a
  hard clamp.

### Walls

While the game's camera sits closer than 0.85 of its recent distance (collision pulled it in), the result is
pulled toward the game's distance from the pivot: not at all at 0.85, fully at 0.65 and closer, smoothstep
between. The threshold used to be hard, and the game's own modes shorten the arm as well (aiming 250 to
about 85 cm, CombatNear to 150): running away from the camera, the whole lag along the arm (up to 70 cm on
Balanced) went in one frame as the distance crossed 0.85, and came back in one. A position write that
shortens the distance more than 15 % (Cinematic to Tight, 312 to 225 cm) tripped it too, from about 0.2 s to
1 s after V, so for `position_transition` + 0.3 s after a mode write the recent distance tracks the game's
and the clamp stays off. The recent distance settles at 1/s, so a real wall at half distance lets go after
about 1.7 s.

### Snaps

On toggle (0.4.0), more than `reset_gap` (0.25 s) without a view (cutscene, free camera, load), or a pivot
jump over `reset_distance` (500 cm). The world delta drives the step. Leaving a pause is a snap as well:
the camera does not update under one, so the gap exceeds `reset_gap`.

### Crossfade

Settings changes crossfade instead of snapping (0.8.0). Every publish used to set `g_reset`, snapping the
smoothed pivot to the capsule: invisible standing, a jump of the whole lag (up to ~1 m) when V switched
presets mid-motion, plus instant jumps from a new lag limit, rotation smoothing and mode FOV. Now only hard
cuts snap (startup, player or pawn change, `reset_gap`, `reset_distance`). A tuning generation (read under
the existing SRW lock), an O generation and a mode-write generation start a smoothstep crossfade over
`position_transition` (0 = none) from the last output, as offsets from the game's own view so moving and
turning during it do not lag; world delta, so slow motion slows it with the game. A pause does not hold it:
the camera stops updating under one (0 calls/s), so the first update after it exceeds `reset_gap` and is a cut. The smoothing state itself is not rewritten.
A publish bumps the tuning generation only when a hook value changed: N and the switches change none, and a
fade they started held part of the old lag for the transition time.

## Camera modes and position tuning

### The gameplay camera

Verified 2026-09-16. `<pawn>.FollowCamera` is a `RebelCameraComponent` holding a stack of camera modes,
each a `RebelCameraMode` subclass instanced under the component when pushed. Outdoors, standing, the stack
is one `BP_CameraMode_Base_LongRange_C`; sprinting pushes a `BP_CameraMode_Sprint_C` on top and
the LongRange instance stays below it. The component itself has no reflected properties.

69 mode classes under `/Game/_Dawnwalker/Player/Camera/Modes/`. The ones a player sees for more than
a moment: `Base`, `Base_LongRange`, `Base_CloseRange`, `Base_CloseRange_Mantle2m`, `Sprint`,
`Sprint_VampiricFastTraversal`, `Aiming`, `AimingOnLadder`, `AimingClawRide(Ledge)`, `FocusMode`,
`GapSqueeze`, `ClawRide`, `ClawRideLedge`, `AntiGrav`, `AntiGravAiming`, `Shadowstep_2_Base` (in `Modes/Shadowstep/`), and the
combat set `CombatNear`, `CombatFromArm`, `CombatFromArm_LongRange`, `CombatFromArm_VeryLongRange`,
`CombatFistFightMode`, `CombatSprinting`. The other ~40 are `Finisher_*` / `ShadowStepAttack*` effect cameras.

### Tunables

`RebelCameraModeTPP`, LongRange / Sprint values:

| Property | LongRange | Notes |
|---|---|---|
| `CameraHorizontalLagSpeed` / `CameraVerticalLagSpeed` | 3 / 8 | Sprint: 3 / 8 |
| `CameraHorizontalLagMaxDistance` / `CameraVerticalLagMaxDistance` | 30 / 0 | 0 = unclamped |
| `bEnableCameraHorizontalLag` / `bEnableCameraVerticalLag` | true / true | |
| `bUseCameraLagSubstepping`, `CameraLagMaxTimeStep` | false, 0 | |
| `DefaultFieldOfView` | 90 | Sprint: 95 |
| `ViewPitchMin` / `ViewPitchMax`, `ClampViewSpeed` | -60 / 40, 8 | |
| `BlendInArgs` / `BlendOutArgs` (`AlphaBlendArgs`) | 1.0 s opt 5 / 1.5 s opt 4 | mode push / pop |
| `CameraTypeBlendArgs` | 1.0 s opt 2 | camera type change |
| `CameraOffsets` | TMap `ECameraType` -> `CameraOffset` | below |
| `ModeCollisionSettings.PenetrationBlendInTime` / `OutTime` | 0.5 / 1.0 | |
| `ClippingDistance`, `ClippingBlendSpeed`, `UpVectorBlendSharpness` | 100, 4, 5 | |

`ECameraType`: `Default` = 1, `Interior` = 2, `Habitat` = 3. Set by the game from the environment,
not a player option. `CameraOffset` fields: `PivotZOffset`, `bOverrideFOV`, `OverriddenFieldOfView`,
`bUseOffsetPitchCurves`, `TargetOffset` (Vector: X back, Y side, Z up), plus curve structs.
LongRange: Default `(-250, 30, 0)`, Interior `(-190, 70, 10)`, Habitat `(-250, 70, 0)`, pivot Z 45 each.

### How writes land

Measured live on the running game.

- **Scalars on the live instance apply next frame.** `DefaultFieldOfView` 90 -> 70 on the live
  LongRange mode: `RebelPlayerCameraManager:GetFOVAngle()` read 70 on the following eval. (Read in
  the same eval it still reads the old value: the camera updates after the game-thread callback.)
- **`CameraOffsets` is read only on a camera type change.** `TargetOffset.X` -250 -> -600 on the live
  instance left the camera-to-pawn distance at 260. `SetCameraType` to the current type, or 2 then 1
  inside one callback, changed nothing. 2 in one callback and 1 in the next applied it: 607.
- **Instant apply:** set `CameraTypeBlendArgs.BlendTime = 0`, flip to another type, flip back on the
  next frame, restore the blend time. -400 written, 408 measured, no visible swing. With the blend
  time left above 0 the flip glides to the new offset instead.
- **CDO writes reach new instances.** `Default__BP_CameraMode_Sprint_C.DefaultFieldOfView` 95 -> 120,
  then sprint: the new `BP_CameraMode_Sprint_C` instance read 120 and `GetFOVAngle()` 120. So a mod
  writes the CDO for modes not yet pushed and the live instance for modes on the stack.
- All test values restored (LongRange -250 / FOV 90 / type blend 1.0, Sprint CDO and instance 95).
- A world paused under the pause menu does not update the camera: reads there prove nothing.

### Position tuning (`mode_tuning.hpp`)

Per group (exploring, sprinting, combat, aiming, claw ride and anti-grav): distance %, height, shoulder
and FOV. For every mode: a shoulder swap (`N`), the
look up/down limits and a transition time. 41 settings on the menu page (the three safety keys and `game_lag_scale` left it on 2026-09-19; the safety keys stay in the ini), validated with the menu's
parser. The groups cover 23 `BP_CameraMode_*` classes; the finisher and shadowstep attack cameras are
left alone. 22 are loaded at session start (21 checked 2026-09-16, `Shadowstep_2_Base` 2026-09-20).
`CombatSprinting` is day Coen's camera for sprinting with a weapon drawn, and its class is loaded by day only
(at night: absent, a lookup miss costs about 50 ms, so a miss is remembered until a map load or a push; the
vampire modes stay loaded by day). A save load by day captures it in the normal apply. For a night that turns
to day with no map load: a mode pushed from a class not captured yet is matched by
class name in `adopt_new()`, which captures the class, writes its CDO and that instance from the last apply
and requests the flip. Both were added 2026-09-20; `Shadowstep_2_Base` (Traversal) is a `RebelCameraModeTPP`
child branched off `Base` (FOV 100, pitch -60 / 40, key 1 at -400 70 50), `CombatSprinting` a
`CombatCameraMode` (FOV 90, pitch -89 / 89, key 1 at -200 50 0). The class count under `Modes/` moves with
streaming (60 to 76 seen in one session).

The shipped values differ widely, so every setting is relative:

| Mode | FOV | lag h/v | pitch | CameraOffsets `TargetOffset` (key: X, Y, Z) |
|---|---|---|---|---|
| Base | 100 | 0 / 8 | -60 / 40 | 1: -135 70 0, 2: -115 63 0, 3: -125 50 0 |
| Base_LongRange | 90 | 3 / 8 | -60 / 40 | 1: -250 30 0, 2: -190 70 10, 3: -250 70 0 |
| Sprint | 95 | 3 / 8 | -60 / 40 | 1: -230 70 0, 2: -210 55 0, 3: -250 70 0 |
| Sprint_VampiricFastTraversal | 95 | 3 / 8 | -60 / 40 | 1: -400 100 0, 2 and 3 as Sprint |
| Aiming | 100 | 0 / 8 | -89 / 89 | -60 60 0 on all three |
| AimingClawRide | 100 | 0 / 8 | -89 / 89 | 1: 0 0 70 |
| CombatNear | 80 | 0 / 0 | -89 / 89 | 1: -150 50 0 |
| CombatFromArm_VeryLongRange | 90 | 3 / 3 | -89 / 89 | 1: -200 110 20 |
| ClawRide | 100 | 8 / 8 | -89 / 89 | 1: -300 0 0 |

So distance scales X only where X < 0, the shoulder adds outward only where Y is not 0 (centred modes stay
centred), and the pitch limits apply only to modes shipped at -60 / 40. A negative shoulder offset stops at
the centre (0.7.4).

- **Layout from reflection, then a plausibility gate.** Offsets are looked up by name: `DefaultFieldOfView`
  0x54, `ViewPitchMin/Max` 0x58/0x5C, lag speeds 0x68/0x6C (`RebelCameraMode`), `CameraOffsets` 0x898 and
  `CameraTypeBlendArgs` 0x8E8 (`RebelCameraModeTPP`), `BlendTime` +0x08 in `AlphaBlendArgs`, `TargetOffset`
  0x10 and `OverriddenFieldOfView` 0x08 in `CameraOffset` (584 bytes). The lag enables are bitfields sharing
  the byte at 0x64, written through `FBoolProperty` and its mask, never as a byte: the game's lag is switched
  off while `enabled` is 1 (independent of `camera_tuning`) and put back at 0 and at unload. Tested live
  2026-09-19: with both speeds at 1.0 the camera floated on jumps, and clearing the two bits on the live
  `Base_LongRange` made it rigid at once, so the game reads them every frame. Before that the lag speeds were
  scaled by a `game_lag_scale` setting (removed), which at its default 3 still left the game about half the delay. The map is walked with
  `FScriptMap::GetScriptLayout(1, 1, size, align)` (ECameraType key, value at 0x08). Before the first write
  each captured original must look like a camera (FOV 30-170, 1-3 keys in 1..3, offsets within 2000 cm),
  or that mode alone is left as shipped (0.7.4; before, one implausible mode switched tuning off
  everywhere). The first capture logged Base_LongRange as fov 90, lag 3/8, pitch -60/40, key 1 at
  (-250, 30, 0): the values measured live.
- **Originals once, writes from originals.** Each class's CDO is read the first time it is seen and every
  write is computed from that, so applies cannot compound. A level change drops the CDO and instance
  pointers (classes may unload) but keeps the originals: a class that stayed loaded still holds the mod's
  values. At unload the CDOs get the originals back; live instances keep their values until the game pushes
  new modes. `restore()` re-captures before writing, and does nothing unless this session applied. A neutral
  apply was checked live: every value equal to the shipped one.
- **Where writes go.** Every captured CDO (new instances copy it), then the player camera's live modes, held
  as `LiveRef`s and checked against the object array before every write (the apply and the flip's two
  blend-time writes). A mode's outer is the `FollowCamera` it was pushed on (checked live). The list is
  filled two ways. Once per camera or world, one `ForEachUObject` pass compares each object's outer with the
  camera, then its class with the captured classes. After that the new-object callback hands over anything
  constructed with the player's camera as outer (one pointer compare per constructed object, a locked
  hand-off on a match, at most 256 waiting or the next apply scans again), and the apply sorts modes from
  the rest and drops collected ones. History: every apply and both flip stages called
  `FindAllOf("RebelCameraModeTPP")`, which compares names up every object's class chain: 58-64 ms a call on
  the game thread (435,100 objects), so each N or V dropped 3-4 frames at the press, again on the next frame
  and again 0.25 s after the glide. The outer-compare pass alone still read every object and measured
  23-27 ms per press (UEBench over five presses: max frame 34.3 ms against a 19.7 ms mean, 0 stutters),
  which is why it now runs only where a load hides it. The apply log line carries its own duration. Modes
  on other cameras are left alone: new instances copy the CDO and a new player camera gets its own apply.
- **The flip waits for the camera.** `CameraOffsets` is only read on a camera type change. 0.7.0 flipped the
  type over two engine ticks, and an Apply from the Mod Menu landed while the menu had the world paused:
  the camera never updated between the two flips and the new distance showed only later. 0.7.1 counts
  `GetCameraView` calls for the player's camera (`g_view_updates`) and advances each stage only after one:
  pending, away, back. A request while away runs again after the switch back; one during the glide back
  starts over at once (queued behind the glide, a quick second press moved the camera in two steps). The flip abandons if the player camera
  changes, waits while the player camera is briefly null, and switches back only from the type it set
  (0.7.4).
- **The flip glides.** With the type blend at 0 a shoulder swap snapped. The flip now sets the live modes'
  `CameraTypeBlendArgs.BlendTime` to `position_transition` (0.5 s default; 0 snaps) and restores the game's
  value 0.25 s after the blend ends, counted in the world time of the player's camera updates (the hook sums
  its `DeltaTime`), since the blend runs on world time: a pause or slow motion mid-glide must not put the
  blend time back early. An unload mid-glide restores it too. Verified afterwards: live mode and CDO back at 1.00.

## Presets and the Mod Menu page

### The Mod Menu page

- **Page** (`mod_settings.ini`; 0.5.0 had 19 settings in General, Presets, Follow, Rotation, Safety,
  Debug). Validated by running the menu's own `providers.lua` / `choices.lua` under Lua 5.4: the page opens,
  `rotation_rate` hides while `rotation_smoothing = 0`, and an Apply changes exactly the edited lines.
- **The menu's own `Type = preset` does not fit.** A linked preset sets every target picker to one shared
  value (`PresetTargets`, `CustomValue`), so it cannot carry slider numbers.
- **The menu re-reads the file on every page open** (`Choices.open`), so loaded values show on reopen.
  It also refuses an Apply when the file changed since the page opened ("config changed externally;
  reopen this mod"), which is why the 0.7 Load preset description said to reopen the page. 0.8.0 never
  writes the file while a page can be open, so that advice is gone.
- **Config lives in `ue4ss/Mods/DWSmoothWalker/config/`, not `scripts/config/`.** UE4SS creates a Lua mod for any
  mod folder with a `scripts` subfolder (`UE4SSProgram.cpp` 1424) and then logs a missing `main.lua` on every
  start; this mod has no Lua. `mod_settings.ini` points at `config/smoothwalker.ini`.

### The settings file

- **Live settings without Lua** (0.5.0). `on_update` (UE4SS thread) checks `smoothwalker.ini`'s write time every
  250 ms. The hook copies a numbers-only `Tuning` struct under a shared SRW lock, so nothing allocates on the
  worker thread. `toggle_key` and `preset_key` stay startup-only. UE4SS runs key callbacks on the same
  thread as `on_update` (`UE4SSProgram.cpp`, `process_event` then `fire_update`).
- **Robust reads** (0.7.4). Non-finite ini values (`std::stod` takes `nan`/`inf`) are ignored, and a
  non-finite pivot, view or result skips the frame. The Mod Menu's rename leaves the ini briefly absent: a
  missing file after startup keeps the live settings and retries. The startup read retries for 200 ms so a
  Mod Menu rename cannot leave default key names; with the ini missing the default keys are bound.
  `sanitize` clamps to the Mod Menu's ranges.
- **Diff-based reload** (0.8.0). The DLL keeps a baseline: every numeric key as last known in the file (after
  it parsed or wrote it). Startup parses everything. A later change (same 250 ms mtime poll, stamp before
  read, a missing file keeps the live settings) applies only keys whose number differs from the baseline,
  onto the live settings, then clamps. Order within one reload: (a) ordinary edits; (b) `preset_save` 1-6
  saves the slot from the live settings (`presets/Slot N.ini` is written at once: the menu does not watch it);
  (c) a changed non-zero `preset` loads that preset, then the preset keys edited in the same Apply are
  applied again on top. An empty slot logs and loads nothing. `enabled` changes the live switch only when
  the file's value changes, so O and an Apply of other settings do not fight. `enabled` and O
  switch the whole mod: off also publishes the position tuning as inactive (the modes go back to the game's
  values, as `camera_tuning = 0` does), skips the per-tick aiming `GetState` calls, and V and N are ignored,
  so off is a clean A/B against the game's own camera. O sets `m_settings.enabled` too, so the write-back
  puts it in the file and the page shows it.
- **Deferred write-back.** Nothing writes `smoothwalker.ini` from O, V, N or a plain reload. The exception:
  a reload that loaded a preset or saved a slot flushes at once. Deferred, a page reopened before
  the world ran again showed the new `preset` with the old sliders. The cost: the page still open refuses
  its next Apply ("reopen this mod"); its sliders were stale anyway. The desired file is every
  numeric live value, with `preset` = the active preset and `preset_save` = 0. `on_update` writes the
  differing numbers (in place, temp file plus rename) only when the camera is live, which means no menu page
  is open (the page is reachable only from the main or pause menu, where the player camera does not update),
  and only when the file's mtime still equals the stamp last processed (otherwise the poll goes first).
  Numbers are compared as written (`%.6g`), so a value the file cannot hold exactly does not rewrite
  forever. A failed write warns once per failure streak and retries every 250 ms, and removes its temp
  file; a changed or missing file waits for the poll on the same 250 ms throttle.
- **Pending side file.** A preset loaded from the paused menu used to be lost if the game exited or
  hot-reloaded before the camera went live: the file kept `preset = 103` and any sliders, and startup does
  not treat `preset` as a load request. Whenever the pending set or the stamp changes, the DLL writes
  `config/smoothwalker.pending` (temp plus rename; the menu never reads it): the `smoothwalker.ini` mtime
  the state was computed against, then every differing key. It is deleted after a successful flush or when
  nothing is pending. At startup, after the full parse, it is applied onto the live settings only if
  `smoothwalker.ini`'s mtime still equals its stamp (the values then flush once live), and deleted either way.
- **Startup flush.** The constructor runs the flush once without the `camera_live()` gate (stamp check
  kept). No page can be open while a mod constructs: on first start the menu has not run, and Ctrl+R
  uninstalls Lua mods before C++ mods reload (`UE4SSProgram.cpp` `uninstall_mods`, `queue_reinstall_mods`). This
  rewrites a `preset` id the regenerated manifest no longer lists (a deleted drop-in) before the page could
  fail on it ("configured value is outside declared choices", `choices.lua`).

### Presets

- **Presets carry 32 keys** (0.8.0; 36 until 2026-09-19, when `wall_clamp`, `reset_distance`, `reset_gap` and
  `position_transition` left: a preset is a camera look, and the first three are off the menu page, so a
  preset would have changed settings the player cannot see): follow, turning, the look limits and every group's distance, height, shoulder and
  FOV. Not `enabled`, `camera_tuning`, `shoulder_swap`, `show_banner`, `log_stats`, the key names,
  `preset` or `preset_save`. A preset file holding fewer keys loads and matches on the keys it has.
- **Built-ins** (cycle order): Tight, Balanced, Cinematic. Follow values, horizontal retuned 2026-09-19 for the game's lag being off (it had
  added up to 30 cm of trail; before: 25 cm 18/s, 70 cm 8/s; Cinematic was tried at 145 cm 3/s, too much, and kept as it was): Tight (lag 40/20 cm,
  12/20 per s, constant), Balanced (the shipped default: 85/50 cm, 6.5/10 per s; 95 cm 5.5/s was tried and read too loose, smoothstep h), Cinematic
  (120/80 cm, 4/6 per s, ease in-out, floor 0.35, turning smoothed at 25). Balanced equals the shipped
  `smoothwalker.ini` on all 32 keys.

  | Key | Tight | Balanced | Cinematic |
  |---|---|---|---|
  | `position_transition` | 0.4 | 0.5 | 0.8 |
  | pitch min / max | -60 / 40 | -60 / 40 | -70 / 55 |
  | exploring distance / height / shoulder / FOV | 90 / 0 / 0 / 0 | 100 / 0 / 0 / 0 | 115 / 10 / 10 / 5 |
  | sprinting | 90 / 0 / 0 / 0 | 100 / 0 / 0 / 0 | 110 / 10 / 10 / 8 |
  | combat | 95 / 0 / 0 / 0 | 100 / 0 / 0 / 0 | 110 / 0 / 0 / 0 |
  | aiming | 100 / 0 / 0 / 0 | 100 / 0 / 0 / 0 | 100 / 0 / 0 / 0 |
  | claw ride and anti-grav distance / height / FOV | 95 / 0 / 0 | 100 / 0 / 0 | 115 / 0 / 5 |

  `reset_distance` / `reset_gap` are 500 / 0.25 in all three. Every value sits on its slider step.

  Why these values (0.8.0, before release). The game's exploring and sprint modes lag the camera
  themselves (speed 3, up to 30 cm), a filter in series with the follow. It is now switched off (see
  "Layout from reflection"); the presets were tuned with part of it still present, so they may read
  tighter: retune by feel. Cinematic's `min_rate_scale` went 0.25 to 0.35: at 4/s the rate at
  rest was 1/s, a drift of about 3 s after stopping (the "floaty" of 0.5.0). Its exploring distance went
  125 to 115 because 125 % and +5 FOV both shrink the character; sprint sells speed with FOV (+8) rather
  than distance (110). Balanced still leaves the camera where the game puts it.
- **Six slots** (0.7 had 6, 0.8 drafts 10; back to 6 on 2026-09-19 with names). A slot's display name is the
  `name` line of its `Slot N.ini`, read at startup, shown in both pickers and the banner, and kept when the
  slot is saved again (picker order, rename after restart and name kept on re-save confirmed in game 2026-09-19). One constant, `dwsc::MAX_SLOTS = 6` in `config.hpp`, bounds the slot file
  names, the save range and the slot ids. The menu caps a picker at 64 values (`choices.lua`), and the
  Preset picker also carries Custom and the three built-ins, so 60 slots is the ceiling.
- **Presets folder** (`config/presets/`, replaces `presets.ini`; created at startup if missing). A
  slot save writes `Slot N.ini`: a two-line comment on turning it into a drop-in, `name = Slot N`, the 37
  keys. The mod writes no other file there. Any other `*.ini` is a drop-in preset, parsed like a slot file:
  `;`/`#` comments, `name = ...` for the label, keys outside the 37 and non-finite values ignored, values
  clamped on load, files with no preset keys or over 64 KiB skipped. Enumeration uses the wide Win32 APIs
  (the game path and file names may be non-ASCII). `release/example-preset/Template.ini` is a commented
  drop-in.
- **Scanned once per session**, in the constructor before anything reads presets: `Slot 1.ini` to
  `Slot 6.ini` (exact name, any case), then the drop-ins sorted case-insensitively by file name as ids
  201, 202, ... Presets are cached for the session (a slot save updates its entry); files added or removed
  while the game runs are ignored until the next start. At most 64 - 4 - 6 = **54 drop-ins**; the rest
  are skipped with one log line. Label: `name`, else the file stem, with control characters and `|`
  stripped, trimmed, cut to 48 bytes on a code point boundary, and valid UTF-8 (invalid UTF-8 anywhere makes
  the menu skip the whole manifest); else `Preset <id>`. Slots are always labelled `Slot N`. Banners and log
  lines convert the UTF-8 name with `MultiByteToWideChar`. A file named `mod_settings.ini` is skipped: the
  menu's manifest scan is recursive and would read it as a page.
- **Cached values are normalized.** Each slot or drop-in is applied onto default settings through the normal
  path (clamps, integer rounding, flags), read back for the keys it holds, and round-tripped through the
  `%.6g` write format. Before this, a drop-in with `follow_rate_h = 100` or `curve_h = 1.4` never matched
  once loaded: the indicator went Custom, the next V jumped to Tight and later drop-ins were unreachable.
- **Why the manifest is regenerated at startup.** The Mod Menu reads every `mod_settings.ini` once per game
  session, the first time its settings UI is built (`main.lua`, `Providers.discover` cached in
  `s.providers`), and the DLL constructor runs before that. So the constructor rewrites only the
  `PresetValues` and `PresetLabels` lines of `[Setting.preset]` to `0|101|102|103|1..10|201..` and
  `Custom|Tight|Balanced|Cinematic|<saved slots by name>|<drop-in names>|<empty slots>` (empty slots last, so
  the picker has nothing dead between the presets that load; they stay listed because the page fails to open
  if the ini's `preset` id is not among the values, and a slot saved this session becomes that id), and the
  `[Setting.preset_save]` labels likewise, keeping every other byte and the line endings,
  and writes (temp plus rename) only if the content changed. The shipped manifest lists no drop-ins. If the
  section or lines are missing or the write fails, it logs and turns drop-ins off for the session, so the
  indicator never reports an id the page lacks. Checked with the menu's parser: 64 values with 48-byte
  UTF-8 labels open.
- **Mod reload** (`Ctrl+R` on rc6; Framecore's profiles set `HotReloadKey = F11`, so `Ctrl+F11` there).
  Adding or removing drop-ins and reloading rescans and regenerates the manifest, and the
  menu picks it up: Ctrl+R uninstalls Lua mods too, the Mod Menu's state is rebuilt and discovery rereads
  every manifest. A `preset` id that no longer exists is rewritten by the startup flush.
- **The Preset picker** (`preset`, replaces `preset_load`; 0 Custom, 101-103, 1-6, 201+; default 102). The
  active preset is the last one loaded or cycled if all its keys still match the live values (within 1e-4),
  else the first match among the built-ins, slots 1-6, then drop-ins, else Custom. It is recomputed after
  every reload, save, load, cycle and shoulder swap, from the session cache. At startup the
  file's `preset` is not a load request, only the preferred match. The menu has no read-only type, so the
  indicator is the same picker the user changes to load a preset; the page shows the new value on reopen.
- **A slot save makes that slot the active preset** (it holds the live values, so it matches); a load in the same
  Apply still wins.
- **V** cycles built-ins, non-empty slots, then drop-ins, starting after the active preset (the first entry from
  Custom or an id not in the order), through the same load path as the menu. The active id updates in
  memory on each press, so quick presses advance one step each.

### Keys: V and N only while the camera is live

`N` pressed with the Mod Menu open wrote `shoulder_swap` to the ini behind the open page (the swap then
played on close), which makes the menu refuse the next Apply on that page. Since 0.7.2 the hook
stamps the QPC time of every player-camera update, and `V` and `N` act only when the last one is under 0.25 s old: not under a pause, a load, a cutscene or the free
camera. The log says when a press was ignored. Both are also ignored while the mod is off. `O` stays
live; its `enabled` change is written back like the others, once the camera is live.

### Banners

- **The region banner.** There is no free-text notification: every `NotificationSystemLibrary` push is
  typed. But `PushRegionEnteredNotification(WorldContextObject, FRegionData, bool IsNewlyDiscovered)` shows
  `RegionData.RegionDisplayText` in the region banner. Verified live 2026-09-16 with
  `FText("SmoothWalker: Cinematic")` and `IsNewlyDiscovered = false`: the banner appeared. Parameter layout:
  WorldContextObject 0x00, RegionData 0x08 (RegionTag, GlossaryEntry, LootRegionTag as FNames at 0x00,
  0x08, 0x10; RegionDisplayText at 0x20), IsNewlyDiscovered 0x40, 0x48 total. The DLL checks those
  offsets against reflection once and turns banners off on a mismatch, builds the buffer zeroed, lends
  an `FText` into it with `CopyBorrowedTo`, and calls `ProcessEvent` from the `EngineTick` post
  callback. `show_banner` switches it off.
- **Rapid banners queue, and a showing one must not be ended early** (measured 2026-09-16). Each push
  lands in `NotificationSubsystem.NotificationQueue` (TArray of `NotificationInfo`), and the region banner
  runs about 1.3 s showing (widget `State` 1), 2 s shown (2), then hides (3) in
  `WBP_NotificationPanel_QuestUpdate_C`, so N quick presses meant N banners in a row. `NotificationQueue:Empty()`
  from Lua cleared waiting entries without disturbing the banner on screen. Calling `NotifyEnded()` on
  `CurrentNotification` while its widget was in state 1 cleared `CurrentNotification` at once and let the
  next push show 0.06 s later, but the game crashed a few seconds on, in game code:
  `EXCEPTION_ACCESS_VIOLATION reading address 0x38`, most likely the widget finishing its show/hide cycle
  and reaching back for the notification that was gone. Do not end a notification a widget is showing;
  thin the queue instead.
- **Debounced, own entries thinned** (0.7.3). `request_banner` is debounced: the banner goes out 0.4 s after
  the last request, with the last text. Before the next push the DLL removes its own entries that are still
  waiting, compacting the TArray in place on the game thread. The game's own notifications are never
  touched, and a banner already on screen plays out (about 3-4 s). Shoulder swaps show no banner. The
  subsystem is looked up once and kept as a `LiveRef`: `FindFirstOf("NotificationSubsystem")` measured 28 ms,
  0.4 s into every V glide.
- **Ours are recognised by class and text** (0.7.4). No addresses are kept: a queued entry is ours only if it
  is a `RegionEnteredNotificationInfo` whose text starts with `SmoothWalker:`. (0.7.3 recorded the new last
  entry of `NotificationQueue`, found by reflection, straight after each push and compared pointers only; a
  stale pointer could match the game's own notification at a reused address.) The banner flag is cleared
  under the mutex.
- **Banners need a player** (0.8.0). A request while no player controller is held (the main menu) is dropped,
  not queued; the tick publishes an atomic flag on adopt and clears it on forget, since requests come from
  the UE4SS update thread.

## Loader profiles

### Loader: Vercadi rc6

`Dawnwalker-UE4SS-v1.2.1-rc6-build25232147` (Nexus mod 18), for Steam build 25232147 (exe SHA-256
`CB9B7D7B...`, 176,305,016 bytes, the build tested). UE4SS `97b7e501` plus Vercadi's local patches (rc5
struct/ref/out ownership, UE 5.5.4 soft-object layout translation, FString userdata copy fix).
`UE4SS.dll`, `dwmapi.dll`, `VTableLayout.ini` and both `UE4SS_Signatures/` files hash identical to
the package's `SHA256SUMS.txt`. Tested with rc6's settings plus one change: `EnableHotReloadSystem = 1`, so
`Ctrl+R` reloads. rc6 turns the external log console on (`ConsoleEnabled = 1`).

rc6 is the target for C++ mods: the RE-UE4SS source is checked out at `97b7e501`, rc6's base. Vercadi's
patches are not public source. It replaced a local build of UE4SS `2bfa839f` under rc5's configs, whose
signature comments still named build 25129649.

### Framecore's UE4SS for BoD

Framecore's "UE4SS for BoD" (Nexus 283) ships the official RE-UE4SS experimental build
of 97b7e501, the commit this DLL builds against, with `HotReloadKey = F11` (mod reload is `Ctrl+F11`). Its
default "Performance" profile sets `HookBeginPlay = 0`, `HookEndPlay = 0`, `HookLoadMap = 0`,
`HookEngineTick = 1`, `HookUObjectProcessEvent = 1`, `DefaultExecuteInGameThreadMethod = EngineTick`; its
"Compatibility" profile turns BeginPlay, EndPlay and LoadMap on and `HookUObjectProcessEvent` off. UE4SS
installs those three detours only when configured (`UnrealInitializer.cpp` 807, 892, 900), and a callback
registered on an uninstalled detour fails with a logged error (`DetourInstance.hpp` 200-206). Under 0.7 and
Performance the controller was never found, and controller, pawn and camera mode pointers dangled across
level loads. 0.8.0 covers every profile:

| Job | rc6 (all hooks) | Compatibility | Performance |
|---|---|---|---|
| Find the controller | new-object callback, BeginPlay | new-object callback, BeginPlay | new-object callback |
| Controller created before the mod (mod reload) | FindFirstOf at init, then backing off 2 s to 60 s while none | same | same |
| Controller or pawn destroyed | EndPlay, object array check each tick | same | object array check each tick |
| Level change | LoadMap pre, world pointer change | same | world pointer change, object array checks |
| Camera mode CDOs after a class unloads | object array check before use | same | same |

- **New-object callback:** `StaticConstructObject` post, which UE4SS always installs (`Metadata.hpp`,
  `ALWAYS_TRUE`). It runs on whichever thread constructs the object, loading threads included, so it only
  skips CDOs and archetypes by `params.SetFlags`, compares the class FName with `BP_PlayerController_C`, and
  on a match reads the object's index and hands it off under a mutex. The engine tick adopts it after an
  object array check.
- **Liveness:** each held pointer keeps the `GUObjectArray` index it had when taken from a live object. The
  check reads the array item (same pointer, neither `Unreachable` (1 << 28) nor `Garbage` (1 << 21)), then
  the object's class against the one recorded, so a different-class object reusing the address and index in
  one tick does not pass. A `Pawn` property still pointing at a garbage pawn is waited out, not re-adopted
  each tick.
  UE4SS's `IsPendingKill` tests bit 29, which UE 5 reuses, so it is not used. The engine tick checks
  controller, pawn, follow camera and root before anything reads through them; a dead controller clears the
  player and looks again at once.
- **World change:** `GEngine->GameViewport->World` from the `UEngine*` the engine tick receives, both offsets
  from reflection once, two pointer reads per tick, the world only compared. A change runs exactly what
  LoadMap pre runs (forget the player, `ModeTuner::forget()`, re-apply the position), and running it twice
  for one load is harmless.
- **ModeTuner:** captured CDO and class pointers are checked against the object array before every use
  (`capture()`, so `apply()` and `restore()`, and `for_each_instance()`). This does not rely on the world
  change running first, because a new world can reuse the old one's address. Live mode instances were already
  looked up fresh on each apply; the flip camera is used only while it equals the tick-checked player camera.
- **Scans:** the new-object hand-off is installed in every profile, so `FindFirstOf` is only a fallback: once
  at init, then, while no controller is known, after a wait that doubles from 2 s to a 60 s cap with each
  miss. A world change or a lost controller resets it to an immediate lookup (skipped if the hand-off already
  supplied one). Never while a controller is known. The cost of one walk is not measured.
- **Startup log:** one line lists the new-object callback, BeginPlay, EndPlay and LoadMap as on or off; a
  missing EngineTick hook logs a warning, since nothing works without it.

## Unload and hot reload

Since 0.7.4. `UnregisterCallback` waits for running callbacks (`RemoveCallback` ->
`WaitForExecutorsToFinish`). Order at unload: unregister callbacks, restore the vtable and wait, then
`restore()`. A `GetCameraView` call inside the hook could otherwise return into the freed DLL, so the hook
keeps an in-hook counter; unload restores the vtable, sleeps 50 ms, then waits for the counter to reach zero,
up to 5 s, and logs if a call is still in the hook. After a mid-game hot reload one controller lookup is
requested at init.

## Performance

Measured 2026-09-16, 0.6.1, rc6, build 25232147. Two measurements, because the cost is far below
whole-frame noise.

**Inside the hook.** 0.6.1 times `smooth_view` with QPC when `log_stats = 1` (the timing only runs then):
**3.3-3.6 µs per frame** at 47 and 57 fps, standing and moving, about 0.017 % of a 21 ms frame.

**Whole frames, UEBench** (a separate UE4SS frame-time benchmark mod). Same spot and view, hands off, `O`
toggling the smoothing between captures (so "off" means the DLL loaded with the hook returning after the
original, the fair A/B for the math), order on, off, on, off. Poll sampler, 20 s per capture after a 2 s
settle, all four valid with identical stamps (`sg.*` 2, ViewDistance 3, `r.ScreenPercentage` 58):

| Capture | SmoothWalker | n | mean ms | p50 ms | p95 ms | p99 ms | stutters |
|---|---|---|---|---|---|---|---|
| 13:17:08 | on | 945 | 21.25 | 21.14 | 23.28 | 24.23 | 0 |
| 13:21:21 | off | 945 | 21.26 | 21.21 | 23.21 | 24.26 | 0 |
| 13:21:54 | on | 945 | 21.28 | 21.24 | 23.20 | 24.20 | 0 |
| 13:22:27 | off | 945 | 21.23 | 21.23 | 23.07 | 23.96 | 0 |

Median p50: on 21.19 ms, off 21.22 ms. The on/off difference (-0.03 ms) is smaller than the spread
between two captures in the same state (0.10 ms on, 0.02 ms off): **no measurable frame-time cost**, no
stutters in either state. A DLL-not-installed baseline was not run; the hook's early return when off is
one atomic load and a pointer compare.

**UEBench had to be fixed first.** Its 1.0.0 poll sampler (a 1 ms `LoopAsync` queueing game-thread
closures) crashed the game 4-6 s after F8 under rc6, whose queue drains on every engine tick: Lua ran on
the async thread and the game thread at once (`attempt to call a nil value (method 'GetFrameCount')`,
`Ref was not function`, then an access violation, then an abort, in `UE4SS.dll`; `main.dll` in neither
crash). The poll now runs as `LoopInGameThreadAfterFrames(1, ...)` and the run driver on the engine tick
too. Before deploying it, a live test loop of the same kind ran exactly 180 calls over 180 frames and
cancelled itself; the four captures above then ran clean. The fix is in UEBench itself.

## Known limits

- **One game build.** The hook needs `GetCameraView` at vtable slot 214 as on build 25232147; when slot 214
  is not overridden the mod refuses to hook and stays inactive.
- **One UE4SS commit.** The DLL links against `UE4SS.dll` built from `97b7e501`; a loader on another commit
  needs a rebuild against it.
- **The page indicator lags.** After an Apply of a slider, the page stays open with the old `preset` value
  shown until it is reopened, because the corrected indicator is written only once the camera is live again.
  After a preset load or slot save the file is written at once, so that page refuses another Apply
  until it is reopened.
- **Drop-ins are read at startup.** Files added to or removed from `config/presets/` while the game runs are
  ignored until the next start or mod reload. At most 54 drop-ins.
- **Finisher and shadowstep attack cameras** (~40 classes) are left as shipped.
- **Stats race.** The stats accumulator's load-then-store race is not fixed (stats only).
- **The `FindFirstOf` fallback's cost** for one object walk is not measured.

## Version history

### 0.4.0: C++ follow camera on GetCameraView

Hook, player camera, pivot, follow with a hard leash, walls and snaps as described above.

In-game test, 2026-09-16 (rc6, build 25232147, defaults): walk, sprint, jump, turn, walls, `O` toggled
several times. Reported: smooth, no visible jitter or stutter; the camera pulls in near walls with no
visible clipping. No crash. The log over 49 active 5 s samples: 43-54 smoothed frames/s, mean pivot lag
65 cm, max **80.1 cm, the `max_lag_h` leash**, wall clamp active in 10 samples (2-48 % of their frames).

**Tuning note.** While running, the lag sat on the leash: at 6/s with smoothstep and a 0.25 floor the
effective rate near 65 cm is ~3.3/s, whose steady-state lag at running speed exceeds 80 cm. So the hard
clamp, not the curve, set the running offset. 0.5.0 answers it with the soft leash.

### 0.5.0 and 0.6.0: live settings, Mod Menu page, presets, banner

Soft leash, live settings, the Mod Menu page and the region banner, as described above.

- **Presets were menu actions the DLL runs** (0.5.0-0.7.4; 0.8.0 replaced `preset_load` with the `preset`
  indicator). `preset_load` (101-103 built-in, 1-6 slot) and `preset_save` (1-6) were pickers. On the next
  poll the DLL saved the 12 `PRESET_KEYS` to `presets.ini`, or loaded a preset and rewrote those numbers in
  `smoothwalker.ini` in place (comments kept, temp file plus `MoveFileEx`), then set the picker back to 0. Save
  ran before load when one Apply set both.
- **Built-ins.** 0.6.0 introduced Tight, Balanced and Cinematic with the follow values above. 0.5.0 had
  Subtle/Cinematic/Responsive; in play only Cinematic felt different, and it read as pulled back and floaty
  (160 cm, turning at 12).

In-game test 2026-09-16 (0.6.0): banners showed for V, O and a menu load; the three presets feel
distinct; Balanced kept as the default. No warnings in the log.

### 0.6.1: hook timing

`smooth_view` timed with QPC under `log_stats = 1`; see "Performance".

### 0.7.0 and 0.7.1: camera position in the game's own modes

Position tuning, as described in "Position tuning". 0.7 presets carried follow keys only.

In-game test 2026-09-16 (0.7.1): Exploring distance 150 % applied the moment the menu closed; `N` glides
in both directions, including quick presses; 0.5 s kept as the default.

Group test, same day: exaggerated values written to the live ini (sprint FOV +20, combat distance 200 %,
aiming shoulder +40, claw ride and anti-grav distance 50 %, look limits -85 / 80), checked live before
play. CDOs read Sprint FOV 115, CombatFromArm key 1 at -240 and CombatNear at -300, Aiming
Y 100 with AimingOnLadder still centred at Y 0 and -40 / 89, ClawRide -150, AntiGrav -150 / -82, and
-85 / 80 only on the modes shipped at -60 / 40. All five reported visible in play. Cycling presets with
`V` during the test left the position values alone, as designed then (0.7 presets carried follow keys only;
0.8.0 presets carry position too).
Restoring the neutral values put every checked CDO back to shipped and swept 3 live modes.

### 0.7.2: V and N only while the camera is live

See "Keys". Tested: `N` with the menu open does nothing, `N` in play swaps.

### 0.7.3: debounced banners

See "Banners". Tested in play: a burst of V gives one banner with the final preset, a single V after it
shows promptly, V during a showing banner gives that banner then one more, no warnings.

### 0.7.4: hot-reload, camera-mode and settings fixes (2026-09-16)

19 defects in 0.7.3, each confirmed against the code and the UE4SS source before fixing. Corrections to what the design had assumed: UE4SS runs key callbacks on the same
thread as `on_update` (`UE4SSProgram.cpp`, `process_event` then `fire_update`), and `UnregisterCallback`
waits for running callbacks (`RemoveCallback` -> `WaitForExecutorsToFinish`).

| Finding | Fix |
|---|---|
| Hot reload: a `GetCameraView` call inside the hook could return into the freed DLL | In-hook counter; unload restores the vtable, sleeps 50 ms, waits for the counter |
| Unload restored modes before unregistering callbacks, racing the game thread | Order: unregister callbacks, restore vtable and wait, then `restore()` |
| Live mode pointers and the flip camera were kept across frames (use after GC, pawn death mid-flip) | Instances looked up at each flip stage; the flip abandons if the player camera changes |
| `restore()` skipped CDOs dropped by `LoadMap`, so a reload could capture tuned values as originals | `restore()` re-captures before writing |
| NaN from the ini (`std::stod` takes `nan`/`inf`) or a bad read stuck in the follow state and reached the view | Non-finite ini values ignored; non-finite pivot, view or result skips the frame |
| The Mod Menu's rename leaves the ini briefly absent; a poll then reset every setting to defaults | A missing file after startup keeps the live settings and retries |
| A stale banner pointer could match the game's own notification at a reused address | No addresses kept: a queued entry is ours only if it is a `RegionEnteredNotificationInfo` whose text starts with `SmoothWalker:` |
| `enabled`/O described as "the game's camera" but position tuning stayed on | Descriptions say it switches the follow only |
| A negative shoulder offset could cross to the other shoulder | Offset stops at the centre |
| After a mid-game hot reload the controller was only found on O, which also toggled smoothing off | One controller lookup requested at init |
| One implausible mode switched tuning off everywhere, restore included | That mode alone is left as shipped |
| The flip restored its remembered type even if the game changed type mid-flip | Switches back only from the type it set |
| Timestamp taken after the read; banner flag cleared outside its lock; keys unbound when the ini is missing; slot files could inject non-preset keys or out-of-range values | Stamp first; flag under the mutex; default keys bound; slots filtered to preset keys; `sanitize` clamps to the Mod Menu's ranges |

A second check confirmed 15 fixes and found one partial (the banner match, then still address-based
with a 5 s window; replaced as above). Follow-ups: a flip waits while the player camera is
briefly null instead of abandoning (an unpossess and repossess would otherwise leave the away camera type);
the unload wait runs to 5 s and logs if a call is still in the hook; `restore()` does nothing unless this
session applied; the startup read retries for 200 ms so a Mod Menu rename cannot leave default key names.

Not changed: the stats accumulator's load-then-store race (stats only). Verified offline by compiling
`config.hpp` against `nan`/`inf`, out-of-range values, CRLF and a slot carrying `preset_load`; the menu parser
still opens the page.

### 0.8.0: presets carry position, the Preset picker

Three problems with 0.7: a preset left the camera position alone, the menu had no way to show which preset
was active, and V and N wrote `smoothwalker.ini` at once, so every Mod Menu Apply re-read the whole file and
anything written behind an open page made its next Apply fail.

Changes, each described above: diff-based reload, deferred write-back, the pending side file and the
startup flush ("The settings file"); presets carrying 37 keys (32 now), ten slots (6 now), the presets folder with drop-ins,
normalized cached values, the regenerated manifest, the Preset picker and V cycling through all of them
("Presets"); the crossfade ("Crossfade"); banners needing a player ("Banners"); and discovery that works in
every loader profile ("Loader profiles").
