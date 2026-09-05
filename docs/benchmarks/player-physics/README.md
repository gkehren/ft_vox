# Player physics validation — 2026-09-05

Branch: `feature/player-physics`. Reference: `main` at
`64d56b3211206f902b1c6cc82e2a0c47b163dff4` (the merged #117 section-meshing
work) — the A/B comparison uses the exact final PR parent (`64d56b3`) and the
final PR HEAD (`2f1bde955a58594e37a3da3d09abdbfa24fb644a`).
Windows, MSVC Release, Ryzen 7 9800X3D, GeForce RTX 4070 Ti.

## Correctness

- Release build succeeded. Full CTest results are in [ctest.txt](ctest.txt).
  Full CTest suite: 20/20 passed (the 19 suites carried by `main` plus the new
  PlayerPhysics suite).
- CPU tests cover swept collision, simultaneous contacts, coplanar seams,
  high-speed falling, ceiling hits, different body sizes, bounded recovery,
  sprint/braking/diagonal normalization, buffered/coyote jumps and liquid motion.
- Naturally reached swimming equilibrium supports a fresh surface-jump press.
- Movement and timed jumps agree at 30/60/144/240 render FPS; long-frame catch-up
  is bounded. The simulation allocation probe observes zero ordinary heap
  allocations over 2400 ticks.
- Chunk tests cover unpublished backing, concurrent terrain generation,
  pre-mesh availability, read-only remeshing, negative coordinates, deferred
  placement/deletion and stale edits after recycling.
- The running scene and player HUD were inspected. Automated Windows input was
  stopped when the tool detected concurrent user input; a full automated GUI
  sequence for all mode/focus transitions was not completed.

## CPU physics profile

Commands, from the repository root:

```powershell
./build/tests/Release/test_physics.exe --profile
./build/tests/Release/test_chunk_lifecycle.exe --physics-profile
```

The real-voxel benchmark includes the scoped adapter's lock/cache setup and
destruction. It uses seed 42, 1200 warm-up ticks and 12000 measured ticks, with
repeated walking/sprinting/jumping commands and bounded periodic resets.
Latest output is in [physics-profile.txt](physics-profile.txt), rerun on the
final rebased tree. It measures the CPU path, not rendering or large
populations of future entities. The target is below 0.1 ms p95 per tick on
this reference machine, not a CI timing assertion.

## Streaming comparison

Existing unmodified camera benchmark path, seed 42, 10 seconds measurement,
2 seconds warm-up, view distance 512, validation off and uncapped presentation.
Both binaries used the same runtime resource directory. Each column is the
median of three interleaved runs (`main`, PR, `main`, PR, `main`, PR) against
the exact final PR parent.

| Metric (ms) | `main` 64d56b3 (median) | PR 2f1bde9 (median) | Delta |
|---|---:|---:|---:|
| Frame mean | 1.2387 | 1.2381 | -0.05% |
| Frame p95 | 2.6211 | 2.6239 | +0.11% |
| Streaming mean | 0.3102 | 0.3090 | -0.38% |
| Record mean | 0.2036 | 0.2031 | -0.25% |
| GPU mean | 1.0871 | 1.0874 | +0.02% |

No regression was observed: every metric sits within measurement noise
(|delta| <= 0.4%). Physics is intentionally suspended during this renderer
benchmark, so any small improvement must not be attributed to #118; the
separate CPU profile measures its added workload. The subsequent
surface-jump threshold/recovery tuning does not execute on the scripted
flight path.

## Vulkan validation limitation

The game with validation enabled reported
`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275`. Both also reproduced on a separately
built archive of the reference `main` commit, using the same runtime resources
and `--seed 42 --benchmark 5 --benchmark-warmup 0`.

The engine additionally reported loaded RTSS hooks. This matches the existing
[SRGB/STORAGE overlay diagnosis](../../vulkan-validation.md). No renderer,
validation filtering, overlay settings or open GitHub issues were changed.
The game-validation run therefore cannot be described as clean, despite passing
CTest resource/error-reporting checks.
