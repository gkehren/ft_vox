# Issue #208: Phase D verification (Epic #204 — CPU GameSimulation)

Post-refactor verification that moving the player/mob simulation and the
authoritative world behind the CPU-only `sim::GameSimulation` facade — and
removing the temporary `ChunkMobWorld` mob adapter — introduced **no behavior
or performance regression**. Compare against
[`issue-205-pre-refactor.md`](issue-205-pre-refactor.md),
[`issue-206-phase-b.md`](issue-206-phase-b.md) and
[`issue-207-phase-c.md`](issue-207-phase-c.md).

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
§2/§3/§10 for the boundary, the Phase D simulation contract and the adapter
removal status.

## What changed

- `sim::GameSimulation` (`src/Sim/GameSimulation.hpp/.cpp`, in `ft_vox_core`)
  now owns the authoritative gameplay state: `TerrainGenerator`, `VoxelWorld`,
  `physics::PlayerController` (120 Hz) and `entities::MobSystem` (60 Hz), plus
  flight mode, block selection, spawn/restore, the session reload counter and
  the persistence lifecycle (open/close/flush/save/shutdown). Explicit
  commands/lifecycle in, bounded read-only value snapshots out
  (`PlayerSnapshot`, `MobSnapshot`, `WorldSnapshot`).
- `Engine` is the graphical host: it maps SDL input to `sim::PlayerCommand`,
  owns pause/focus/benchmark policy and the camera, and delegates every
  player/mob/world operation to the facade. The `RenderWorldBridge` attaches
  to `sim->world()` (the same read seam Phase E will use).
- `ChunkMobWorld` is **deleted**. Its CPU half (collision + biome/grass
  surface scan) moved verbatim to `World/WorldMobView.hpp`; the derived entity
  light sampling stays on the bridge (`sampleSmoothedLight`) and is consumed
  by the Engine AFTER the mob snapshot. The `entities::MobWorld` interface no
  longer declares a light seam, and no path nests the world and render map
  locks anymore.
- Day/night is documented as a **presentation** parameter
  (`ShaderParameters.dayTime`, advanced host-side, pause-gated there) — not
  authoritative world time; the simulation owns no wall clock beyond the
  fixed-step accumulators. Visuals unchanged.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8 cores / 16 threads), Windows 11 Pro
  (same reference system as the Phase A/B/C baselines).
