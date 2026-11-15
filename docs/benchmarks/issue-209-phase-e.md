# Issue #209: Phase E verification (Epic #204 — graphical host integration)

Post-refactor verification that completing the graphical host integration —
documented frame orchestration, the single suspension policy, the shared
player-aware placement rule and the world-generation epoch barrier —
introduced **no behavior or performance regression**. Compare against
[`issue-205-pre-refactor.md`](issue-205-pre-refactor.md),
[`issue-206-phase-b.md`](issue-206-phase-b.md),
[`issue-207-phase-c.md`](issue-207-phase-c.md) and
[`issue-208-phase-d.md`](issue-208-phase-d.md).

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
§2/§3/§10 for the boundary and the Phase E graphical host contract.

## What changed

- `Engine::run` is the documented 16-step frame orchestrator; the cohesive
  blocks moved into named steps (`tickMobs`, `updateFrameUniforms`,
  `recordFrame`, `updateWindowTitle`) with **identical profiler scope names
  and nesting**, so benchmark metrics reference the same frames. No step was
  reordered: acquire stays before the simulation steps (paced dt is sampled
  acquire-to-acquire; failed acquire / minimize resync the frame clock).
- `Engine::simulationSuspended()` is the single host suspension predicate
  (paused || unfocused || benchmark-active); the freeze matrix (pause, focus,
  minimize, benchmark, inspection) is documented in `Engine.hpp` and
  `docs/engine-architecture.md`. Semantics unchanged.
- The player-aware placement rule moved from the Engine's right-mouse
  handler into `sim::GameSimulation::canPlaceVoxel` (pure query, verbatim
  rule): graphical and headless hosts now share the same gameplay
  validation; the accepted edit still publishes through the bridge.
- **World-epoch barrier**: `Engine::resetWorldProjection()` runs on every
  world transition before the old bridge is detached — draw/shadow lists,
  mob presentation states (also pushed empty to `WorldRenderer::setMobs`),
  block highlight, upload budget, UI biome cache and biome map are dropped.
  The replacement `RenderWorldBridge` is stamped with the simulation's
  `worldGenerationId()` at attach (`worldGenerationId()` accessor);
  standalone/test bridges default to epoch 0. Async quiesce stays in the
  bridge destructor; the persistent close stays refusable.
- No temporary adapters remain (all removed in Phases C/D); no UI design
  changes; no new GPU waits (uploads stay inside frame recording).
- **Linux build fixes (behavior-neutral portability):**
  `Chunk/ChunkBorders.hpp` now includes `<memory>` (it used `std::unique_ptr`
  relying on MSVC's transitive includes; GCC 14 does not provide them — the
  base commit does not compile on Linux without this). The Linux verify
  build uses `glslc` (the CMake `GLSLC` path, same compiler family as the
  Windows reference build); the `glslangValidator` fallback rejects the
  shaders' `#include` directives without `GL_GOOGLE_include_directive`.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8 cores / 16 threads), Windows 11 Pro
  (same reference system as the Phase A/B/C/D baselines).
- **GPU:** NVIDIA GeForce RTX 4070 Ti, Vulkan 1.4.
- **Base commit:** `1b30c07f` (main, Phase D landed) vs the Phase E branch.
- **Build:** Visual Studio 18 2026, MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`.
- **Linux cross-check:** WSL2 Debian, g++ 14.2, CMake 3.31, system SDL3 /
  Vulkan loader, glslc 2025.2 (userspace extraction of the Debian package).
- **Date measured:** 2026-11-15.

## Test suites

```powershell
ctest --test-dir build -C Release --output-on-failure
```

**41 / 42 PASS.** The only failure is the pre-existing `cave_emissive` golden
mismatch tracked in [#213](https://github.com/gkehren/ft_vox/issues/213), with
metrics **identical** to the Phase A/B/C/D baselines:

```
mean_abs=0.041943 rms=0.070555 hot=0.800885
```

All other visual scenes pass. New/changed suites:

| Suite | Result | Notes |
|---|---|---|
| **GameSimulation** (CPU-only) | PASS, 112 checks (+10) | new player-aware placement-rule fixture (`canPlaceVoxel`: walking refusal, non-solid/flying allowance, pure-query, authoritative publication) |
| **EngineReload** (GPU) | PASS, 27 checks (+10) | epoch barrier: generation monotonic across initialize/reload, bridge epoch == sim epoch at every attach, draw/shadow/mob derived state dropped on reload |
| **RenderBridge** | PASS, 32 checks (+3) | bridge epoch stamp at attach/reattach; standalone default 0 |
| CoreContracts / CoreIncludeGuard | PASS | `Physics/BlockPhysics.hpp` already in the core closure; no forbidden edge |
| VoxelWorld | PASS, 211 checks | unchanged |
| ChunkLifecycle / WorldPersistence / MeshUploadLifecycle / MeshFaceKey | PASS | unchanged |
| TerrainGeneration | PASS | per-seed determinism fixtures byte-identical |

### Linux cross-check (WSL2 Debian, g++ 14.2)

The full graphical target **and all test targets build clean** on Linux
(`cmake --build -j16`, Release) with the two portability fixes above.
`ctest`: **40 / 42 pass**, 2 skipped (no physical GPU: the WSL Vulkan device
is **llvmpipe**, a software rasterizer). `EngineReload` — including the new
epoch-barrier checks — **passes on Linux** too. The two failures are
**pre-existing platform issues, verified identical on the base commit**:

- `WorldPersistence`: 15 sparse-diff checks fail because the SaveService
  worker's regenerated terrain base diverges from load-time generation under
  GCC (the Phase A doc's FMA-contraction caveat: cross-target bit-identity is
  not guaranteed). **Verified pre-existing:** `origin/main` + only the
  `<memory>` include fix reproduces the exact same 15 failures. The
  canonical shared flag set / cross-platform determinism work belongs to
  #210 / #211; Phase E does not touch terrain or persistence code.
- `VisualRegression`: golden scenes were captured on the canonical RTX 4070
  Ti; llvmpipe renders outside tolerance (and its GPU readback timing fails
  the auto-exposure adaptation probe). Cross-GPU pixel identity is an
  explicit Epic non-goal; the canonical-GPU gate is the Windows run above.

## CPU profiles

`test_chunk_lifecycle --mobs-profile` / `--physics-profile` (real voxels):

| Metric | Phase D | Phase E | Delta |
|---|---:|---:|---|
| Mob update mean (ms, 48 mobs) | 0.03831 | 0.03603 | −2.3 µs (noise) |
| Mob update p95 (ms) | 0.0551 | 0.0503 | sub-0.01 ms |
| Mob cells/tick · travelled · dropped | 1596 · 2753.91 · 0 | 1596 · 2753.91 · 0 | **identical** |
| Local light sampling mean (ms) | 0.004431 | 0.004191 | −0.2 µs (noise) |
| Player physics avg (ms, real voxels) | 0.000621 | 0.000668 | +0.05 µs (noise) |
| Player physics p95 (ms) | 0.001 | 0.001 | identical |
| Physics cells/tick · travelled · dropped | 39.9123 · 682 · 0 | 39.9123 · 682 · 0 | **identical** |

Deterministic outputs (cells sampled, distance travelled, dropped steps) are
**bit-identical**; the update deltas are sub-microsecond machine noise.

## In-game benchmark (graphical smoke)

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 20
```

