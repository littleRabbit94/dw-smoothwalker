# Camera API

For Lua mod authors on The Blood of Dawnwalker who want to read or drive Smoothwalker's camera from their
own UE4SS mod.

## What it is

`Smoothwalker` is a global table the Smoothwalker DLL injects into every Lua mod's state at that mod's
`on_lua_start`, before your script's top level runs. There is nothing to `require` and nothing to copy into
your mod folder: if Smoothwalker is installed and loaded, the table is already there when your script starts.
`Smoothwalker == nil` means Smoothwalker is not installed (or loads after your mod, which UE4SS does not
guarantee against, so always check). `Smoothwalker.api_version == 0` means Smoothwalker unloaded or hot
reloaded after your mod grabbed the table: every function in it now answers `nil, "unloaded"` instead of a
stale pointer into a DLL that is no longer there. Check `api_version` once at the call site if you held a
local reference across a long idle period; a `local SW = Smoothwalker` you keep does not go stale on its own,
its functions just start answering `unloaded`.

## Threads

Get this wrong and every call either does nothing or errors out silently into a `nil, reason` you didn't log.

**Thread-agnostic** (call from anywhere): `register()`, `view()`, `live()`, `enabled()`, `layers()`, `owner()`.
These only read atomics or a value taken under a lock; they are safe from a mod's top level, `LoopAsync`,
`ExecuteAsync`, a key bind, a hook, or `ExecuteInGameThread`.

**Game thread only**: `layer_set()`, `layer_clear()`, `claim()`, `release()`. These write state the camera
hook reads on the game thread, so calling them off it fails: `bad_thread` (the call landed on the wrong
thread) or `no_game_thread` (no game-thread tick has run yet, so there is nothing to be right or wrong
against).

A UE4SS Lua mod's top level, `LoopAsync` and `ExecuteAsync` callbacks are **not** the game thread. A
`RegisterKeyBind` callback is not guaranteed to be either, so wrap the actual work in
`ExecuteInGameThread` and route the key bind through that, the way DWFreeCam does it (its "game-thread route"
section): build the runner once, queue it from the key callback, never allocate in the callback itself. On
the shipped UE4SS config (`DefaultExecuteInGameThreadMethod = EngineTick`, `HookEngineTick = 1`) that queue
lands on the game thread; on a `ProcessEvent`-drained config it lands on whatever thread calls
`ProcessEvent`, so a runner should still be defensive. `RegisterHook` callbacks and the body you pass to
`ExecuteInGameThread` run on the game thread and may call the game-thread-only functions directly.

## Reference

All angles are degrees, all distances centimeters, all durations seconds. `view()`'s `offset`-shaped fields
are camera-space: x forward, y right, z up. Rotation order everywhere is pitch, yaw, roll. FOV is degrees,
clamped to 5..170 after any layer sum is applied. `weight` is 0..1. A `blend` of 0 snaps instead of easing.

### `Smoothwalker.api_version`

Integer field, not a call. 4 in this release and only ever grows; a future Smoothwalker adds
functions, it never removes or renames one that shipped. 0 means unloaded, see above. 3 had the same calls,
but a repeat `claim` did not renew the lease (see "Ownership"); require 4 if your mod refreshes a claim.

### `Smoothwalker.mod_version`

String field, Smoothwalker's own version (for example `"0.9.0"`). Useful in bug reports; not meant to be
parsed for feature gating, use `api_version` for that.

