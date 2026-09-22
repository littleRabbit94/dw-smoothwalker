# Smoothwalker: design and measurements

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
cmake --build <build dir> --config Game__Shipping__Win64 --target DWSmoothwalker --parallel
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

**Feet, not centre, vertically** (0.9.0, 2026-09-21). A crouch shrinks the capsule from half height 96 to 42
and drops the root by the 54 cm difference in one frame; the mesh offset goes -97 to -43, so the feet stay
put. The game's own camera does not follow at once: its pivot (`GetPivotLocation`, root + `PivotZOffset` 45
at rest) was read 48 cm below its rest position with the capsule already standing, so the game eases its
pivot height over time whatever the lag flags say. Lagging the root made `game camera - pivot` grow by 54 in
the crouch frame: the camera popped up by the shown lag (about 39 cm under the soft leash), then sank as the
follow and the game's easing settled, and the mirror on standing up. The vertical follow now tracks the
capsule bottom, root Z minus `CapsuleHalfHeight` (a reflected float on the root; the offset is resolved with
the pawn, and a missing or implausible value falls back to the centre). The feet do not move in a crouch, so
the follow adds no vertical lag to one and the game's eased height passes through; standing, the feet sit a
constant 96 below the centre, so nothing else changes. Horizontal follow, the arm, the wall clamp and the
cut test still use the centre. Measured live 2026-09-21 at rest: root 16231.9 standing / 16177.9 crouched,
the camera component 80 above the root in both, the game pivot root + 45 in both.

