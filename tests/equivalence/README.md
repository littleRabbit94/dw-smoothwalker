# Equivalence harness (core split, stage 2)

Checks that the split build's hook behaves exactly as the one-piece hook of commit `47f6645` (stage 1 done, before
the split). Never shipped: `mod/` is what ships. See `docs/design.md`, "Core and processors".

## Run

From the repo root, in WSL (or any Linux with g++ 13+, git, tar, awk):

```
wsl -d archlinux -- sh tests/equivalence/run.sh                  # 8 sessions x 300,000 frames
wsl -d archlinux -- sh tests/equivalence/run.sh 2 20000          # sessions, frames
wsl -d archlinux -- sh tests/equivalence/run.sh --mutate core    # plant a mutation: must FAIL
```

Exit 0 and `PASS` only if every session matched. Mutations (`core`, `follow`, `processor`) are applied to a copy of
`src/` in `build/`, never to the tree: the crossfade eased linearly, the follow's wall-clamp hold 0.35 s instead of
0.3 s, mode writes not bumping the processor's generation.

## What it compares

- **Baseline:** `47f6645`'s headers and its `dllmain.cpp` from `namespace` to the end of `camera_live`, extracted by
  `run.sh` and compiled unchanged (`base_driver.cpp`).
- **Split:** the working tree's `core/pipeline.hpp` and `sw/processor.hpp`, joined only through the core's table
  (`new_driver.cpp`).
- Both link into one program (namespaces renamed `base_*` / `new_*` on the command line) and get the same events in
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

## Files

| File | Contents |
|---|---|
| `run.sh` | Extracts the baseline, copies the tree (and plants a mutation), builds, runs the sessions in parallel |
| `main.cpp` | Session generator, lockstep driver, comparison, coverage counts |
| `harness.hpp` | The driver interface, settings, records |
| `base_driver.cpp`, `new_driver.cpp` | Each build behind that interface; UE4SS-bound feeding code is quoted from the mod |
| `api_ops.inc` | The Lua API's game-thread operations without Lua, over either build's state |
| `shim/` | Windows, Lua, UE4SS log stubs; `prelude.hpp` maps SEH to injectable faults |

Out of reach (UObjects, UE4SS): discovery, the engine tick, `mode_tuning.hpp`, banners, the overlay widget, the
settings file. Those are checked in game.