| Function | Thread | Returns |
|---|---|---|
| `register()` | any | the calling mod's folder name (string), or `nil, "unknown_state"` if called before `on_lua_start` finished installing the table (should not happen in practice). Idempotent: logs `[DWSmoothwalker] API consumer '<name>' registered` once per mod, safe to call again. |
| `view()` | any | `{ game = View, shown = View, pivot = {x,y,z} or nil, age = seconds }`, or `nil, "no_view"` before the first player-camera update. `View` is `{x, y, z, pitch, yaw, roll, fov}`, world space. `game` is the camera as the game built it before Smoothwalker touched it; `shown` is what actually got written (equal to `game` when the mod is off and settled, or while another mod owns the camera without `keep_layers`). `pivot` is the character pivot the follow is built around, `nil` when not known this update. `age` is how long ago the snapshot was published. |
| `live()` | any | `bool, age_seconds`. `true` when `age < 0.25`; `age` is `math.huge` before the first update. Use this to tell a real camera update from a paused/loading/cutscene game, since `view()` still returns the last snapshot in those states. |
| `enabled()` | any | `bool`: the mod's own live switch (the O key, the ini, the Mod Menu page). Independent of layers and ownership; a layer or a claim still applies while `enabled()` is false, that is a different mod's feature running on top of Smoothwalker's own on/off. |
| `layer_set{ offset, rotation, fov, fov_abs, weight, blend, ttl }` | game | `true`, or `nil, reason` with `bad_thread`, `no_game_thread`, `table_expected` (arg 1 must be a table), `not_finite` (a numeric field was NaN/inf/non-number), `no_slot` (all 8 layer slots taken by other mods), `unknown_state`. See "Layers" below. |
| `layer_clear()` | game | `true`, or `nil, reason` with `bad_thread`, `no_game_thread`, `unknown_state`. Fades your layer out over its own last `blend`; does not free your slot, `layer_set` reuses it. |
| `layers()` | any | `{ {mod, active, fov, fov_abs, weight}, ... }`, one entry per slot that has ever been claimed, for diagnosing who holds what. `fov_abs` is present only when that layer set one. |
| `claim{ keep_layers, ttl }` | game | `true`, or `nil, reason` with `bad_thread`, `no_game_thread`, `unknown_state`, `already_yours` (you already hold it; if this call carried a `ttl`, your lease restarted from now and `keep_layers` took this call's value), `taken` (someone else holds it), `must_keep` (Smoothwalker's own crossfade is mid-flight; try again shortly), `not_finite`. See "Ownership" below. |
| `release(mode)` | game | `mode` is `"cut"` (default when omitted or nil) or `"glide"`. `true`, or `nil, reason` with `bad_thread`, `no_game_thread`, `unknown_state`, `not_owner`, `bad_mode` (mode was neither string). |
| `owner()` | any | the owning mod's name (string), or `nil` if nobody holds it or the holder's lease expired. Never releases anything itself, pure read. |

### Layers

One layer per mod, eight slots total, first come first served: the first `layer_set` a mod calls claims a
slot for the life of the mod (until `on_lua_stop` or unload), and every later `layer_set` from that mod
replaces the same slot's contents rather than taking a new one. `layer_clear` fades the slot to nothing but
does not give it back to the pool.

`layer_set` fields, all optional:

| Field | Meaning | Default |
|---|---|---|
| `offset = {x,y,z}` | camera-space move, x forward / y right / z up, cm | `{0,0,0}` |
| `rotation = {pitch,yaw,roll}` | added rotation, degrees | `{0,0,0}` |
| `fov` | added FOV, degrees | `0` |
| `fov_abs` | pulls FOV toward this absolute value, degrees | none |
| `weight` | scales everything above, 0..1, clamped | `1` |
| `blend` | seconds to ease the change in (and out, on clear); `0` snaps | `0` |
| `ttl` | lease in seconds; past it the layer fades out as if cleared. Refresh by calling `layer_set` again before it expires. `0` or omitted: no lease, holds until cleared or the mod stops. | `0` |

`fov` and `fov_abs` combine: `fov_abs` contributes `(fov_abs - current_fov) * weight`, `fov` contributes
`fov * weight`, both added to the game's FOV, then the sum across all layers is clamped to 5..170.

While another mod owns the camera (see Ownership), layers are skipped entirely unless the owner claimed
with `keep_layers = true`; `view().shown` reflects whichever is true. A mod's own `enabled()` switch does not
gate other mods' layers, only its own on/off feature.

A mod that stops (script error, hot reload, `on_lua_stop`, unload) has its layer faded out on its last
`blend` and its claim released with a cut, automatically, no cleanup call needed from you.

### Ownership

One owner at a time, no priorities: `claim()` succeeds for whoever calls it first, everyone else gets
`taken` until that owner releases, its lease runs out, or it stops. While you own the camera the follow math
keeps running underneath (so the handback is warm, not a snap back to a stale offset), but nothing is
written unless you asked for `keep_layers = true`.