Full game loop (SDL + Vulkan + streaming + edits + GPU uploads + benchmark
report) runs clean, exit 0 — exercising the rewired host end-to-end:
`reloadWorld` through the epoch barrier + `GameSimulation::reload`, benchmark
spawn placement, flight suspension/restore, mob advance + snapshots + bridge
light sampling. Clean run (no concurrent workload):

| Metric | Phase D (doc) | Phase E |
|---|---|---|
| Score / Grade | 9891 / **S** | **9894 / S** |
| Avg FPS | 502.5 | 543.4 |
| 1% low FPS | 194.1 | 197.5 |
| Frame p50 / p95 / p99 (ms) | 1.53 / 4.16 / 5.15 | 1.54 / 3.69 / 5.06 |
| Streaming (ms/frame) | 1.13 | 1.02 |
| TerrainGen (ms/chunk) | 2.49 | 2.34 |
| Frames > 33.3 ms | 0 | **0** |
| `staging.failures` / `upload.deferred` / `pool.rejected` | 0 / 0 / 0 | **0 / 0 / 0** |
| `mesh.pool.growEvents` | 0 | **0** |

Absolute FPS varies with machine state (the Phase B/C/D docs record the same
caveat); the grade is **S** in both and the loop is clean. Draw/mesh workload
character is unchanged (`draws.opaque` ≈ 5198/frame, `mesh.build` sample-sum
≈ 3.69 ms, `upload.chunks` ≈ 0.99/frame, `arena.pages` settles at 5 after the
warmup peak of 6).

Additional graphical smokes (both exit 0):

- `ft_vox --seed 42 --view 8 100 8 -90 0 3` — inspection camera (flight via
  `sim->setPlayerFlight`, suspended player/mobs, clean shutdown).
- `ft_vox --seed 42 --world phase-e-smoke --view …` then
  `ft_vox --world phase-e-smoke --view …` — persistent save/restore through
  the real binary: session 2 adopts the stored seed and logs
  `[world] restored player at (8, 98.38, 8)` (the session-1 position);
  `player.state` + `world.meta` round-trip, no flush failures.

## Resource-leak / spike check

- **No persistent pool growth**: `mesh.pool.growEvents=0` over the measured
  window; stable arena page count (5 resident after the warmup peak of 6).
- `staging.failures=0`, `upload.deferred=0`: no staging exhaustion regression.
- The epoch barrier adds **no GPU waits**: `reloadWorld` keeps the single
  pre-existing device-idle + retire flush around the bridge swap; per-frame
  uploads stay inside `recordFrame`.
- The bridge still detaches cleanly before the simulation tears down
  (declaration order + explicit resets in `~Engine` / `reloadWorld` /
  `initializeNoiseGenerator`); the barrier clears the derived CPU snapshots
  that outlive the bridge handles (draw/shadow lists, mob states, highlight,
  biome caches).

## Conclusion

Phase E completes the graphical host integration with **no behavior change**
(deterministic fixtures bit-identical, full Windows suite green except the
pre-existing #213 visual mismatch with identical metrics, Linux build fixed
and its two test failures verified pre-existing on the base commit) and
**no performance or memory regression** (mob/physics within noise, in-game
benchmark Grade S at parity, zero staging failures, no persistent pool
growth). The Engine is a pure graphical host: commands in, snapshots out,
one suspension policy, and an explicit world-epoch barrier guaranteeing no
stale render snapshot or async job survives a world reload.
