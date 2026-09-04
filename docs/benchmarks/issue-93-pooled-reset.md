# Issue #93: pooled acquisition reset

Measured on 2026-09-04, Windows, MSVC 19.50, Release, NVIDIA RTX 4070 Ti.
Baseline: `682138e5c977`; after: that revision plus the issue #93 changes.

`ChunkPool::acquire()` now uses `ResetMode::ForGeneration`, avoiding the
65,536-byte AIR fill. Default reset and pool release still clear voxels.
The generator's reused-storage reset contract is unchanged.

## Isolated reset cost

Run `build/tests/Release/test_chunk_lifecycle.exe --reset-perf`.
This compares the old full-reset behavior with the new acquisition behavior
in one binary, using one preallocated chunk, six alternating rounds of
100,000 resets per mode. Wall-clock measurements:

| Mode | Total for 600,000 resets | Per reset |
| --- | ---: | ---: |
| Full (previous acquisition behavior) | 236.815 ms | 0.395 us |
| ForGeneration | 43.3161 ms | 0.072 us |

This is a hot-buffer microbenchmark, not an end-to-end speedup estimate.
It excludes pool locking, generation, and release. No allocator counters were
instrumented; lifecycle tests verify stable voxel and shell capacities.

## Pure generation control

Three sequential runs of `test_terrain.exe --gen-perf 500 1337` per version:

| Version | Owning ms/chunk (runs 1/2/3) | Pooled-into ms/chunk (runs 1/2/3) |
| --- | --- | --- |
| Before | 1.23317 / 1.55271 / 1.19927 | 1.20954 / 1.29059 / 1.20200 |
| After | 1.26067 / 1.27865 / 1.36961 | 1.27222 / 1.29932 / 1.30979 |

All runs report stable reused-buffer capacities. This benchmark does not call
`Chunk::reset()` and therefore does not directly measure the optimization.
The variations do not establish an improvement or a regression in generation.

## Vulkan streaming

Command: `build/Release/ft_vox.exe --seed 42 --benchmark 30`.
1920x1080, view distance 512, VSync off, IMMEDIATE presentation, 2 s warmup.
No concurrent build or test during the baseline and final comparison runs.

| Metric | Before | After |
| --- | ---: | ---: |
| Measured wall time | 30.0006 s | 30.0019 s |
| Average frame | 1.80854 ms | 1.82516 ms |
| Streaming CPU scope | 0.374747 ms | 0.384825 ms |
| TerrainGen mean | 1.58569 ms | 1.64423 ms |
| TerrainGen jobs | 20,250 | 20,046 |
| TerrainGen aggregate | 32,110.3 ms | 32,960.2 ms |

The worker TerrainGen timer starts after acquisition, so it excludes the
removed fill. These single runs show no end-to-end gain; a sub-microsecond
acquisition saving is smaller than the observed run-to-run variation.
The report's `Acquire` scope measures swapchain acquisition, not ChunkPool.

Raw reports:

- Before: [baseline](bench_20260904_093904_682138e5c977_s42_sc9909.txt).
- After: [isolated comparison](bench_20260904_094249_682138e5c977__s42_sc9907.txt).
- [Intermediate after run](bench_20260904_094127_682138e5c977__s42_sc9907.txt)
  overlapped with test execution and is excluded from the comparison.

## Validation

Release builds of `ft_vox`, `test_terrain`, and `test_chunk_lifecycle` pass.
`ctest --test-dir build -C Release -R 'ChunkLifecycle|TerrainGeneration|StreamOptHelpers' --output-on-failure --timeout 120`
passes all three suites (41.11 s), including determinism, borders, reused
generation targets, dirty voxel regeneration, full retirement clearing,
actual pool reuse, cancelled acquisition, and stable storage capacities.
