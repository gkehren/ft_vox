# Issue #108 — incremental streaming candidate maintenance (A/B)

Fix: `perf(chunk): incremental candidate maintenance in streaming loop (#108)`.
Baseline: `main` @ `1d62865` (post-#119). Both binaries built Release, each run
from its own tree (branch worktree `D:/Projects/ft_vox`, baseline worktree
`D:/Projects/ft_vox_main108`).

## Protocol

- `ft_vox --seed 42 --benchmark 30` (wide streaming benchmark, exits after report)
- Default benchmark settings: `maxRenderDistance` 512, `streamFrontBias` 0.30
- Interleaved A/B/A/B, 2 runs per side (2026-09-06, same machine, same session)
- The benchmark camera is in **constant motion** — this is the *worst case* for
  the incremental design (every frame is a chunk-cross or heading change) and it
  never exercises the zero-work stationary path.

## Results

| Metric | main `1d62865` | branch `f2c14a8` | Δ |
|---|---|---|---|
| CPU `Streaming` avg (run 1) | 0.359 ms | 0.151 ms | **−58 %** |
| CPU `Streaming` avg (run 2) | 0.366 ms | 0.181 ms | **−51 %** |
| Frame avg | 1.999 / 2.005 ms | 1.901 / 1.997 ms | ≈ noise |
| Score | 9901 / 9902 (S) | 9913 / 9908 (S) | — |
| `chunks.active` peak | 4628 / 4628 | 4629 / 4627 | fill parity |
| TerrainGen n / avgMs | 18995 / 2.545 | 19056 / 2.550 | parity |
| MeshQueue avgMs | 0.0100 | 0.0101 | parity |
| indirect.commands.peak | 14445 | 14513 | parity |

## Interpretation

- Main-thread `Streaming` drops ~2.2× even in the all-motion benchmark; frames
  where the camera stays inside one chunk with unchanged settings now do **zero**
  candidate work (no scan, no prune, no sort, no queue rebuild) — that regime is
  not represented in the benchmark average, real gameplay benefits more.
- The issue's stretch target (`< 0.1 ms average` at view 512) is not reached
  under constant motion; the remaining cost is the per-chunk-cross footprint
  diff + queue refresh + single re-sort. World-fill throughput, queue latency
  and draw counts are unchanged.