`log_trace = 1` keeps the last 240 frames of the vertical follow (dt, root Z, half height, feet Z, smoothed
Z, game camera Z, output Z, shown lag, cut) in a ring and writes them to the log 90 frames after every half
height change, so a crouch or stand sits in the middle of the dump. It exists because sampling the
transition over the Lua bridge at 50 ms crashed the game (see the toolkit's bridge notes).

Verified 2026-09-21 with the trace, 18 dumps over repeated crouches and stands at rest: on the change frame
the root moves 54, the feet and the smoothed Z do not move, and the output Z equals the game camera Z to
0.0 cm on that frame and every frame after. The game itself eases the 54 cm over about 25 frames (0.35 s,
an S-curve peaking at 5-9 cm per frame, with a 2 cm bump the other way on the first frame), which is what
the player now sees; the mod adds nothing to it.

**Crouch hold** (same day). Played, that was too early: while moving, the camera went down before the character,
whose crouch animation is still blending in. In the shipped game the modes' own vertical lag delays the
descent; the mod turns that lag off while it is on, and the feet pivot left nothing in its place. So the
height change is lagged through the vertical follow, measured so it cannot pop: an episode starts on a half
height change with `crouch_base` = camera Z above the feet (root motion cancels out); `crouch_drop` = base
minus the current value is the game's eased change so far, 0 on the change frame and 54 once the game has
settled; `crouch_smoothed` trails it at `follow_rate_v` with the curve, and the difference, leashed and
scaled by the aiming factor, lifts the shown pivot (holds the camera up in a crouch, down in a stand).
`crouch_drop` stops updating 0.6 s in, after the game's easing, so a later pitch change cannot leak into it;
the hold then decays to nothing and the episode ends. A stand before the crouch has settled folds the running
hold into the new episode, so the output stays continuous. A cut clears it. The trace has a `hold` column.
Traced the same day: the game's camera starts down about 130 ms after the capsule change and is 90 % down
by 290 ms, moving or not; with the hold the output reaches those marks at about 180 and 450 ms, hold peak
21 cm. What remains is the game's: a crouch pressed while the character is still coming to a stop shrinks
the capsule (and drops both cameras) at once, and the crouch pose blends in half a second to a second later,
after the stop animation. Checked with the mod off (`enabled = 0` live): the shipped camera dips the same.

### Follow

A smoothed pivot trails the capsule (its centre horizontally, its bottom vertically, see "Pivot"): horizontal and
vertical rates, each with a response curve over lag distance and a leash. The camera becomes `smoothed pivot + (game camera - pivot)`, so orbiting stays
instant and only following lags. Optional rotation smoothing slerps the view and swings the arm to match.
The rotational trail is capped at 90 degrees, pulled in along its own arc: a slerp always catches up the
short way round, so a trail past 180 degrees ran backwards (Nexus bug report 2026-09-21: turning follow
speed 1, a 360 spin, the camera reversed halfway). At speed 1 a fast spin now rides the cap, 90 degrees
behind, and catches up over about a second when the turn stops. A turn past 180 degrees inside one frame is
still ambiguous, as it is for any smoothing.

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

Per group (exploring, sprinting, combat, focus, aiming, claw ride and anti-grav): distance %, height, shoulder
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
- **Config lives in `ue4ss/Mods/DWSmoothwalker/config/`, not `scripts/config/`.** UE4SS creates a Lua mod for any
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
  onto the live settings, then clamps. Order within one reload: (a) ordinary edits, with
  `active_before` = the `preset` id captured before them; then exactly one of (b) `preset` edited to an
  empty slot 1-6, which saves the live values into it (adopt); (c) `preset` edited to another non-zero id,
  which loads that preset, applies the preset keys edited in the same Apply again on top, and on a slot
  saves the result back into it; (d) `preset` edited to 0, which detaches (`m_loaded_id = 0`) and pins Custom (`m_custom_pinned`):
  `update_active_locked` would otherwise match the unchanged values to the slot again and the next edit
  would be saved into it. The pin holds until a preset is loaded or a slot adopted, and startup restores
  it when the file says 0 over values a preset matches;
  (e) `preset` untouched with `active_before` a slot 1-6 and at least one preset key edited, which saves
  the live values into that slot (autosave). `presets/Slot N.ini` is written at once: the menu does not
  watch it. Built-ins and drop-ins are read-only: an edit with one active just falls through to
  `update_active_locked`, which gives Custom. `enabled` changes the live switch only when
  the file's value changes, so O and an Apply of other settings do not fight. `enabled` and O
  switch the whole mod: off also publishes the position tuning as inactive (the modes go back to the game's
  values, as `camera_tuning = 0` does), skips the per-tick aiming `GetState` calls, and V and N are ignored,
  so off is a clean A/B against the game's own camera. O sets `m_settings.enabled` too, so the write-back
  puts it in the file and the page shows it.
- **Deferred write-back.** Nothing writes `smoothwalker.ini` from O, V, N or any reload, a preset load
  included (0.8.0 flushed a load at once, which made the still-open page refuse its next Apply; 0.9 waits
  for the menu to close instead, see below). An adopt or an autosave leaves the live numbers equal to what
  the Apply wrote, so nothing is pending. The desired file is every numeric live value, with `preset` = the
  active preset. `on_update` writes the differing numbers (in place, temp file plus rename) only when the
  camera is live or the Mod Menu is closed (`mod_menu_open`), so never behind an open page, and only when
  the file's mtime still equals the stamp last processed (otherwise the poll goes first). A load from the
  pause menu is therefore in the file within 250 ms of leaving the menu, and a page reopened from the pause
  menu shows the loaded values; the page still open after the load keeps its stale sliders, and an Apply
  there still works and moves only the keys it changed.
- **Menu-open detection** (2026-09-21). The menu has no close signal and its `dmm_api` fires only on Apply,
  so the DLL reads the menu's widget tree. Its host is created with the native class
  `CommonActivatableWidget` (`main.lua`, `library:Create` with `/Script/CommonUI.CommonActivatableWidget`),
  which the game's own screens all subclass, so an instance of exactly that class is the menu. Sampled live
  with the bridge: open, `Visibility` 4 (SelfHitTestInvisible), activated, in viewport; closed, the object
  lingers until GC with `Visibility` 1 (Collapsed), disabled, out of the viewport. `mod_menu_open` keeps the
  host last seen open and rescans (`ForEachUObject`, exact class, not unreachable, `Visibility` not
  Collapsed) only when that one is collapsed or gone; it runs only with a write pending and the camera not
  live, so never during play. If a menu update changes the host the check returns false and the write lands
  under the open page as in 0.8.0: a locked page, never a lost value. The finer check (the page switcher and
  the title text, for a list round-trip without leaving the menu) was left out as two more fragile parts.
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

- **Presets carry 36 keys** (32 in 0.8.0; four `focus_` keys since 0.9.0, when focus left the combat group; a file or preset without them takes its combat values, in `parse_settings` and `fill_focus`; 36 until 2026-09-19, when `wall_clamp`, `reset_distance`, `reset_gap` and
  `position_transition` left: a preset is a camera look, and the first three are off the menu page, so a
  preset would have changed settings the player cannot see): follow, turning, the look limits and every group's distance, height, shoulder and
  FOV. Not `enabled`, `camera_tuning`, `shoulder_swap`, `show_banner`, `log_stats`, the key names,
  or `preset`. A preset file holding fewer keys loads and matches on the keys it has.
- **Built-ins** (cycle order): Tight, Balanced, Cinematic. Follow values, horizontal retuned 2026-09-19 for the game's lag being off (it had
  added up to 30 cm of trail; before: 25 cm 18/s, 70 cm 8/s; Cinematic was tried at 145 cm 3/s, too much, and kept as it was): Tight (lag 40/20 cm,
  12/20 per s, constant), Balanced (the shipped default: 85/50 cm, 6.5/10 per s; 95 cm 5.5/s was tried and read too loose, smoothstep h), Cinematic
  (120/80 cm, 4/6 per s, ease in-out, floor 0.35, turning smoothed at 25). Balanced equals the shipped
  `smoothwalker.ini` on all 36 keys.

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
  `name` line of its `Slot N.ini`, read at startup, shown in the picker and the banner, and kept when the
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
  if the ini's `preset` id is not among the values, and a slot saved this session becomes that id), keeping
  every other byte and the line endings, and writes (temp plus rename) only if the content changed. The shipped manifest lists no drop-ins. If the
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
  indicator is the same picker the user changes to load a preset or claim an empty slot; the page shows the
  new value on reopen.
