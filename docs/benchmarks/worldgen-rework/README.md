# Worldgen rework A/B benchmarks (#119)

Interleaved A/B comparison of the world generation rework against the
current `main` (the shared-arena renderer #121 is merged in both sides).
Reports are named `bench_<UTC>_<sha>_<seed>_sc<score>.txt`; a single
underscore before the seed marks a clean tree at build time.

- Baseline `main`: `f04495a42f84` (shared mesh arenas + indirect drawing,
  post-#121)
- PR worldgen rework: `519916a20c12` (branch rebased on that `main`;
  the renderer is #121's, #119 adds worldgen + alpha-cut shadow
  descriptors on top)
- Machine: Windows, NVIDIA GeForce RTX 4070 Ti, 1920x1080, VSync off, view
  distance 512, bundled resource pack, validation off.
- Protocol: for each seed (42, 1337, 2026) three interleaved `main, PR`
  pairs, 30 s measurement + 3 s warmup per run, Release builds from their
  own clean trees (each binary loads its own shaders). Medians of the
  three runs below; the raw reports (`20260906_0255`-`0306`) were removed
  from HEAD by the benchmark retention policy and live only in
  [Git history](https://github.com/gkehren/ft_vox/tree/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/worldgen-rework).

| Metric (median of 3)      | s42 main→PR | s1337 main→PR | s2026 main→PR |
|---------------------------|-------------|---------------|---------------|
| Score                     | 9919→9917   | 9922→9921     | 9919→9912     |
| Frame avg (ms)            | 1.64→1.82   | 1.62→1.82     | 1.68→1.85     |
| Frame p95 (ms)            | 3.49→3.86   | 3.52→3.68     | 3.60→3.83     |
| Frame p99 (ms)            | 3.83→4.07   | 3.69→3.88     | 3.86→4.20     |
| GPU avg (ms)              | 1.46→1.63   | 1.44→1.62     | 1.49→1.65     |
| GPU p95 (ms)              | 1.81→2.00   | 1.83→1.89     | 1.86→2.00     |
| Streaming CPU (ms)        | 0.26→0.26   | 0.25→0.26     | 0.26→0.27     |
| Record CPU avg (ms)       | 0.35→0.37   | 0.35→0.39     | 0.35→0.40     |
| Record CPU p95 (ms)       | 0.45→0.50   | 0.45→0.52     | 0.46→0.55     |
| TerrainGen worker (ms)    | 1.28→2.12   | 1.30→2.17     | 1.28→2.26     |
| MeshBuild worker (ms)     | 1.85→2.38   | 1.86→2.44     | 1.87→2.58     |
| indirect.commands.peak    | 14009→14746 | 14354→16474   | 14659→15889   |
| arena pages peak          | 8→10        | 8→9           | 9→11          |
| arena high-water (MB)     | –→730       | –→689         | –→765         |

Reading: the richer world raises TerrainGen worker cost by ~65-76% and
MeshBuild by ~28-38% (mature trees, ground props, cross/flat detail
quads), which shows up as ~0.1-0.2 ms on frame avg/p95 and ~0.1-0.2 ms on
GPU avg. Record grows by +4-19% (~0.02-0.05 ms) because the vegetation
adds live sections, i.e. more indirect commands per frame
(+5-15%). The benchmark score stays grade S on every seed (all deltas
within 1-2 points of the baseline on a 0-10000 scale); streaming CPU is
unchanged. The shared arenas absorb the extra geometry with single-digit
page counts (peak 9-11 pages, ~0.7 GB high-water) — no per-chunk
allocation churn returns with the denser world.

## Historical A/B (pre-rebase, merge-base `d677d9d9cc71`)

The first A/B of this PR (before the #121 rebase, at `12085db668fa`; raw
reports retained in [Git history](https://github.com/gkehren/ft_vox/tree/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/worldgen-rework), not in HEAD) showed the same shape: TerrainGen +60-75%,
MeshBuild +20-25%, frame p95 +0.1-0.2 ms, GPU p95 +0.06-0.11 ms,
Streaming/Record CPU unchanged, score grade S on every seed. That
comparison's GPU-memory column is superseded by the arena gauges above.
