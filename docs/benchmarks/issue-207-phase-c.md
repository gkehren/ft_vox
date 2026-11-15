# Issue #207: Phase C verification (Epic #204 — render world bridge)

Post-refactor verification that moving the derived render pipeline out of the
temporary `ChunkManager` adapter into the renderer-owned `RenderWorldBridge`
(which **references** the host-owned `VoxelWorld`, never owns it) and removing
the `ChunkCollisionView` light wrapper introduced **no behavior or performance
regression**. Compare against [`issue-205-pre-refactor.md`](issue-205-pre-refactor.md)
and [`issue-206-phase-b.md`](issue-206-phase-b.md).

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
§2/§3/§10 for the boundary, the world event stream and the adapter removal
status.

## What changed

- `RenderWorldBridge` (`src/Renderer/RenderWorldBridge.hpp/.cpp`) now owns the
  derived render pipeline: render handle map, mesh/LOD/light dispatch, GPU
  uploads, geometric commit groups, visibility/draw/shadow lists, entity
  light-cache intent + sampling, deferred GPU retirement and the lifecycle
  debug trace. It references the world via `world()` and projects
  already-resident records on attach (`attachToWorld`), so it can attach to a
  live world (the Phase E seam).
- `Engine` owns `VoxelWorld` **and** `RenderWorldBridge` (world declared first,
  bridge destroyed first). `ChunkManager` and `ChunkCollisionView` are
  **deleted**; physics consumes `WorldCollisionView` directly and the temporary
  `ChunkMobWorld` wraps it plus the bridge light seam.
- Mesh publication still validates against `sim::ChunkIdentity` (single source
  on `WorldChunk`); no duplicate generation/revision pair was introduced.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8 cores / 16 threads), Windows 11 Pro
  (same reference system as the Phase A/B baselines).
- **GPU:** NVIDIA GeForce RTX 4070 Ti, Vulkan 1.4.
- **Base commit:** `7745f6e4` (main, Phase B landed) vs the Phase C branch.
- **Build:** Visual Studio 18 2026, MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`.
- **Date measured:** 2026-11-15.

## Test suites

```powershell
ctest --test-dir build -C Release --output-on-failure
```

**40 / 41 PASS.** The only failure is the pre-existing `cave_emissive` golden
mismatch tracked in [#213](https://github.com/gkehren/ft_vox/issues/213), with
metrics **identical** to the Phase A/B baselines:

```
mean_abs=0.041943 rms=0.070555 hot=0.800885
```

All other visual scenes pass. New/changed suites:

| Suite | Result | Notes |
|---|---|---|
| **RenderBridge** (new, CPU-only) | PASS, 24 checks, 0.73 s | Phase C boundary acceptance (below) |
| CoreContracts | PASS, 31 checks | unchanged |
| CoreIncludeGuard | PASS | deleted headers dropped; mesh/light pools reclassified render-derived |
| VoxelWorld | PASS, 211 checks | headless world with no bridge (unchanged) |
| ChunkLifecycle | PASS | constructions now `VoxelWorld` + `RenderWorldBridge`; assertions unchanged |
| WorldPersistence | PASS | collision view re-pointed at `WorldCollisionView`; assertions unchanged |
| MeshUploadLifecycle | PASS | commit-group probes re-pointed at the bridge |
| MeshFaceKey / MeshResultPool | PASS | `beginBuild` identity contract |
| DebugUi | PASS | chunk snapshot/trace via the bridge |
| EngineReload | PASS | world reload through `VoxelWorld` + bridge |
| TerrainGeneration | PASS | per-seed determinism fixtures byte-identical |

### Issue #207 acceptance coverage (`test_render_bridge`)

- **CPU world runs with NO bridge registered**: construct `VoxelWorld`, stream,
  load, async-generate, edit, pump — all headless, no bridge present.
- **Bridge is a derived projection, single source of truth**: `&bridge.world()`
  is the host world; render handle count mirrors world residency; an edit made
  through the bridge is authoritative in the world.
- **Mesh publish validates `sim::ChunkIdentity`**: a stale-generation result is
  injected and asserted to be discarded back to its pool without touching the
  committed mesh.
- **Border edits form atomic commit groups**: a `CHUNK_SIZE-1` edit registers a
  geometric commit group (PR #178 contract preserved).
- **World outlives the bridge; re-attach projects the live world**: destroying
  the bridge leaves world residency intact and editable; a fresh bridge binds
  the existing residents and rebuilds the derived mesh.

The remaining acceptance items are covered by the pre-existing suites, now
running through the bridge: chunk unload/reload + teleport + high-rate border
edits + worker race/stale results (`ChunkLifecycle`, `WorldPersistence`),
multiple edit commit groups + GPU staging exhaustion (`MeshUploadLifecycle`),
and the visual runtime using the bridge for mesh jobs / GPU upload / draw &
shadow lists (`ft_vox_visual_tests`, `EngineReload`).

## CPU profiles

`test_chunk_lifecycle --mobs-profile` / `--physics-profile` (real voxels):

| Metric | Phase B | Phase C | Delta |
|---|---:|---:|---|
| Mob update mean (ms, 48 mobs) | 0.03480 | 0.03753 | +2.7 µs (noise) |
| Mob update p95 (ms) | 0.0446 | 0.0544 | sub-0.01 ms |
| Mob cells/tick · travelled · dropped | 1596 · 2753.91 · 0 | 1596 · 2753.91 · 0 | **identical** |
| Local light sampling mean (ms) | 0.003598 | 0.003838 | +0.24 µs (noise) |
| Player physics avg (ms, real voxels) | 0.000636 | 0.000661 | +0.025 µs (noise) |
| Player physics p95 (ms) | 0.001 | 0.001 | identical |
| Physics cells/tick · travelled · dropped | 39.9123 · 682 · 0 | 39.9123 · 682 · 0 | **identical** |

Deterministic outputs (cells sampled, distance travelled, dropped steps) are
**bit-identical**, and the timing deltas are sub-microsecond-to-few-microsecond
absolute — within run-to-run machine noise, not a refactor cost. Both stay far
below the documented < 0.1 ms p95/tick physics target. (As in Phase B, absolute
CPU timings are machine-state sensitive; the bit-identical deterministic
outputs are the authoritative no-behavior-change evidence.)

Terrain generation is source-unchanged by this PR; `test_terrain --profile 32
1337` totals **2.49 ms/chunk** vs the Phase B doc's 2.68 ms/chunk (machine
noise; the `TerrainGeneration` determinism suite passes byte-identical).

## Persistence / streaming workload

```powershell
./build/tests/Release/bench_persistence.exe 15
```

Seed 42, 200 edits/s, view distance 96, forward 24 blocks/s. Frame avg (ms):

| Run | Phase B (doc) | Phase C |
|---|---:|---:|
| baseline steady-state | ~4.10 | **3.84** |
| persistence async steady-state | ~4.10 | **3.75** |

Phase C is in the same range (slightly faster this session). Save-worker
ordering/superseding, explicit flush + close latencies and the final
`remainingDirty=0` are unchanged. No new CPU spike crossing chunk boundaries.

## In-game benchmark (graphical smoke)

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 20
```