`claim{ keep_layers = bool, ttl = seconds }`: `keep_layers` (default `false`) keeps other mods' layers
applying on top of your ownership; leave it off if you are driving the camera yourself and do not want
another mod's offset fighting you. `ttl` (default `0`, no lease) is how long the claim survives without a
refresh; a claim that can crash or hang your mod should take a lease and refresh it. A claim can fail with
`must_keep` if Smoothwalker's own crossfade is mid-flight; that is transient, retry a frame or two later.

Refreshing (`api_version` 4): call `claim{ ttl = seconds }` again before the lease expires. While you hold the
camera, a repeat claim answers `nil, "already_yours"`; when it carries a `ttl` it also restarts the lease from
now with that `ttl` and sets `keep_layers` to this call's value (default `false`, so pass it again if you
claimed with it). A switch of `keep_layers` lands on the next update, not eased. A repeat claim without a
`ttl` changes nothing, so a bare `claim()` never strips your lease. Treat `already_yours` as success. A
refresh that arrives after the lease ran out is a new claim: the old one has already ended with a cut, and
the call answers `true` (or `taken`/`must_keep`, like any first claim). On `api_version` 3 a repeat claim
answered `already_yours` without renewing anything.

`release(mode)`: `"cut"` (default) snaps the camera back to the game's own follow on the next update.
`"glide"` eases from wherever you left the camera into the warm follow over the mod's configured transition
time; if that transition is set to 0 by the player, a glide degrades to a cut, that is the player's own
setting and Smoothwalker leaves it alone.

`owner()` returns `nil` once a lease has expired, without itself releasing anything: the actual drop happens
the next time the game thread runs `claim` or `release` and notices. Do not treat a `nil` from `owner()` as a
guarantee the slot is free to claim in the same breath, call `claim()` and handle `taken` normally.

A mod that stops while it owns the camera has its claim released with a cut, automatically.

## Recipes

### (a) A zoom key that toggles fov_abs

```lua
local zoomed = false
local function toggleZoom()
    zoomed = not zoomed
    if zoomed then
        Smoothwalker.layer_set{ fov_abs = 60, blend = 0.4 }
    else
        Smoothwalker.layer_clear()
    end
end
local runZoom = guarded(toggleZoom)  -- guarded(): checks IsInGameThread, pcalls, logs
RegisterKeyBind(Key.F9, function() ExecuteInGameThread(runZoom) end)
```

### (b) A dutch-angle layer with a leased refresh

```lua
-- A ttl means a stalled or crashed LoopAsync stops the tilt instead of leaving it stuck forever.
local tiltOn = false
local function refreshTilt()
    if not tiltOn then return end
    Smoothwalker.layer_set{ rotation = { roll = 8 }, blend = 0.3, ttl = 1.0 }
end
local runRefreshTilt = guarded(refreshTilt)
LoopAsync(500, function()
    if tiltOn then ExecuteInGameThread(runRefreshTilt) end
    return false
end)
```

### (c) A photo-mode style handoff

```lua
local function enterPhoto()
    local ok, reason = Smoothwalker.claim{ ttl = 5.0 }
    if not ok then
        if reason == "taken" or reason == "must_keep" then log("camera busy: %s", reason) end
        return
    end
    -- drive the camera yourself here (your own actor, hook, or reflection writes)
end
local function exitPhoto()
    local ok, reason = Smoothwalker.release("glide")
    if not ok and reason ~= "not_owner" then log("release failed: %s", reason) end
end
```

### (d) Reading the rendered view for an overlay

```lua
local function currentFov()
    local v, reason = Smoothwalker.view()
    if not v then return nil, reason end  -- "no_view": nothing published yet
    return v.shown.fov
end
```

## Versioning and support

`api_version` only ever grows; check it once, not every call, if your mod needs a feature newer than 1
(2: layers, 3: `claim`/`release`/`owner`, 4: a repeat `claim` with a `ttl` renews the lease).
`mod_version` is Smoothwalker's own release string, useful context in a bug report but not a feature gate.
Call `register()` at your mod's top level (thread-agnostic, safe there): it costs nothing and the
`[DWSmoothwalker] API consumer '<name>' registered` log line it produces the first time is often the fastest
way to confirm your mod actually saw Smoothwalker's table before anything else went wrong. `layers()` and
`owner()` are diagnostic reads, thread-agnostic, useful from a console command to show who holds a slot or
the camera without having to reproduce the bug on the game thread. Report issues against
[littleRabbit94/dw-smoothwalker](https://github.com/littleRabbit94/dw-smoothwalker).