- **A slot save makes that slot the active preset** (it holds the live values, so it matches). An adopt sets
  `m_loaded_id` to the slot; an autosave keeps it there.
- **Slots are profiles** (2026-09-21; the `preset_save` picker until then). One picker does all three jobs,
  told apart by the ` (empty)` suffix: picking an empty slot copies the live settings into it, picking a
  saved slot loads it, and any later Apply that moves a preset key while that slot is active writes the new
  values back into it. Built-ins and drop-ins are unchanged by an edit; the indicator just goes Custom.
  Why the save picker went: it was an action faked as state. The DLL had to reset `preset_save` to 0 after
  the save, which forced a `smoothwalker.ini` write while the page that triggered it was still open, and the
  menu (`choices.lua`, `M.replace`) refuses an Apply when the file's bytes differ from the snapshot it took
  when the page opened. So saving a slot cost the player the page. Adopt and autosave write only the slot
  file, leaving `smoothwalker.ini` exactly as the Apply wrote it, so the page keeps working.
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
  `FText("Smoothwalker: Cinematic")` and `IsNewlyDiscovered = false`: the banner appeared. Parameter layout:
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
  is a `RegionEnteredNotificationInfo` whose text starts with `Smoothwalker:`. (0.7.3 recorded the new last
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
of 97b7e501, the commit this DLL builds against, with `HotReloadKey = F11` (mod reload is `Ctrl+F11`).
2c ships the same UE4SS.dll and dwmapi.dll as 2b, byte for byte; it only sets `HookProcessConsoleExec = 1`
(unused here) and moves the GUI key from O to P. Its
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

| Capture | Smoothwalker | n | mean ms | p50 ms | p95 ms | p99 ms | stutters |
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

## Other camera mods and a camera API

Surveyed 2026-09-22 from the Nexus page text of every camera mod listed for the game (mod ids below),
plus the source of FreeCam (350) and PhotoMode with OSD (361). Nothing here was run against the game
yet; the two open checks are at the end.

### Two surfaces, two compatibility stories

Smoothwalker touches the camera in two places. The **view hook** (slot 214) edits the final view after
the game has built it. The **mode writes** (`mode_tuning.hpp`) put distance, height, shoulder and FOV into
the game's camera-mode CDOs and live instances, with the type flip. The hook composes with nearly
everything; the mode writes share their fields with at least eight other mods.

**The view hook composes with:**

- **Control-rotation writers**: Dynamic Follow Camera (351), Combat Camera - Configurable (480, its
  smooth tracking), Centered Lock-On Camera (218, its target follow). They steer the game's camera
  through control rotation each tick; the hook sees the result and trails it. Correct by construction.
  The player gets two layers of softness, and the 90 degree trail cap (0.9.0) is what keeps a fast
  auto-yaw from reversing the follow.
- **Photo-mode mods**: FreeCam (350), PhotoMode with OSD (361), Photo Mode Unlocked (595), PhotoModeBoD
  (369). The photo camera is its own actor with its own component, so the hook returns on the pointer
  compare; `camera_live()` goes false while it holds the view, V and N are ignored, and the return is a
  `reset_gap` snap. No work needed.
- **Targeting hooks**: Free Combat Camera (340 and 568), AXIS (588). They hook lock-on and target
  selection, not the view. The standalone build of 340 ships its own `version.dll` or `winmm.dll`
  proxy, a second injector beside the UE4SS one: an install note for the page, not a Smoothwalker issue.

**One untested hazard on the hook.** If a DLL mod also swaps slot 214, hook order decides chaining. The
guard only checks that the slot differs from `CameraComponent`'s, so a foreign hook passes it, and the
destructor writes back whatever it captured, which unhooks the other mod if it loaded after
Smoothwalker. Bites on hot reload only. Cheap fix: on unload, restore only if the slot still holds
`get_camera_view_hook`, otherwise log and leave it.

**The mode writes conflict with:**

| Mod | Writes | Interaction |
|---|---|---|
| Farther and Centered Camera (236) | `CameraOffsets` distance per mode, one shot, "no polling" | Smoothwalker's next Apply overwrites it silently |
| Camera Presets and First Person View (393) | Distance, pivot, offset, FOV per mode; rebuilds the camera on switch; 2 s file poll | Two writers polling the same fields, last one wins every few seconds |
| Centered Exploration Camera (245), Zelda-Inspired Camera (343), Camera Tweaks (162, 201), Centered Lock-On Camera (218) | Offsets on exploration or combat modes | Same fields as the position groups |
| Wider Lock-On Camera FOV (105) | `DefaultFieldOfView` on the lock-on modes | The FOV group collides |

The specific risk is the baseline. `mode_tuning.hpp` computes every write from "the CDO values captured
the first time a class is seen". If a Lua mod has written the CDO before Smoothwalker first sees that
class, the captured baseline is the other mod's value and every percentage applies on top of it. The
smoothing itself and the lag switch are contested by nobody.

### What Smoothwalker has that nobody else does

The final `FMinimalViewInfo` once per frame, the smoothed pivot, the aiming flag, and a proven
game-thread-to-worker handoff (`Tuning` under an SRW lock, the atomics). It is the one mod at the point
where the view is produced. A per-frame FOV or roll ramp is not reachable from Lua at all
(dawnwalker-toolkit `docs/mods.md`, "Driving the camera from a mod", 2026-09-22: `DefaultFieldOfView`
needs a two-callback type flip and `CapturedCameraComponent.FieldOfView` is dead), so anything
continuous other mods want has to come through this hook.

### Transport

`CppUserModBase::on_lua_start(mod_name, lua, main_lua, async_lua, hook_lua)` fires for every Lua mod as
it starts, with that mod's Lua state (`LuaMod::fire_on_lua_start_for_cpp_mods`, `97b7e501`). Smoothwalker
injects a `Smoothwalker` global table of C functions into each state and removes it in `on_lua_stop`.
No files, no console tricks. UE4SS shared variables are not an option from C++: `LuaMod` is not
exported, so `m_shared_lua_variables` is not reachable across the DLL boundary. A `smoothwalker` console
command mirrors the same calls for C++ mods and scripts. `Smoothwalker.api_version` is an integer,
bumped on any incompatible change.

### Surface, in order of value

1. **Read.** `view()` returns location, rotation, FOV and the smoothed pivot from the last hook call;
   `live()` returns whether the player camera updated in the last 250 ms and, when not, why (paused,
   photo camera, cutscene, load). All already computed; publishing is a few more atomics.
2. **Override layers.** A mod registers a layer of numbers: position offset in pivot space, rotation
   offset, FOV delta or absolute FOV, weight, blend time. The hook applies the layer stack after the
   follow and before the wall clamp, through the same `guarded_write` of `VIEW_BYTES`. This is where a
   scripted camera or a per-frame FOV mod lives. A layer dies with its owner's `on_lua_stop`, or when its
   owner stops refreshing it (a lease, so a crashed script cannot pin the view).
3. **Authority.** `claim()` and `release(mode)`: one owner at a time, first come, refused with a reason
   (`taken`, `already_yours`, `must_keep`, `not_owner`, `bad_thread`). While owned, Smoothwalker keeps the
   follow math running on the game's view but does not write, so `release("glide")` eases back from where
   the owner left the camera and `release("cut")` snaps. Layers are off while owned unless the owner opts
   in. It replaces the `camera_live()` inference with an explicit signal and gives a photo mode or an
   AXIS-style gate a real handshake.
4. **Mode-value service.** `set_mode_value(group, field, value)`: the baseline capture and the type flip
   done once, by one owner, so the mods in the table above stop fighting. Pays off only if their authors
   adopt it; ship it last and document it as the fix for the conflict table.

### What SmoothCam's API teaches (read 2026-09-22)

SmoothCam (Skyrim, `mwilsnd/SkyrimSE-SmoothCam`, `SmoothCamAPI.h` and `modapi.cpp`) exposes a
resource-ownership interface, not camera math: three resources (camera, crosshair, stealth meter), each with
one owner at a time. Not copied; these mechanics transfer:

- **Exclusive ownership with result codes.** `RequestCameraControl` returns OK, AlreadyGiven, AlreadyTaken,
  MustKeep or NotOwner. No priorities: two mods cannot out-rank each other forever, and a refusal names why.
  Adopted above; the priority claim of the first draft is gone.
- **Warm state while owned.** `RequestInterpolatorUpdates` keeps SmoothCam's smoothing running while another
  mod drives, so the handback is seamless. For the hook that is free: run the follow on the game's view, skip
  the write. A photo-mode exit stops being a `reset_gap` snap.
- **The handback is the owner's choice.** `SendToGoalPosition(shouldMoveToGoal, moveNow)` before release:
  snap to the goal, or interpolate from where the owner left the camera. Hence `release("cut" | "glide")`.
- **Goal and shown are both readable.** `GetLastCameraPosition` (rendered) and `GetGoalPosition` (where the
  smoother wants to be, world and player-local). `view()` returns the game's view, the shown view and the pivot.
- **Thread contract as a result code.** `GetSmoothCamThreadId` and `BadThread`. UE4SS Lua has an async thread
  (`LoopAsync`, `ExecuteAsync`): every API call checks the game thread and returns `bad_thread` otherwise.
- **Additive versions, named consumers.** V1, V2, V3 only add; `RegisterConsumer(name)` logs who attached.
  Hence an integer `api_version` that only grows, and `register(name)` returning a handle and logging the name.

Not needed: the SKSE messaging handshake (`on_lua_start` injection replaces it) and the HUD resources.
Not there at all: SmoothCam is all or nothing. Owning the camera means moving it yourself with the engine's own
calls; there are no offsets, FOV, roll or stacking. On this game most camera mods are small offset, FOV and
distance tweaks and Lua has no per-frame view access, so the override layers are the part of this design with
no precedent to check against. One rule borrowed for them: layers are off while a mod owns the camera unless
the owner opts in, as SmoothCam pauses its interpolators unless asked.

### Slice 1 as built (branch `camera-api`, 2026-09-22)

`src/lua_api.hpp`. `on_lua_start` puts a global `Smoothwalker` table into each Lua mod's state; `on_lua_stop` and
the destructor replace every function in it with a pure-Lua stub answering `nil, "unloaded"` and set
`api_version` to 0, so a `local SW = Smoothwalker` held by a consumer never points into an unloaded DLL.
Consumers are keyed by their main `lua_State*` (from `LUA_RIDX_MAINTHREAD`), never by a string they pass.

| Call | Returns |
|---|---|
| `Smoothwalker.api_version` | 1 (integer, only ever grows) |
| `Smoothwalker.mod_version` | `"0.9.0"` |
| `Smoothwalker.register()` | the calling mod's folder name; logs `API consumer '<mod>' registered` once |
| `Smoothwalker.view()` | `{ game = {x, y, z, pitch, yaw, roll, fov}, shown = {...}, pivot = {x, y, z} or nil, age = s }`, or `nil, "no_view"` before the first player-camera update |
| `Smoothwalker.live()` | `bool, age` (age in seconds; `math.huge` before the first update); live means age < 0.25 s |
| `Smoothwalker.enabled()` | the mod's live switch |

The hook publishes the snapshot once per player-camera update through a seqlock (`g_seq` odd while writing,
one writer): the game's view as read, what was handed back (equal when off), the pivot, a QPC stamp. Off and
settled, the hook still publishes game = shown, without a pivot. All slice-1 calls read atomics only, so they
are safe from any thread, including a mod's top level and `LoopAsync`; the game-thread id is captured on the
engine tick for the slices that will need it.

**Tested live 2026-09-22** from UEBridge's Lua state (the bridge's `eval_lua` runs on the game thread):
standing, `view().shown` equalled `view().game` and both matched `PlayerCameraManager:GetCameraLocation()` to
the last digit, pivot about 3 m from the camera, age under 10 ms, `register()` returned `UEBridge` twice and
logged once. Walking in a circle, three single samples: lag 98-101 cm between game and shown, a yaw trail of
-3.8 to +2.9 degrees flipping with the turn (rotation smoothing on, Cinematic), pivot moving metres between
samples, live. Under the pause menu: `live()` false, age 14 s, `enabled()` still true. One testing rule: never
busy-wait inside an eval; it holds the game thread, the camera stops updating, and every later sample in the
same eval is stale.


