# Issue #205: pre-refactor baselines (Epic #204, Phase A)

Reproducible CPU/world/physics/save/visual/streaming baselines captured
**before** any behavior migration, so later phases (B–G) can prove the
simulation/render split introduced no regression. Phase A only extracts
already-independent CPU translation units into `ft_vox_core` and adds value
contracts; it changes no gameplay or graphical behavior. These numbers are the
reference point.

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
for the boundary design and migration checkpoints.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8 cores / 16 threads), 31.2 GB RAM,
  Windows 11 Pro 10.0.26300.
- **GPU:** NVIDIA GeForce RTX 4070 Ti, driver 32.0.16.2114, Vulkan 1.4.357
  (canonical visual-regression GPU). Also present: AMD Radeon integrated +
  USB Mobile Monitor Virtual Display (not used).
- **Base commit:** `4f931d6cdb007268ef21cc853e1ed6a7168ef2bf` (2026-10-12).
- **Build:** Visual Studio 18 2026, MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`; `glslc`
  `D:/VulkanSDK/1.3.290.0/Bin/glslc.exe`. GLM/FastNoise2 via FetchContent.
- **Date measured:** 2026-11-14.

All commands run from the repository root against the Release build in
`build/`. Seeds and settings are stated per section so each run is reproducible.

## Terrain generation

```powershell
./build/tests/Release/test_terrain.exe --profile 32 1337
./build/tests/Release/test_terrain.exe --world-stats 16 3
```

`--profile 32 1337`: seed 1337, 32×32 = 1024 chunks. Mean per-chunk cost
(milliseconds):

| Stage | ms/chunk |
|---|---:|
| 2D noise (height/biome) | 0.266668 |
| Aquifer | 0.0150953 |
| Thermal erosion | 0.0184805 |
| Base cave 3D | 0.617023 |
| Spaghetti caves | 0.0443955 |
| Large caverns | 0.0240758 |
| Voxel fill | 0.482283 |
| Ore | 0.012122 |
| Cave decoration | 0.0200853 |
| Vegetation | 0.383468 |
| Border shell | 0.329275 |
| **Total** | **2.61567** |

`--world-stats 16 3`: 3 seeds, 16×16 chunks/seed, stride 31, 196608 columns.
Generation 1840.45 ms total / **2.39642 ms/chunk**. Height min/p50/p95/p99/max =
5 / 73 / 115 / 133 / 178. Ocean 21.9259%, rivers 3.87115%. Per-chunk
logs/foliage/ground-cover/kelp = 35.737 / 224.626 / 35.444 / 2.8125. Cave
air/water/lava cells = 1570942 / 445167 / 269380. Full biome coverage and ore
Y-bands were exercised (determinism + dense-vs-tiled border parity remain
covered by the `TerrainGeneration` suite).

## Persistence / streaming

```powershell
./build/tests/Release/bench_persistence.exe
```

Seed 42, 20 s per run, 200 edits/s. A = synchronous baseline, B = async
SaveService path (both measured on this tree):

| Metric (ms) | A baseline | B async |
|---|---:|---:|
| Frame avg | 3.521 | 3.627 |
| Frame p95 | 4.425 | 4.389 |
| Frame p99 | 5.101 | — |
| Streaming avg | 3.461 | 3.569 |
| Streaming p95 | 4.329 | 4.361 |

Save-worker: serialize avg 2.569 / max 3.166 ms; write avg 0.899 / max 2.662 ms;
3219 bytes per save. Explicit flush 115.8 ms, close 17.1 ms, shutdown 132.9 ms.

## Passive mobs (CPU)

```powershell
./build/tests/Release/test_chunk_lifecycle.exe --mobs-profile
```

Seed 42, center (0, −64), 48 mobs over real voxels:

| Metric | Value |
|---|---:|
| Mob update mean | 0.0372707 ms |
| Mob update p95 | 0.0544 ms |
| Cells sampled / tick | 1596 |
| Distance travelled | 2753.91 blocks |
| Dropped steps | 0 |
| Local light sampling mean | 0.00398439 ms |
| Local light sampling p95 | 0.0047 ms |
| Light sampling per mob | 0.0830081 µs |

## Player physics (CPU)

```powershell
./build/tests/Release/test_physics.exe --profile
./build/tests/Release/test_chunk_lifecycle.exe --physics-profile
```

Synthetic swept-collision profile, 12000 ticks: avg 0.000234167 ms,
p95 0.0004 ms, 41.215 cells/tick, 0 dropped steps.

Real-voxel profile (scoped adapter lock/cache setup + destruction included),
seed 42, 1200 warm-up + 12000 measured ticks:

| Metric | Value |
|---|---:|
| avg | 0.000680508 ms |
| p95 | 0.0011 ms |
| cells / tick | 39.9123 |
| waiting ticks | 0 |
| travelled | 682 blocks |
| dropped steps | 0 |

Both are far below the documented < 0.1 ms p95/tick target
([player-physics/](player-physics/README.md)). These measure the CPU path only,
not rendering.

## Visual regression

```powershell
ctest -C Release -R VisualRegression --output-on-failure
```

Offscreen 640×360 golden-image comparison, smoke tolerances
(`FT_VOX_VISUAL_SMOKE=1`, validation on). **20+ scenes PASS.** One scene,
`cave_emissive`, **FAILs** the reference comparison:

```
mean_abs=0.0419435 (max 0.0588235)
rms=0.0705545 (max 0.0980392)
max_abs=0.690196
hot_pixels=184524 ratio=0.800885 (max 0.1, threshold 20)
verdict=FAIL
```

**This failure is pre-existing and unrelated to Phase A.** Evidence:

- `git diff --name-only` for this change lists only `.gitignore`,
  `CMakeLists.txt`, `docs/benchmarks/README.md`, `tests/CMakeLists.txt` —
  **zero** `.cpp`/`.hpp` source changes.
- `ft_vox_visual_tests` compiles the core sources directly and links neither
  `ft_vox_core` nor any new header; its target definition is unchanged.
- The `cave_emissive.png` reference was committed `99c05d58` (2026-09-08,
  issue #141), over two months before the current driver
  32.0.16.2114 / Vulkan 1.4.357.
- The scene is self-deterministic (it fails only the golden comparison, not
  determinism) — a stable bloom/emissive driver-vs-golden shift.

Per the reference policy ([`../visual-regression.md`](../visual-regression.md)),
references are regenerated **only** explicitly via `--update-references` with
reviewable justification. Regenerating them is out of scope for Phase A and was
**not** done. The mismatch is recorded here transparently as the pre-refactor
visual baseline and tracked in
[#213](https://github.com/gkehren/ft_vox/issues/213) so it is not lost.

## CPU test suites (all PASS)

| Suite | Time |
|---|---:|
| CoreContracts (new, CPU-only) | 0.05 s |
| CoreIncludeGuard (new) | 0.14 s |
| PlayerPhysics | 0.03 s |
| StreamOptHelpers | 0.07 s |
| TerrainGeneration | 91.01 s |
| WorldPersistence | 2.73 s |
| PassiveMobs | 0.45 s |
| MobRealTerrain | 0.65 s |
| WorldSave | 0.20 s |

## Chunk lifecycle / mesh result tests

```powershell
ctest -C Release -R "ChunkLifecycle|MeshFaceKey|MeshUploadLifecycle|MeshResultPool|MeshArena|VoxelPool|BorderPool" --output-on-failure
```

These exercise the chunk/mesh coupling points (`ChunkManager`, `Chunk`,
publication/revision rules, pooled recycling) that phases B/C will split. All
PASS on this tree:

| Suite | Time |
|---|---:|
| ChunkLifecycle | 1.07 s |
| MeshFaceKey | 0.13 s |
| MeshUploadLifecycle | 1.85 s |
| MeshResultPool | 0.04 s |
| MeshArena | 0.61 s |
| VoxelPool | 0.03 s |
| BorderPool | 0.03 s |

## Feature matrix (pause / focus / reload / benchmark)

Behavioral reference for the features the Epic definition-of-done requires to
keep working. Phase A changes **no** input/UI/pause/reload/benchmark source
(zero `.cpp`/`.hpp` diff), so these are preserved by construction; the covering
suites confirm the underlying contracts still pass.

| Feature | Side | Automated coverage | Reproduce | Status |
|---|---|---|---|---|
| Pause | sim halt + render dt=0 | `AutoExposure` / `VisualRegression`: dt≤0 paused frames leave exposure strictly unchanged; `ProfilerWorkerConsistency`: reset-while-paused safe | `ctest -R "AutoExposure|ProfilerWorkerConsistency"` | PASS |
| Focus | input routing + window focus | `InputRouting`: keyboard-capture routing (global vs gameplay vs Escape, issue #76); `FramePacing`: VSync delta snapping/multiples/clamps | `ctest -R "InputRouting|FramePacing"` | PASS |
| Reload | world identity + persistence | `EngineReload`: `reloadWorld()` closes an open persistent world, save identity survives, stored seed adopted, second reload closes again | `ctest -R EngineReload` | PASS |
| Benchmark | derived profiling | `GpuProfile`, `WorkloadTelemetry`, `WorkloadTelemetryDisabled`, `ProfilerWorkerConsistency`; in-game `--benchmark` | `ctest -R "GpuProfile|WorkloadTelemetry|ProfilerWorkerConsistency"` | PASS |

Supporting UI suites also PASS: `UiShell` (0.03 s), `DebugUiCore` (0.06 s).
Feature-suite times: InputRouting 0.03 s, FramePacing 0.03 s, EngineReload
3.38 s, GpuProfile 0.02 s, WorkloadTelemetry 0.03 s, WorkloadTelemetryDisabled
0.01 s, ProfilerWorkerConsistency 0.09 s.

The in-game benchmark path is reproducible with:

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 30
```

