# Issue #101: fixed memory/workload reference

Captured on Windows with NVIDIA GeForce RTX 4070 Ti, Release/MSVC, Vulkan,
1920x1080, view distance 512, seed 42 and IMMEDIATE presentation. Validation
was disabled for the performance runs; GPU timestamp profiling stayed enabled.
The default orbit path and resource pack were used, with the biome map closed.

```powershell
$env:FT_VOX_VALIDATION = '0'
# Legacy baseline: unmodified main at ed13e1dd6982
./build/Release/ft_vox.exe --seed 42 --benchmark 60 --benchmark-warmup 15 --vsync off
# Instrumented reference: run each setting in a separate process
$env:FT_VOX_TELEMETRY = '1' # repeat with '0'
./build/Release/ft_vox.exe --seed 42 --benchmark 60 --benchmark-warmup 15 --vsync off
```

## Reference lineage

| Role | Revision | Reports |
| --- | --- | --- |
| Legacy performance comparison (unmodified main) | `ed13e1dd6982` | [main-baseline.txt](main-baseline.txt) |
| **Optimization telemetry reference (current)** | `f95a5d9` clean HEAD | [telemetry-enabled.txt](telemetry-enabled.txt) / [telemetry-disabled.txt](telemetry-disabled.txt) |

The legacy baseline was built after creating the branch, before changing
source, so its branch label is `feat/101-memory-workload-telemetry` but its
clean revision is exactly `main` at `ed13e1dd6982`. It keeps the dirty
instrumented-build marker and is retained for historical comparison only.

The current reference pair was rebuilt and rerun from the committed clean
`f95a5d9` HEAD (no dirty marker) after the capture-local gauge fix, using the
same binary for both telemetry settings. Future optimization issues
(#102+) should compare against this pair, because they will read the metrics
this instrumentation publishes.

| Run | Average frame ms | Average Record ms | Average MeshBuild ms/job |
| --- | ---: | ---: | ---: |
| Unmodified main (`ed13e1d`) | 1.60507 | 0.317011 | 2.32276 |
| `f95a5d9`, telemetry enabled | 1.59726 | 0.311557 | 2.27726 |
| Same binary, telemetry disabled | 1.59867 | 0.311360 | 2.28611 |

Enabled and disabled measurements are close in this pair (about 0.4%
difference in mean MeshBuild time). The instrumented pair is not slower than
the legacy baseline here, but that comparison spans different commits and is
not a controlled telemetry-overhead measurement; the enabled/disabled pair is
the control.

The runs reached 4,635 peak acquired chunks (`chunks.active` peak 4,630, an
end-of-frame sample) with empty or nearly empty load/generation/mesh queues at
the end. This captures sustained streaming after 15 seconds of warmup, rather
than a five-second startup-only profile. The orbit continues to replace
chunks, so it is not a static-camera or zero-allocation workload.

## Memory/workload reference with telemetry enabled

| Quantity | Observed value |
| --- | ---: |
| Pool capacity / peak acquired / peak deferred chunks | 8,894 / 4,635 / 16 |
| Resident voxel capacity | 555.875 MiB |
| Retained shell capacity | 709.026 MiB |
| Peak combined retained CPU mesh capacity | 1,301.376 MiB |
| Peak live GPU mesh bytes | 598.738 MiB |
| Peak retired GPU mesh bytes / buffers | 2.637 MiB / 66 |
| Mesh buffer creations / destructions in measurement | 92,272 / 91,306 |
| Successful asynchronous chunk uploads | 31,938 |
| Staging slice peak / failures / deferred uploads | 0.684 MiB / 0 / 0 |
| Average opaque / water / total shadow draws per frame | 1,527.20 / 881.21 / 649.84 |

Staging and chunk-manager gauges are capture-local (see below): these peaks
describe measured frames only, so the warmup staging high-water mark no longer
inflates them.

The mesh stage totals were skylight 11,549.5 ms, block light 2,420.56 ms,
occupancy 447.472 ms and the fused face/greedy/AO/output loop 19,129.3 ms,
over 14,735 completed full mesh jobs. LOD accounted for 17,203 jobs and
452.26 ms. These are summed worker time, not elapsed wall time; mesh timing
stops before telemetry ownership publication.

The full report includes each individual CPU vector size/capacity, GPU mesh
class, cascade draw count and output count. See
[telemetry semantics](../../workload-telemetry.md) before comparing results:
ownership byte counts are neither process RSS nor VMA physical residency,
and capacity peaks are measured at publication boundaries.

## Validation

The final Release build passed. [CTest passed 14/14 tests in 41.84 seconds](ctest.txt).
[Report consistency checks](report-checks.txt) passed for all 27 gauge peaks,
CPU/GPU aggregate sums, pool balance, shadow cascade totals and the
capture-local reset property. Tests cover worker concurrency and stale capture
epochs, repeated resets, capture-local gauge exclusion (Registry unit test and
a real StagingRing warmup-to-measurement passage under Vulkan), disabled
instrumentation, real chunk moves/retained storage and real Vulkan allocation
retirement. The existing CPU/GPU profiler tests remain enabled. Vulkan's
negative diagnostic test intentionally reports an error and must exit
unsuccessfully; CTest recognizes that expected result.

These are individual development-machine runs, not an overhead confidence
interval. No claim of negligible overhead or renderer speedup is made from
them. Use `FT_VOX_TELEMETRY=0` when counters are not needed. Future optimization
comparisons should retain the same path, duration, warmup, resource pack,
viewport, view distance, VSync and instrumentation settings.

Linux and macOS were not exercised in this validation session.