### Slice 2 as built (branch `camera-api`, 2026-09-22)

The override layer, `api_version` 2. One layer per consumer, eight slots, keyed by the consumer's Lua state.
The probe keys are gone; a layer is what they were standing in for.

| Call | Does |
|---|---|
| `Smoothwalker.layer_set{ offset = {x, y, z}, rotation = {pitch, yaw, roll}, fov = delta, fov_abs = deg, weight = 0..1, blend = s, ttl = s }` | Replaces the caller's layer. `offset` is in the camera's own frame (x forward, y right, z up, cm); `rotation` and `fov` are added; `fov_abs` pulls toward an absolute FOV by `weight`; `weight` scales everything; `blend` eases the change in (0 snaps); `ttl` is a lease, past it the layer fades out as if cleared, refreshed by calling again. Returns `true`, or `nil` with `bad_thread`, `no_game_thread`, `table_expected`, `not_finite`, `no_slot`, `unknown_state`. |
| `Smoothwalker.layer_clear()` | Fades the caller's layer out over its last `blend`. |
| `Smoothwalker.layers()` | `{ {mod, active, fov, fov_abs, weight}, ... }` for every slot with an owner, for bug reports. |

Game thread only for the two writers: the bridge's evals, key binds, hooks and `ExecuteInGameThread` qualify; a
mod's top level and `LoopAsync` do not. `view()`, `live()`, `enabled()`, `register()` stay thread-agnostic.

