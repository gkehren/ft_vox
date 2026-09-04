# Issue #88: Engine-scheduled biome map tiles

Measured 2026-09-04 on Windows, MSVC Release, NVIDIA GeForce RTX 4070 Ti.
Base revision: `6f1fbd8ac7a6`; measurements include this working-tree change.

## Implementation and priority contract

`TerrainGenerator::getBiomeRegion()` remains sequential. The new plan API
describes independent rectangles in the original sampling grid;
`getBiomeRegionTile()` samples one rectangle with the existing canonical
erosion/biome pipeline. Tile-local output never changes world-coordinate
rounding. Reverse-order and concurrent assembly match sequential output.

The Engine's `submitBiomeMap()` uses the existing ThreadPool, with at most
eight active tile lanes and Low priority for planning and every tile. Each
tile queues its successor separately, giving High/Normal chunk work another
scheduling opportunity. ThreadPool searches all queues at each priority
before considering the next priority. Work already executing is not
preempted. Task-count publication and sleeper notifications are synchronized
with queue visibility and the condition-variable wait transition.

A submission sentinel plus an atomic remaining-work count prevents early
publication. The last task converts the disjoint biome writes to RGBA and
resolves the result future; no pool worker waits for children. Exceptions
and cancellation drain submitted tasks and produce an invalid result.
GameUI keeps its existing single-flight and stale-result rules.

Maps spanning at most 128 blocks use one Low job with the UI-owned scratch.
Parallel sampling never shares that scratch: each visited worker retains
at most ~0.7 MiB of region scratch. No native threads are added. Retention
is per visited worker, not per lane, because tasks can migrate between workers.

## Standalone map latency

Run `build/tests/Release/test_biome_map.exe --map-perf`.
256x256 pixels, seed 1337, center (-123.6, 432.1), eight workers. Both paths
are warmed, then measured three times with alternating order. Values are
mean wall-clock latency in milliseconds; scheduled time includes queueing,
assembly, and RGBA conversion. Every measured output is checked byte-for-byte.

| Zoom | Sequential | Scheduled |
| --- | ---: | ---: |
| 0.1 | 617.361 | 84.2733 |
| 0.25 | 109.146 | 16.8534 |
| 0.5 | 28.3086 | 4.7153 |
| 1 | 8.25587 | 1.73337 |
| 2 | 3.22343 | 3.3601 |
| 4 | 1.92967 | 2.16207 |
| 8 | 1.85937 | 1.5362 |

Zoom 0.1 improves by about 7.3x. Small maps intentionally stay sequential;
their sub-millisecond variations are not evidence of a speedup.

## Streaming with the map open

Reproduce the control and scheduled runs:

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 15 --benchmark-map 0.1 --benchmark-map-sequential
./build/Release/ft_vox.exe --seed 42 --benchmark 15 --benchmark-map 0.1
```

Both run the same binary and scheduler. The control uses the previous
one-Low-job map algorithm, so this comparison isolates map scheduling rather
than comparing binaries with different instrumentation. The World panel is
open with a fixed map center and normal one-second refresh cadence; camera
movement does not cancel the map. Follow-player cancellation is covered by
tests, not this latency experiment.

Engine pool: 16 workers, map lanes capped at eight. Viewport 1920x1080,
view distance 512, IMMEDIATE presentation, VSync off, two-second warmup,
15-second measurement. No concurrent build or tests. Observed frame times
clustered near 5 ms with most time in Present; these runs do not establish
an uncapped rendering-throughput improvement.

| Metric | Sequential map | Scheduled map |
| --- | ---: | ---: |
| Completed maps | 9 | 14 |
| Mean map latency, including queueing | 664.308 ms | 93.0621 ms |
| TerrainGen mean execution | 1.57173 ms | 1.56174 ms |
| MeshBuild mean execution | 2.9515 ms | 2.94787 ms |
| Terrain task mean queue wait | 0.00999458 ms | 0.00920847 ms |
| Mesh task mean queue wait (full + LOD) | 0.00969865 ms | 0.00955753 ms |
| Mean Streaming CPU scope | 0.426466 ms | 0.414646 ms |
| Mean frame | 4.99149 ms | 5.00096 ms |
| TerrainGen completed tasks | 7200 | 6423 |
| MeshBuild completed tasks | 5399 | 4747 |

The map latency improves about 7.1x while measured mean chunk execution and
queue waits remain similar. Task counts and queue peaks differ between the
two runs; these single runs are not a throughput or tail-latency guarantee.
Queue metrics measure enqueue-to-worker-start, excluding the time a chunk
spends awaiting admission by the streaming budget. No queue percentiles or
allocator counters were collected.

Raw reports:

- [Sequential map](bench_20260904_101251_6f1fbd8ac7a6__s42_sc10000.txt)
- [Scheduled map](bench_20260904_101310_6f1fbd8ac7a6__s42_sc10000.txt)

## Validation

- Full Release build and all ten CTest suites, including Vulkan resource smoke.
- Terrain determinism and dense-vs-tiled border parity remain covered by
  `TerrainGeneration`.
- `BiomeMapGeneration`: reverse-order plan assembly, exact pixel coverage,
  fractional/negative coordinates, odd dimensions and partial edge tiles,
  extreme steps, scratch retention, invalid rectangles, late tile
  cancellation, one/four-worker parity, cancellation of queued work,
  exception completion, and pool shutdown draining.
- A gated two-worker regression verifies that queued High/Normal tasks in
  other queues precede local Low work.
- Existing world/seed/request rejection, single-flight refresh, scratch
  reuse, and upload-policy tests remain enabled.
- Linux/macOS and thread/address sanitizers were not run locally.
