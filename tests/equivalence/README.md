# Equivalence harness (core split, stage 2)

Checks that the split build's hook behaves exactly as the one-piece hook of commit `f22aa8e` (stage 1 done, before
the split). Never shipped: `mod/` is what ships. See `docs/design.md`, "Core and processors".

## Run

From the repo root, in WSL (or any Linux with g++ 13+, git, tar, awk):

```
wsl -d archlinux -- sh tests/equivalence/run.sh                  # 8 sessions x 300,000 frames
wsl -d archlinux -- sh tests/equivalence/run.sh 2 20000          # sessions, frames
wsl -d archlinux -- sh tests/equivalence/run.sh --mutate core    # plant a mutation: must FAIL
wsl -d archlinux -- sh tests/equivalence/run.sh --no-baseline-fixes   # leave the baseline unpatched: must FAIL
EQ_REKEY_CHANCE=0.0005 sh tests/equivalence/run.sh 8 300000      # more re-keys per session (default 0.00001)
```

Exit 0 and `PASS` only if every session matched. Mutations (`core`, `follow`, `processor`) are applied to a copy of
`src/` in `build/`, never to the tree: the crossfade eased linearly, the follow's wall-clamp hold 0.35 s instead of
0.3 s, mode writes not bumping the processor's generation.

## What it compares

- **Baseline:** `f22aa8e`'s headers and its `dllmain.cpp` from `namespace` to the end of `camera_live`, extracted by
  `run.sh`, patched with `baseline-fixes/*.patch` (below), and compiled (`base_driver.cpp`).
- **Split:** the working tree's camera and `smoothwalker/follow/processor.hpp`, joined only through the core's
  interface (`new_driver.cpp`). The camera's own translation units are compiled as the DLL builds them (`NEW_SOURCES`
  in `run.sh`: `camera/authority.cpp`, `camera/hook.cpp`, `camera/pipeline.cpp`, each with `shim/prelude.hpp`
  first); the driver installs the hook on a slot of its own with `hook_slot` and calls `get_camera_view_hook`, and
  the API operations call the product's `Authority` (`new_api_ops.inc`).
- Both link into one program (the baseline's four namespaces renamed `base_*`, the split's root `dw` renamed `new_dw`, on the command line) and get the same events in
  lockstep from one seeded session (`main.cpp`): walking, jumps, teleports under and over `reset_distance`, pauses,
  slow motion, crouches, implausible half heights, a missing half-height offset, NaN and infinite views, odd
  `DeltaTime` (0, negative, NaN, infinite), other cameras' updates, lost root / camera / translation offset, injected
  faults on each guarded memory access, the toggle, settings publishes (every hook key, `position_transition` 0,
  unchanged publishes), mode writes, the aiming / combat / traversal flags, cuts for every reason, two API consumers
  claiming with and without `keep_layers` and leases (renewed, expiring, released by glide or cut, uninstalled,
  re-keyed), layers (weights, blends, leases, clears, non-finite input), log_stats reports and overlay reads.
- After every event: the view bytes the hook left (96-byte buffer, so a write past FOV shows), the API snapshot and
  claim state, every debug-overlay atomic, the view update count and world seconds, the cut flag and reason,
  `Smoothwalker.enabled()`, the layer state, and, when taken, the log_stats counters and the overlay's reads. The two
  records must be equal byte for byte, with one exception: every NaN counts as one value (which NaN an operation on
  two NaNs returns follows operand order, which the compiler may swap). The clocks must match too: the fake
  `QueryPerformanceCounter` advances one tick per call, so both builds must make the same calls in the same order.

## Intended changes since f22aa8e

A change to the mod that the baseline cannot agree with is not a harness failure to wave through: it goes in as a
patch on the extracted baseline, so baseline and working tree again agree bit for bit and the harness keeps its
teeth for everything else. `run.sh` applies `baseline-fixes/*.patch` (in name order, `patch -p1` from the baseline's
`src` parent) right after `git archive`. `--no-baseline-fixes` skips them and defines nothing extra: that run must
FAIL, which shows the harness sees the change. A patch's leading text says what it changes and why. Where the
harness's own operation code (`base_api_ops.inc`) mirrors baseline bookkeeping, it calls the function the patch adds when
`EQ_BASELINE_FIXED` is defined (`run.sh` does that only with the patches applied) and keeps the f22aa8e copy otherwise.

| Patch | Changes | Why |
|---|---|---|
| `0001-rekey-frees-layer-slot.patch` | `lua_api.hpp`: `install` moves its locked bookkeeping into `install_locked`, which frees the layer slot the consumer held (`clear_slot`, then `g_slot_owner[slot].clear()`, as `uninstall` does) before replacing the consumer | f22aa8e replaced the consumer on a re-key (a hot reload or a script error at load, no `on_lua_stop`) and left its slot taken: the old layer kept applying and repeated re-keys used up the 8 slots. The re-key event now differs by design: `api.layers_any` set, `api.layer_generation` and `layer.generation` bumped by the fade-out, but only when the consumer held a layer |

Without the patch, each of the 8 default 8 x 300,000 sessions fails at a re-key event (frame 4,485 to 146,294; a
session has 2 to 8 re-keys, and the first one whose consumer held a layer is the mismatch).

## Files

| File | Contents |
|---|---|
| `run.sh` | Extracts the baseline, applies the baseline fixes, copies the tree (and plants a mutation), builds, runs the sessions in parallel |
| `baseline-fixes/` | Patches for the intended changes since f22aa8e (above) |
| `main.cpp` | Session generator, lockstep driver, comparison, coverage counts; optional third argument (`EQ_REKEY_CHANCE` in `run.sh`) is the re-key chance |
| `harness.hpp` | The driver interface, settings, records |
| `base_driver.cpp`, `new_driver.cpp` | Each build behind that interface; UE4SS-bound feeding code is quoted from the mod |
| `base_api_ops.inc` | The baseline's Lua API game-thread operations without Lua, over f22aa8e's `dwapi::` state; frozen with the baseline |
| `new_api_ops.inc` | The same operations over the working tree: the Lua argument checks, then `Authority`'s own methods; `api_state` reads `Authority::inspect` in the baseline's record order |
| `shim/` | Windows, Lua, UE4SS log stubs; `prelude.hpp` maps SEH to injectable faults (forced first into the camera's `.cpp` files) |

Out of reach (UObjects, UE4SS): discovery, the engine tick, `mode_tuner.hpp`, banners, the overlay widget, the
settings file. Those are checked in game.