In the hook (`apply_layers`): the eight layers are copied under a shared SRW lock each update (numbers only,
like `Tuning`); per slot the seven applied numbers ease from their previous values to the target with a
smoothstep over `blend`, on the world delta, restarted on every `generation` change and on an active flip. The
sum is added **after** the follow, the crossfade and the wall clamp, right before the write, and independently
of the O switch: a layer is another mod's feature, so `enabled = 0` stays the clean A/B for this mod's own
work while the other mod keeps its effect. The off-and-settled early return in the hook is skipped while any
layer is live or fading (`g_layers_any`, set by a `layer_set` or clear, cleared by the hook once every slot is
idle). FOV is clamped to 5..170 after the sum. Non-finite results are dropped to 0.
A stopping consumer (`on_lua_stop`) has its layer faded out and its slot freed.

**Tested live 2026-09-22** from UEBridge's state, LongRange mode at FOV 95 (Cinematic):

| Call | Camera manager read | `view()` |
|---|---|---|
| `layer_set{ fov = 20, rotation = {roll = 10}, blend = 0.5 }` | FOV 115, roll 10 | shown 115 / 10, game 95 / 0 |
| `layer_set{ offset = {y = 60}, blend = 0.3 }` (replaces) | FOV 95, roll 0, camera moved | shown minus game: 60.0 cm along the camera's right vector, z 0 |
| `layer_set{ fov_abs = 60, weight = 0.5, ttl = 3, blend = 0.5 }`, sampled every 0.5 s from `LoopAsync` via `view()` | | 77.5 from 0.5 s to 3.0 s, 95.0 from 3.5 s on |
| `layer_set{ fov = 5 }` from a `LoopAsync` callback | | `nil, "bad_thread"`; `live()` answered there |
| roll layer set, then O off | `enabled()` false, roll still 10, follow shift 0 | shown roll 10, game roll 0, live |
| `layer_clear()` | roll back to 0 | `layers()` keeps the slot: `{mod = "UEBridge", active = false}` |