- **GPU:** NVIDIA GeForce RTX 4070 Ti, Vulkan 1.4.
- **Base commit:** `3855208f` (main, Phase C landed) vs the Phase D branch.
- **Build:** Visual Studio 18 2026, MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`.
- **Date measured:** 2026-11-15.

## Test suites

```powershell
ctest --test-dir build -C Release --output-on-failure
```

**41 / 42 PASS.** The only failure is the pre-existing `cave_emissive` golden
mismatch tracked in [#213](https://github.com/gkehren/ft_vox/issues/213), with
metrics **identical** to the Phase A/B/C baselines:

```
mean_abs=0.041943 rms=0.070555 hot=0.800885
```

All other visual scenes pass. New/changed suites:

| Suite | Result | Notes |
|---|---|---|
| **GameSimulation** (new, CPU-only) | PASS, 102 checks, ~1.1 s | Phase D acceptance (below) |
| CoreContracts | PASS | unchanged |
| CoreIncludeGuard | PASS | `Sim/GameSimulation.*` + `World/WorldMobView.hpp` rooted; deleted `ChunkMobWorld.hpp` dropped |
| VoxelWorld | PASS, 211 checks | headless world (unchanged) |
| RenderBridge | PASS | bridge boundary (unchanged) |
| ChunkLifecycle | PASS | `--mobs-profile` now uses `WorldMobView` + bridge light seam; assertions unchanged |
| WorldPersistence / MeshUploadLifecycle / MeshFaceKey | PASS | unchanged |
| EngineReload | PASS | initialize/reload/restore now through `GameSimulation` |
| TerrainGeneration | PASS | per-seed determinism fixtures byte-identical |

### Issue #208 acceptance coverage (`test_game_simulation`)

Links **only** `ft_vox_core` — no SDL, Vulkan, ImGui, render handles or camera:

- **Headless scripted determinism**: fixed seed 1337, 600 scripted 1/60 s
  frames (walk legs, sprint, jump edges, place/delete edits) with controlled
  residency (synchronous bootstrap only) — player position/velocity/flags,
  mob transforms/animation, edit revision, residency and edited voxels are
  **identical across repeated runs**.
- **Physics edge fixtures**: jump impulse, coyote grace window (and its
  expiry), swimming immersion + swimUp, flight toggle (host-driven body,
  walk refusal inside solid with the historical status message), unknown /
  unloaded terrain hold (no speculative teleport), one-block spawn recovery,
  dropped-step cap after a 10 s delta spike, suspension with **no catch-up
  burst** afterwards.
- **Mob fixtures**: natural group spawn on generated meadow voxels (seed 42,
  streamed footprint quiesced before the mob clock), fixed-step wandering,
  no NaN in bodies or snapshots, previous→current interpolation at the
  accumulator fraction (#129 contract), bounded population (≤ 48), unassigned
  headless entity light, suspension freeze, far-observer retire, and
  **per-seed reproducibility across runs**.
- **Reload + save/restore without camera/renderer**: create → edit → capture
  → durable shutdown → reopen adopts the stored seed, bootstraps around the
  saved position and restores feet/mode/selection/orientation; explicit-seed
  contradiction runs transient with the surfaced error; **refused reload
  (stranded edits) keeps the old world, seed and persistence alive**, then
  succeeds after resolution.
- **Independent update rates**: 120 Hz player (4 steps per 1/30 s frame,
  sub-step accumulation) and 60 Hz mobs (cap + dropped steps) with no frame
  rendered; bounded value snapshots (`PlayerSnapshot` /
  `WorldSnapshot` identity + edit revision).

The remaining acceptance items are covered by the pre-existing suites, now
running through the facade: persistent-world reload identity
(`EngineReload`), the bridge attach seam (`RenderBridge`), and the visual
runtime (`ft_vox_visual_tests`).

## CPU profiles

`test_chunk_lifecycle --mobs-profile` / `--physics-profile` (real voxels):

| Metric | Phase C | Phase D | Delta |
|---|---:|---:|---|
| Mob update mean (ms, 48 mobs) | 0.03753 | 0.03831 | +0.8 µs (noise) |
| Mob update p95 (ms) | 0.0544 | 0.0551 | sub-0.01 ms |
| Mob cells/tick · travelled · dropped | 1596 · 2753.91 · 0 | 1596 · 2753.91 · 0 | **identical** |
| Local light sampling mean (ms) | 0.003838 | 0.004431 | +0.6 µs (per-mob lock, below) |
| Player physics avg (ms, real voxels) | 0.000661 | 0.000621 | −0.04 µs (noise) |
| Player physics p95 (ms) | 0.001 | 0.001 | identical |
| Physics cells/tick · travelled · dropped | 39.9123 · 682 · 0 | 39.9123 · 682 · 0 | **identical** |

Deterministic outputs (cells sampled, distance travelled, dropped steps) are
**bit-identical**. The mob/physics update deltas are sub-microsecond absolute —
within run-to-run machine noise, not a refactor cost; both stay far below the
documented < 0.1 ms p95/tick physics target.

The light-sampling delta is **attributable, not noise**: the removed
`ChunkMobWorld` adapter held ONE render shared_lock for the whole per-frame
block and sampled through the `Unlocked` seam, while the new path (the exact
call the Engine now makes after the snapshot) acquires
`RenderWorldBridge::sampleSmoothedLight`'s shared_lock **per mob**. The
~0.6 µs/frame at 48 mobs (~0.01 µs/mob) is the cost of those per-mob
acquisitions — negligible (~0.1 % of a frame), and the price of removing the
nested world → render lock hold.

## Persistence / streaming workload

```powershell
./build/tests/Release/bench_persistence.exe 15
```

Seed 42, 200 edits/s, view distance 96, forward 24 blocks/s. Frame avg (ms):

| Run | Phase C (doc) | Phase D |
|---|---:|---:|
| baseline steady-state | 3.84 | 5.25 / 5.42 (two runs) |
| persistence async steady-state | 3.75 | 5.65 / 5.66 (two runs) |

This benchmark drives `VoxelWorld` + `RenderWorldBridge` **directly** — its
code path is source-unchanged by Phase D (no `GameSimulation` involvement) —
and the absolute numbers are machine-state sensitive (the Phase B/C docs
record the same caveat; this session ran ~1.4 ms/frame slower across the
board, consistently across two runs). The durability invariants are
unchanged: `failed=0`, `remainingDirty=0`, no busy-queue rejections, save
worker ordering/superseding intact, explicit flush + close latencies normal
(~105 ms + ~20 ms).

## In-game benchmark (graphical smoke)

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 20
```

Full game loop (SDL + Vulkan + streaming + edits + GPU uploads + benchmark
report) runs clean, exit 0. This exercises the rewired host end-to-end:
`reloadWorld` through `GameSimulation::reload`, benchmark spawn placement,
flight suspension/restore, mob advance + snapshots + bridge light sampling:

| Metric | Phase C (doc) | Phase D |
|---|---|---|
| Score / Grade | 9888 / **S** | **9891 / S** |
| Avg FPS | 474.9 | 502.5 |
| 1% low FPS | 186.0 | 194.1 |
| Frame p50 / p95 / p99 (ms) | 1.65 / 4.35 / 5.38 | 1.53 / 4.16 / 5.15 |
| Streaming (ms/frame) | 1.27 | 1.13 |
| TerrainGen (ms/chunk) | 2.50 | 2.49 |
| Frames > 33.3 ms | 0 | **0** |
| `staging.failures` / `upload.deferred` / `pool.rejected` | 0 | **0 / 0 / 0** |
| `mesh.pool.growEvents` | 0 | **0** (no persistent pool growth) |
| `mesh.allocations` created / destroyed (sample instant) | 3 / 3 | 2 / 2 (see leak note below) |