**Coverage limitation (transparent):** the interactive pause/focus GUI
transitions (mouse capture, window focus loss/gain, mode toggles) are exercised
by the running game and the routing/pacing contracts above, not by a dedicated
automated GUI sequence. A full automated Windows input sequence for all
mode/focus transitions was not completed (the same limitation recorded in
[player-physics/](player-physics/README.md)). Because Phase A touches no
input/UI source, this does not affect the no-behavior-change guarantee.

## Conclusion

Phase A is a pure build extraction: the six core TUs are **source-identical**
(zero `.cpp`/`.hpp` diff), compiled once into `ft_vox_core` with the same
Release codegen (`/O2` on this MSVC build) and GLM defines as the game target.
All numbers in this document were measured **on this branch, after the
extraction**; because the measured units are source-identical to the
pre-refactor build, they serve as the pre-refactor baseline that later phases
compare against. (Cross-target bit-identity is not claimed — see
[`../simulation-render-contracts.md`](../simulation-render-contracts.md) §4.)
No source behavior changed, so these baselines are expected to hold across the
extraction. The only visual delta (`cave_emissive`) is a pre-existing
driver-vs-golden mismatch, documented above and tracked in
[#213](https://github.com/gkehren/ft_vox/issues/213); it was not introduced by
this phase. Later phases compare against these numbers to prove the
simulation/render split is behavior-neutral.