Testing rule from the lease run: the bridge's round trip is several seconds, so a short `ttl` cannot be read
by hand; record with a 500 ms `LoopAsync` reading `view()` (thread-agnostic, no object scan) instead.

### Slice 3 as built (branch `camera-api`, 2026-09-22)

Camera authority, `api_version` 3. One owner at a time, first come, no priorities, keyed by the consumer's main
`lua_State*` like the layers. The owner's identity (that state and its mod name) lives on the game-thread side
under `g_mutex`; the hook only ever sees numbers: `g_owner_slot` (the owner's index in a one-wide owner table, -1
for nobody), `g_owner_keep_layers`, `g_owner_expires` and `g_release_generation`, plus `g_blending`, which it
writes. Ownership is one sample taken at the top of `update_view` and used for that whole update: a release lands
from the game thread while the hook runs on a worker, so reading the slot and the release generation at different
points in one update could write a full follow offset for a frame, which would turn a glide into a cut.

| Call | Returns |
|---|---|
| `Smoothwalker.claim{ keep_layers = bool, ttl = s }` | `true`, or `nil` with `bad_thread`, `no_game_thread`, `unknown_state`, `already_yours` (this consumer already owns it), `taken` (another does), `must_keep` (Smoothwalker's own crossfade is mid-flight), `not_finite`. The argument table is optional; `keep_layers` defaults to false and `ttl` to no lease. |
| `Smoothwalker.release(mode)` | `mode` is `"cut"` (the default when absent) or `"glide"`. `true`, or `nil` with `bad_thread`, `no_game_thread`, `unknown_state`, `not_owner`, `bad_mode`. |
| `Smoothwalker.owner()` | the owning mod's name, or `nil`. Reads under `g_mutex` only, so any thread may call it. |

`claim` and `release` write state the hook reads, so they are game thread only, like the layer writers; `owner()`
is thread-agnostic.

In the hook, while the camera is owned: the follow math runs exactly as it always does, so the pivot smoothing,
the rotation smoothing, the crouch hold and the nominal distance stay warm, and then the view is put back to
byte-exactly what the game built and nothing is written. `g_follow.out_offset`, `out_rotation` and `out_fov`
therefore record a zero offset, an identity rotation and the game's FOV every update, which is what a later
glide starts from. No crossfade of Smoothwalker's own runs while owned (the `cut || owned` arm), so a settings
or mode write landing mid-claim is consumed silently rather than fighting the owner. Layers
(`apply_layers`) are skipped while owned unless the owner claimed with `keep_layers = true`; with them kept, the
layered view is written and `view().shown` is that layered view, otherwise `shown` equals `game`.

`release("cut")` sets `g_reset`, the same flag a teleport or a player swap sets, so the next update snaps the
follow to the capsule. `release("glide")` bumps `g_release_generation`, a new atomic folded into the hook's
`changed` alongside `t.generation`, `g_toggle_generation` and `g_position_generation` (with a `seen_release`
field on `g_follow`): the next update sees `changed && g_follow.out_valid` and starts the existing crossfade
from `from_offset`/`from_rotation`/`from_fov` copied from `out_*`, which is the game's view the owner left on
screen, easing to the follow result over `position_transition`. With `position_transition` at 0 that arm is
skipped and the glide degrades to a cut; that is the user's own setting and is left as is. `must_keep` reads
`g_blending`, published by the hook at the end of every update from `g_follow.blending`, and is only trusted
while the snapshot is live (under 250 ms old), so a pause or a cutscene mid-fade cannot pin a claim out forever.
`g_blending` is published at the end of the update that starts a crossfade, so a claim arriving during that one
update passes rather than answering `must_keep`; the stranded fade is then discarded by the `cut || owned` arm on
the next update and the owner has the screen, which is the point of the claim.

`ttl` is a lease, like the one on a layer: past it the hook stops treating the claim as ownership and the game
thread drops the identity the next time `claim`, `release` or `owner` looks, which is a `release("cut")`. An
expired owner's `release` answers `not_owner`. A claim without a `ttl` is the consumer's own responsibility and
holds the camera until it releases or stops, so anything that can crash or hang should take a lease and refresh
it. Three things end a claim besides `release`: the lease, the consumer stopping (`uninstall`, which
`uninstall_all` runs on unload), and the consumer's Lua state being re-keyed by `install` without a matching
`on_lua_stop` (a hot reload or a script error at load), all of them a cut.

### Rules that carry over

- Every call that hands numbers to the hook runs on the game thread (checked, `bad_thread` otherwise) and
  writes them into a locked struct; the hook reads it. Reads of published state are thread-agnostic. Nothing
  in the hook calls a UObject or UE4SS. Same discipline as `Tuning`.
- Refuse layers whose numbers are not finite; clamp weights to [0, 1] and FOV to [5, 170].
- Layers and claims are keyed by the calling mod's name from `on_lua_start`, never by a string the caller
  supplies, so one mod cannot release another's.
- Unload: clear the injected tables in `on_lua_stop`, drop every layer, restore the slot as above.

### Checks run 2026-09-22

Against the UE4SS.log of that day's session (rc6, build 25232147), the UE 5.5.4 engine source, the UE4SS
source at `97b7e501`, and the public source of Combat Camera - Configurable 3.1.0
(`my-mods/Dawnwalker-Combat-Camera-Configurable`). No build was made.

- **Start order.** C++ mods start before Unreal init (`DWSmoothwalker v0.9.0 loaded` at 13:46:55), Lua
  mods 13 s later (13:47:08), and Smoothwalker's first camera-mode class capture lands only after the
  save loads (`camera position applied` at 13:49:19). So `on_lua_start` fires for every Lua mod, the
  C++ side is always already up, and the baseline risk above is real: a Lua mod's CDO write at its
  start sits two minutes ahead of the capture and would be captured as the game's value.
- **`on_lua_start` in the wild.** Combat Camera - Configurable is a C++ mod on the same host layout
  (`static_assert(sizeof(CppUserModBase) == 192)`) that does exactly this: in `on_lua_start` it
  filters on its own Lua mod's name and `lua.register_function("_CCSet", ...)` into that state; its
  Lua half calls the native side through those functions. Injecting into other mods' states is the
  same call without the name filter. `LuaMadeSimple` at `97b7e501` has `register_function`,
  `prepare_new_table`, `add_pair`, `set_number`, `set_bool`, `get_integer`, `get_number`, `get_string`:
  enough for a table API.
- **Slot 214 elsewhere.** Combat Camera - Configurable does not touch it. It MinHooks game targeting
  functions and one engine function, `APlayerCameraManager::ProcessViewRotation` (RVA `0x17d22f0`,
  which it validates as entry `0x880` of the `RebelPlayerCameraManager` vtable at `0x76a6c58`; its
  `viewOtherThread` counter shows the same off-game-thread camera update this doc measured). Its
  vtable pointers are validated, never written. So it is a control-rotation writer, native side, and
  composes with the hook. Free Combat Camera (340) standalone and AXIS (588) have no public source:
  unknown. No other DLL camera mod is installed here, so a runtime read of slot 214 proves nothing
  yet; the unload guard above costs nothing and goes in regardless.
- **FOV and roll after the hook.** UE 5.5.4 `PlayerCameraManager.cpp`: `UpdateViewTarget` ->
  `UpdateViewTargetInternal` -> `Target->CalcCamera` -> the component's `GetCameraView`, then
  `ApplyCameraModifiers` (camera shakes; the only modifier classes reflected in this game are
  `CinematicCameraShake` and a Perlin shake pattern) and `SetActorLocationAndRotation`. `DoUpdateCamera`
  lerps FOV between view targets during a blend. Nothing else rewrites `POV.FOV` or `POV.Rotation.Roll`;
  `LockedFOV` (the `fov` console command) only changes what `GetFOVAngle()` returns. So a hook-side FOV
  or roll write reaches the render unless `RebelPlayerCameraManager` overrides these natively, which
  reflection cannot show (it has no reflected functions). **Measured with a probe build the same day:**
  `probe_fov = 20`, `probe_roll = 10` (two file-only keys, off by default, added to the view after the
  follow) in the Cinematic preset with the LongRange mode at FOV 95: `PlayerCameraManager:GetFOVAngle()`
  read **115** and `GetCameraRotation().Roll` read **10**, and the picture was visibly wider and tilted.
  `LockedFOV` was unset. So the hook's FOV and roll are what the game renders; nothing downstream
  rewrites them. A per-frame FOV or roll layer is buildable. The probe keys are read at startup only (not in
  `NUMERIC_KEYS`, so the 250 ms reload skips them); setting them to 0 needs a restart, or O turns the mod and the
  probe off together.
- **Loader proxies.** The UE4SS proxy here is `dwmapi.dll`. Free Combat Camera's standalone
  `version.dll` or `winmm.dll` would load beside it as a second proxy, not collide with it: the note
  above is about two injectors, not a filename clash.

## Known limits

- **One game build.** The hook needs `GetCameraView` at vtable slot 214 as on build 25232147; when slot 214
  is not overridden the mod refuses to hook and stays inactive.
- **One UE4SS commit.** The DLL links against `UE4SS.dll` built from `97b7e501`; a loader on another commit
  needs a rebuild against it.
- **The page indicator lags.** After an Apply of a slider, the page stays open with the old `preset` value
  shown until it is reopened, because the corrected indicator is written only once the camera is live again.
  After a preset load the file is written once the menu closes, so the open page keeps the old sliders
  until it is reopened; going back to the mod list and straight into the page again, without leaving the
  menu, also shows the old sliders (the write waits for the menu host to collapse).
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
| A stale banner pointer could match the game's own notification at a reused address | No addresses kept: a queued entry is ours only if it is a `RegionEnteredNotificationInfo` whose text starts with `Smoothwalker:` |
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
