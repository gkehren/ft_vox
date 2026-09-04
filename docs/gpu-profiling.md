# GPU timestamp profiling

F7 separates CPU frame/recording scopes, GPU frame/passes, worker CPU jobs and
the pipeline snapshot. `VkFrameContext` owns one timestamp query pool per
frame-in-flight slot. It resolves the previous submission immediately after
that slot's existing fence signals, before command-buffer reuse. Query results
use 64-bit values plus availability, never `VK_QUERY_RESULT_WAIT_BIT`.
Recording resets the pool on the GPU outside rendering. There is no added
per-frame fence wait, queue idle or device idle for profiling.

`VkGpuProfiler` requires the selected graphics queue family to expose
1–64 `timestampValidBits`, and a finite positive `timestampPeriod`.
`timestampComputeAndGraphics` is a blanket guarantee across graphics/compute
queues, not a requirement when the selected queue itself has valid timestamps.
Allocation failure or unsupported timestamps leave rendering functional. The
status distinguishes missing/invalid queue bits, invalid period, invalid queue
family, zero frame slots and pool allocation failure. Nanoseconds are
converted to milliseconds after unsigned counter subtraction and valid-bit
masking (including the 64-bit case and one counter wrap).

The frame interval encloses uploads, Shadow, Opaque, Overlays, Water, Sky, Post,
ImGui and the final present-layout transition. TOP_OF_PIPE/BOTTOM_OF_PIPE
timestamps measure elapsed graphics-queue intervals, including dependencies;
they are not shader-only utilization measurements. Pass intervals can overlap
and must not be summed. CPU `Record`, `Acquire` and `Present` remain CPU wall
times. GPU measurements neither include the CPU call to present nor display
scanout, though queue/semaphore backpressure can affect execution intervals.

The GPU checkbox is independent of CPU capture. `FT_VOX_GPU_PROFILING=0`
disables query reset/write/read work at startup; the checkbox can re-enable it.
Disabled pools retain their small allocation. Clear/reload and GPU toggles
clear the 240-sample history and discard pending results from old captures.

Benchmark reports and the report UI include availability, sample count, GPU
frame average, p95/p99 (at least 100 samples), and per-pass averages. Submission
tags exclude warmup and previous-run results; serial numbers avoid counting
the same delayed result twice. Finalization does not drain the GPU: the final
in-flight samples may be omitted, so GPU and CPU sample counts can differ.
Unavailable timings are explicitly labelled and never replaced with CPU data.

## Validation (Windows, RTX 4070 Ti, 2026-09-04)

Release build and all 11 CTest tests passed. `GpuProfile` exercises capability
gates, unit conversion, 8/64-bit wrap, duplicate exclusion, per-pass aggregation,
percentiles and capture isolation. `VulkanResourceSmoke` exercises two pools,
repeated reuse after a fence, unused query availability, capture reset and
disable/re-enable on a real device.

Five-second seed-42 runs at 1920×1080, zero warmup, validation enabled:

| Profiling / present | GPU samples | GPU avg ms | CPU Acquire ms | CPU Present ms |
| --- | ---: | ---: | ---: | ---: |
| Enabled / IMMEDIATE | 979 | 0.904 | 0.065 | 3.458 |
| Enabled / FIFO | 799 | 1.030 | 2.251 | 2.338 |
| Disabled / FIFO | 0 (unavailable) | unavailable | 4.492 | 0.147 |

IMMEDIATE GPU p95/p99 were 1.267/1.494 ms. Shadow averaged 0.050 ms,
Opaque 0.097 ms, Sky 0.394 ms and Post 0.269 ms. These short runs are functional
checks, not an overhead comparison or a performance claim.

No timestamp/query validation errors were observed. The controlled follow-up
comparison reproduces the same two VUIDs on base main
`098ceae278607d3330d1d9e9a797ec94d9a170a9`, with profiling code absent, and on
the corrected PR with profiling enabled and disabled:
`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275` concerning STORAGE usage on SRGB
swapchain images (8 and 6 occurrences respectively in each run).
Full application validation is **not clean**, but this is preexisting rather
than introduced by the timestamp profiler. The root cause remains unresolved
and is tracked in [#99](https://github.com/gkehren/ft_vox/issues/99).
See the [review validation report](benchmarks/issue98-validation/README.md)
for full logs, environment, tests and limits of the comparison.

Resolution/shadow-distance/overdraw sweeps and external RenderDoc/Nsight
comparison remain manual validation: use the Graphics/Streaming controls with
a fixed world/camera, compare the GPU pass history, and repeat with VSync on/off.
Unsupported-device behavior has unit coverage; no unsupported physical GPU was
available for an end-to-end run.

References: [Khronos timestamp sample](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html)
and [Vulkan queries specification](https://docs.vulkan.org/spec/latest/chapters/queries.html).
