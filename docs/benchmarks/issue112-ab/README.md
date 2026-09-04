# Interleaved A/B reference: base da00e51 vs PR 2b92493 (issue #102)

Three interleaved 60s runs per side (15s warmup, seed 42, telemetry on,
validation off, IMMEDIATE present), same binary pair.

**Timing caveat (machine under external load):** the GPU and CPU were
also being used by other programs while these runs were captured. The
frame-time deltas below (avg/p95/p99, MeshBuild, TerrainGen) must NOT be
read as reliable performance conclusions - the load is uncontrolled and
not symmetric between runs. Re-run on an idle machine before drawing any
timing conclusion.

What stays valid under external load:

- Memory metrics are structural counters, not timings: `voxel.bytes`
  peak 555.88 -> 290.69 MiB (-47.7%), retained pool high-water
  `voxel.pool.capacityBytes` ~290.7 MiB (4,651 blocks),
  `voxel.pool.growEvents` 68-86 per measurement window.
- Functional invariants: zero pool rejects, zero staging failures,
  ownership counts balanced. VoxelPool balance holds on every PR run's
  final sample: `voxel.pool.capacity == voxel.pool.active +
  voxel.pool.free` (4647 = 4619 + 28, 4651 = 4619 + 32,
  4652 = 4619 + 33). Peaks are intentionally not summed the same way -
  the three peaks may have been reached on different frames.

| Metric (medians) | Base da00e51 | PR 2b92493 | Delta |
| --- | ---: | ---: | ---: |
| voxel.bytes peak | 555.88 MiB | 290.69 MiB | -47.7% (structural) |
| voxel.pool.capacityBytes | n/a | 290.69 MiB | new |
| voxel.pool.capacity / active peak | n/a | 4,651 blocks | new |
| voxel.pool.growEvents | n/a | 68-86 | new |
| Frame avg / p95 / p99 | 2.803 / 6.267 / 8.171 ms | 2.525 / 5.439 / 5.991 ms | unreliable (load) |
| TerrainGen avg/job | 1.537 ms | 1.565 ms | +1.8% (unreliable) |
| Streaming avg | 0.380 ms | 0.382 ms | +0.5% (unreliable) |
| MeshBuild avg/job | 2.782 ms | 2.953 ms | +6.1% (unreliable) |
| Pool rejects / staging failures | 0 / 0 | 0 / 0 | = |
