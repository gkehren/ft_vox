# Player physics validation — 2026-09-05

Branch: `feature/player-physics`. Reference: `main` at
`b9245450a4d3b3b5a8465ffa3420fd60fc833626`.
Windows, MSVC Release, Ryzen 7 9800X3D, GeForce RTX 4070 Ti.

## Correctness

- Release build succeeded. Full CTest results are in [ctest.txt](ctest.txt).
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
Latest output is in [physics-profile.txt](physics-profile.txt). It measures the
CPU path, not rendering or large populations of future entities. The target is
below 0.1 ms p95 per tick on this reference machine, not a CI timing assertion.

## Streaming comparison

Existing unmodified camera benchmark path, seed 42, 10 seconds measurement,
2 seconds warm-up, view distance 512, validation off and uncapped presentation.
Both binaries used the same runtime resource directory.

| Metric (ms) | Before | After |
|---|---:|---:|
| Frame mean | 1.42726 | 1.25096 |
| Frame p95 | 3.0893 | 2.6266 |
| Streaming mean | 0.426441 | 0.313651 |
| Record mean | 0.239421 | 0.199737 |
| GPU mean | 1.25192 | 1.08633 |

These are single-run observations, subject to asynchronous world-fill variation
and system load. They do not establish an optimization gain. No regression was
observed in this comparison. Physics is intentionally suspended during this
renderer benchmark; the separate CPU profile measures its added workload.
The subsequent surface-jump threshold/recovery tuning does not execute on the
scripted flight path.

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
