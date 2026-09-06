# Issue #108 — incremental streaming candidate maintenance (A/B)

Fix: incremental candidate maintenance, reworked after review (anchor-driven
intra-chunk reconciliation, centralized invalidation, dispatch counters).
Baseline: `main` @ `1d62865` (post-#119). Both binaries built Release from
clean trees, each run from its own worktree (branch `D:/Projects/ft_vox`,
baseline `D:/Projects/ft_vox_main108`). Raw reports of both sides are archived
here (`*_1d62865*` = main, `*_f2c14a8*` = first PR revision; the post-review
revision appends its own files).

## Protocol

- `ft_vox --seed 42 --benchmark 30` (wide streaming benchmark, exits after report)
- Default benchmark settings: `maxRenderDistance` 512, `streamFrontBias` 0.30
- Interleaved A/B/A/B, 2 runs per side (2026-09-06, same machine, same session)
- The circular benchmark camera moves every frame; at the pre-review revision
  this exercised chunk-cross/heading reconciliation almost every frame. With
  the 4-block anchor of the reviewed revision it mixes zero-work sub-anchor
  frames with periodic anchor/chunk crossings and occasional heading
  reconciliations — i.e. it measures a realistic moving-camera average, not
  the worst-case per-reconciliation cost. The `Stream maintenance:` line in
  each report states the actual frame split.

## Results (pre-review revision f2c14a8, superseded by the post-review A/B below)

| Metric | main `1d62865` | PR `f2c14a8` | Δ |
|---|---|---|---|
| CPU `Streaming` avg (run 1) | 0.359 ms | 0.151 ms | **−58 %** |
| CPU `Streaming` avg (run 2) | 0.366 ms | 0.181 ms | **−51 %** |
| Frame avg | 1.999 / 2.005 ms | 1.901 / 1.997 ms | ≈ noise |
| Score | 9901 / 9902 (S) | 9913 / 9908 (S) | — |
| `chunks.active` peak | 4628 / 4628 | 4629 / 4627 | fill parity |
| TerrainGen n / avgMs | 18995 / 2.545 | 19056 / 2.550 | parity |
| MeshQueue avgMs | 0.0100 | 0.0101 | parity |

## Post-review revision

See the `bench_*` files and the `Stream maintenance:` line added by the
review telemetry; the PR description carries the final table (steady-state,
forced-incremental and end-to-end regimes).

## Interpretation

- Main-thread `Streaming` drops ~2.2× in the moving-camera benchmark even at
  the pre-review revision; the reviewed revision additionally removes the
  16-block footprint staleness (footprint now reconciles every 4 blocks of
  movement) at a bounded reconciliation rate (≤ 4 per axis per chunk).
- World-fill throughput, queue latency and draw counts are unchanged.
