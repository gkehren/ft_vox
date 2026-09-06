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

## Post-review revision (77576c4, anchor-driven reconciliation + counters)

Same protocol, fresh clean-tree builds of both sides, interleaved 2×2:

| Metric | main `1d62865` | PR `77576c4` |
|---|---|---|
| CPU `Streaming` avg | 0.355 / 0.348 ms | 0.161 / 0.174 ms (**−51/−54 %**) |
| Frame avg | 2.020 / 2.003 ms | 1.923 / 1.970 ms |
| Score | 9900 / 9901 (S) | 9914 / 9906 (S) |
| `chunks.active` peak | 4627 | 4677 |
| Maintenance split | n/a (rescanned every frame) | zeroWork 88.2-88.4 %, incremental ~1940, heading 0, full 1, queueSorts ~1941, unloadScans 512 |

Headless maintenance cost (`test_chunk_lifecycle --stream-perf`, view 512,
bias 0.3, empty loaded set): stationary frames ~0.011 us/call; forced
anchor crossings ~116 us/call with the queue kept full (pessimistic
sort-dominated bound — the engine's load budget drains the queue every
frame, shrinking the sort).

Honesty note: the branch shows 2 frames > 16.7 ms per run (run 1 max
71.6 ms, run 2 max 23.1 ms) where main showed none this session; score
and 1% low are unchanged and the count is identical across both branch
runs, but the spike is unexplained — flagged for follow-up if it
reproduces.

## Interpretation (pre-review measurements)

- Main-thread `Streaming` drops ~2.2× in the moving-camera benchmark even at
  the pre-review revision; the reviewed revision additionally removes the
  16-block footprint staleness (footprint now reconciles every 4 blocks of
  movement) at a bounded reconciliation rate (≤ 4 per axis per chunk).
- World-fill throughput, queue latency and draw counts are unchanged.