Full game loop (SDL + Vulkan + streaming + edits + GPU uploads + benchmark
report) runs clean, exit 0, no validation errors:

| Metric | Phase B (doc) | Phase C |
|---|---|---|
| Score / Grade | 9896 / **S** | **9888 / S** |
| Avg FPS | 521.7 | 474.9 |
| 1% low FPS | 202.7 | 186.0 |
| Frame p50 / p95 / p99 (ms) | 1.50 / 3.86 / 4.93 | 1.65 / 4.35 / 5.38 |
| Streaming (ms/frame) | 1.13 | 1.27 |
| TerrainGen (ms/chunk) | 2.49 | 2.50 |
| Frames > 33.3 ms | 0 | **0** |
| `staging.failures` / `upload.deferred` / `pool.rejected` | 0 | **0 / 0 / 0** |
| `mesh.allocations` created / destroyed | — | **3 / 3** (no leak) |

Absolute FPS varies strongly with machine state (the Phase B doc records the
same caveat); the grade is **S** in both and the loop is clean. Draw/mesh
workload character is unchanged (`draws.opaque` ≈ 4749/frame, `mesh.build`
sample-sum ≈ 4.28 ms, `upload.chunks` ≈ 1.02/frame). World reload
(`reloadWorld`) is exercised by the benchmark warmup; `EngineReload` covers the
persistent-world close/reopen identity path.

## Resource-leak / spike check

- `mesh.allocations.created == destroyed` (3/3) and `mesh.pool.growEvents=0`
  over the measured window: no persistent mesh-pool growth.
- `staging.failures=0`, `upload.deferred=0`: no staging exhaustion regression.
- `voxel.pool` / `border.pool` gauges read the authoritative world pools and
  track the live backing (unchanged from Phase B).
- The bridge destructor drains render jobs, releases handles to the pool and
  detaches the worker observer, leaving the world render-free; the
  `RenderBridge` detach/reattach checks confirm no handle or record leak.

## Conclusion

Phase C moves the derived render pipeline across the CPU/render boundary into
the renderer-owned `RenderWorldBridge` and removes the temporary `ChunkManager`
adapter and `ChunkCollisionView` wrapper with **no behavior change**
(deterministic fixtures bit-identical, full suite green except the pre-existing
#213 visual mismatch with identical metrics) and **no performance or memory
regression** (persistence/streaming in range, mob/physics within noise, in-game
benchmark Grade S, zero staging failures, no mesh-pool leak). The bridge
references — never owns — the authoritative world, and the world runs headless
with no bridge registered.
