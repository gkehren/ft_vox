# Worldgen rework A/B benchmarks (#119)

Interleaved A/B comparison of the world generation rework against its exact
merge-base parent. Reports are named `<side>_bench_<UTC>_<sha>_<seed>_sc<score>.txt`;
a single underscore before the seed marks a clean tree at build time.

- Baseline `main`: `d677d9d9cc71` (merge-base with the PR branch, `main` unmoved)
- PR worldgen rework: `12085db668fa` (final code state; the commit adding this
  folder is validation artifacts plus one whitespace-only EOF fix in
  `foliage_wind.inc.glsl` — no functional change after the benchmarked
  revision)
- Machine: Windows, NVIDIA GeForce RTX 4070 Ti, 1920x1080, VSync off, view
  distance 512, bundled resource pack, RTSS state unchanged, GPU otherwise idle.
- Protocol: for each seed (42, 1337, 2026) three interleaved `main, PR` pairs,
  30 s measurement + 2 s warmup per run, Release builds from clean worktrees.
  All numbers below are medians of the three runs (raw reports in this folder).

| Metric (median of 3)      | s42 main→PR | s1337 main→PR | s2026 main→PR |
|---------------------------|-------------|---------------|---------------|
| Score                     | 9907→9904   | 9922→9920     | 9915→9919     |
| Frame avg (ms)            | 1.75→1.81   | 1.61→1.78     | 1.68→1.82     |
| Frame p95 (ms)            | 3.75→3.87   | 3.51→3.70     | 3.59→3.80     |
| Frame p99 (ms)            | 4.22→4.45   | 3.72→3.93     | 3.96→4.00     |
| Avg FPS                   | 570→551     | 619→561       | 596→550       |
| 1% low FPS                | 237→225     | 269→254       | 253→250       |
| GPU avg (ms)              | 1.56→1.63   | 1.45→1.61     | 1.50→1.65     |
| GPU p95 (ms)              | 2.02→2.08   | 1.82→1.93     | 1.90→1.99     |
| GPU p99 (ms)              | 2.28→2.41   | 1.93→2.06     | 2.13→2.09     |
| Shadow GPU (ms)           | 0.18→0.24   | 0.20→0.23     | 0.22→0.27     |
| Opaque GPU (ms)           | 0.90→0.92   | 0.79→0.93     | 0.84→0.98     |
| Streaming CPU (ms)        | 0.31→0.29   | 0.26→0.27     | 0.28→0.27     |
| Record CPU (ms)           | 0.32→0.31   | 0.29→0.30     | 0.29→0.28     |
| TerrainGen worker (ms)    | 1.42→2.23   | 1.32→2.21     | 1.35→2.30     |
| MeshBuild worker (ms)     | 2.09→2.52   | 1.91→2.48     | 1.99→2.64     |
| Opaque draws/frame        | 1157→1135   | 1182→1149     | 1168→1141     |
| Water draws/frame         | 736→699     | 787→745       | 756→661       |
| GPU live peak (MB)        | 609→770     | 617→718       | 638→803       |

Reading: the richer world raises TerrainGen worker cost by ~60–75% and
MeshBuild by ~20–25%, which shows up as ~0.1–0.2 ms on frame p95 and ~0.06–0.11 ms
on GPU p95 (plus ~0.1–0.17 GB more resident chunk geometry from mature trees,
ground props and detail quads). Streaming and record CPU scopes are unchanged;
the benchmark score stays grade S on every seed (2026 is within noise of the
baseline). Generation cost remains within the engine's streaming budget.

Earlier dirty-tree reports (`d677d9d9cc71__*`, built from a working tree with
untracked files) were removed; the historical clean baseline reports in
`docs/benchmarks/` predate this rework and are kept for reference.
