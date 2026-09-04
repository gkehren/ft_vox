# Issue #101: fixed memory/workload reference

Captured on Windows with NVIDIA GeForce RTX 4070 Ti, Release/MSVC, Vulkan,
1920x1080, view distance 512, seed 42 and IMMEDIATE presentation. Validation
was disabled for the performance runs; GPU timestamp profiling stayed enabled.
The default orbit path and resource pack were used, with the biome map closed.

```powershell
$env:FT_VOX_VALIDATION = '0'
# Baseline: unmodified main at ed13e1dd6982
./build/Release/ft_vox.exe --seed 42 --benchmark 60 --benchmark-warmup 15 --vsync off
# Instrumented branch: run each setting in a separate process
$env:FT_VOX_TELEMETRY = '1' # repeat with '0'
./build/Release/ft_vox.exe --seed 42 --benchmark 60 --benchmark-warmup 15 --vsync off
```

The baseline was built after creating the branch, before changing source, so
its branch label is `feat/101-memory-workload-telemetry` but its clean revision
is exactly `main` at `ed13e1dd6982`. The instrumented reports retain that base
hash with the dirty marker and their actual build UTC. They must not be
mistaken for measurements of the unmodified commit.

- [Complete unmodified main baseline](main-baseline.txt)
- [Complete instrumented reference](telemetry-enabled.txt)
- [Complete disabled-instrumentation control](telemetry-disabled.txt)

| Run | Average frame ms | Average Record ms | Average MeshBuild ms/job | Average GPU ms |
| --- | ---: | ---: | ---: | ---: |
| Unmodified main | 1.60507 | 0.317011 | 2.32276 | 1.42449 |
| Instrumented branch, enabled | 1.82319 | 0.375431 | 2.83370 | 1.62070 |
| Same binary, disabled | 1.82874 | 0.374363 | 2.83077 | 1.63932 |

Enabled and disabled measurements are close in this pair (about 0.1% difference
in mean MeshBuild time). Both are slower than the earlier main run, including
GPU and terrain generation time. That difference cannot be attributed to
telemetry from these measurements; the pair is a control, not a statistically
controlled overhead benchmark.

Both main and instrumented runs reached 4,630 active chunks at peak and 1,569
draw-list entries. The instrumented run ended at 4,620 active chunks; end-of-run
stream logs showed approximately 4,620 active chunks with empty or nearly empty
load/generation/mesh queues. This captures sustained streaming after 15 seconds
of warmup, rather than a five-second startup-only profile. The orbit continues
to replace chunks, so it is not a static-camera or zero-allocation workload.

## Memory/workload reference with telemetry enabled

| Quantity | Observed value |
| --- | ---: |
| Pool capacity / peak acquired / peak deferred chunks | 8,894 / 4,643 / 24 |
| Resident voxel capacity | 555.875 MiB |
| Retained shell capacity | 709.026 MiB |
| Peak combined retained CPU mesh capacity | 1,300.041 MiB |
| Peak live GPU mesh bytes | 598.625 MiB |
| Peak retired GPU mesh bytes / buffers | 4.214 MiB / 96 |
| Mesh buffer creations / destructions in measurement | 92,898 / 91,272 |
| Successful asynchronous chunk uploads | 32,192 |
| Staging slice peak / failures / deferred uploads | 0.784 MiB / 0 / 0 |
| Average opaque / water / total shadow draws per frame | 1,488.39 / 867.661 / 651.724 |

The mesh stage totals were skylight 14,922.6 ms, block light 3,171.44 ms,
occupancy 569.93 ms and the fused face/greedy/AO/output loop 23,087.9 ms,
over 14,737 completed full mesh jobs. LOD accounted for 17,454 jobs and
576.679 ms. These are summed worker time, not elapsed wall time.

The full report includes each individual CPU vector size/capacity, GPU mesh
class, cascade draw count and output count. See
[telemetry semantics](../../workload-telemetry.md) before comparing results:
ownership byte counts are neither process RSS nor VMA physical residency,
and capacity peaks are measured at publication boundaries.

## Validation

The final Release build passed. [CTest passed 14/14 tests in 42.08 seconds](ctest.txt).
[Report consistency checks](report-checks.txt) passed for all 27 gauge peaks,
CPU/GPU aggregate sums, vector size/capacity relationships, pool balance,
shadow cascade totals, disabled output and fixed benchmark settings. Tests
cover worker concurrency and stale capture epochs, repeated resets, disabled
instrumentation, real chunk moves/retained storage, real Vulkan allocation
retirement and staging exhaustion. The existing CPU/GPU profiler tests remain
enabled. Vulkan's negative diagnostic test intentionally reports an error and
must exit unsuccessfully; CTest recognizes that expected result.

These are individual development-machine runs, not an overhead confidence
interval. No claim of negligible overhead or renderer speedup is made from
them. Use `FT_VOX_TELEMETRY=0` when counters are not needed. Future optimization
comparisons should retain the same path, duration, warmup, resource pack,
viewport, view distance, VSync and instrumentation settings.

Linux and macOS were not exercised in this validation session.