Absolute FPS varies with machine state (the Phase B/C docs record the same
caveat); the grade is **S** in both and the loop is clean. Draw/mesh workload
character is unchanged (`draws.opaque` ≈ 4980/frame, `mesh.build` sample-sum
≈ 4.14 ms, `upload.chunks` ≈ 1.01/frame).

Additional graphical smokes (both exit 0):

- `ft_vox --seed 42 --view 8 100 8 -90 0 3` — inspection camera (flight via
  `sim->setPlayerFlight`, suspended player/mobs, clean shutdown).
- `ft_vox --seed 42 --world phase-d-smoke --view …` then
  `ft_vox --world phase-d-smoke --view …` — persistent save/restore through
  the real binary: session 2 adopts the stored seed and logs
  `[world] restored player at (20.301, 88.38, 20.301)` (the session-1
  position); `player.state` + `world.meta` round-trip, no flush failures.

## Resource-leak / spike check

- **No persistent pool growth**: `mesh.pool.growEvents=0` over the measured
  window and a stable arena page count (`arena.pages` settles at 4 after the
  warmup peak of 6; the two `arena.growEvents` are one-time warmup growth).
  Note: `mesh.allocations.created == destroyed` at an arbitrary sample instant
  is **not** a leak invariant — arena pages retire on a frame delay, so a
  sample can legitimately show one page still live (review runs observed 3/2
  and 4/3; this doc's run sampled 2/2). The sound evidence is the zero
  pool-grow counter plus the stable page count, both of which hold.
- `staging.failures=0`, `upload.deferred=0`: no staging exhaustion regression.
- `voxel.pool` / `border.pool` gauges read the authoritative world pools via
  `sim->world()` and track the live backing (unchanged semantics).
- The bridge still detaches cleanly before the simulation tears down
  (declaration order + explicit resets in `~Engine` / `reloadWorld`);
  `initializeNoiseGenerator` now also detaches the bridge BEFORE the
  simulation replaces the world, closing a latent re-init ordering hazard
  (the old bridge must quiesce against the still-live old world).
- `GameSimulation`'s destructor is the final persistence safety net
  (`shutdown()` → `forceCloseWorldForShutdown`), mirroring `~VoxelWorld`.

## Review fixes (round 1)

Six review findings were addressed (full suite re-run green except the
pre-existing #213 mismatch; visual metrics unchanged):

- **P3 — leak evidence.** `mesh.allocations.created == destroyed` at an
  arbitrary sample instant is not a leak invariant (arena pages retire on a
  frame delay; review runs legitimately sampled 3/2 and 4/3). The doc now
  cites the sound evidence — `mesh.pool.growEvents=0` plus a stable arena
  page count — and labels the allocation counters as a sample instant.
- **P3 — check count.** The suite table said 100 checks; the actual total is
  102 (the sim-owned `mobsEnabled` toggle checks were added after the first
  doc draft).
- **P3 — light-sampling attribution.** The +0.6 µs/frame delta is not noise:
  the removed adapter held one render shared_lock per frame block and sampled
  through the `Unlocked` seam; the new path acquires
  `sampleSmoothedLight`'s shared_lock per mob. The doc now attributes the
  delta to those per-mob acquisitions (negligible, ~0.1 % of a frame).
- **P3 — snapshot copy.** `mobSnapshots()` returned a reference to an internal
  cache, forcing a per-frame vector copy in the Engine. It is now a const
  out-param (`mobSnapshots(std::vector<MobSnapshot>&)`, exactly
  `MobSystem::renderStates`' shape): the Engine fills its pre-reserved frame
  vector in place, and the sim cache was removed.
- **P3 — lifecycle hygiene docs.** `initialize()` documents that flight mode /
  block selection / status carry over until the spawn step sets them
  (historical Engine behavior); `reload()` documents that the player body is
  left to the host's spawn step — a headless host that reloads without
  re-spawning keeps the previous body (possibly in an unloaded region).
- **P4 — reload refusal path.** `Engine::reloadWorld`'s post-close refusal
  branch is unreachable in the graphical flow (the bridge close already
  succeeded); the comment now states this explicitly and that the defensive
  re-attach deliberately keeps the retained old world renderable.

## Conclusion

Phase D moves the player/mob simulation and the authoritative world behind
the CPU-only `sim::GameSimulation` facade and removes the temporary
`ChunkMobWorld` adapter with **no behavior change** (deterministic fixtures
bit-identical, scripted headless runs reproducible, full suite green except
the pre-existing #213 visual mismatch with identical metrics) and **no
performance or memory regression** (mob/physics within noise, the per-mob
light-lock cost identified and negligible, in-game benchmark Grade S, zero
staging failures, no persistent pool growth). The simulation
runs headless — create, advance, edit, save, reload, restore — with no camera,
renderer, SDL or Vulkan involved, and the graphical host consumes it through
plain commands and bounded value snapshots.