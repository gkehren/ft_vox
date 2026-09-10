# Passive mobs — instanced batch submission (issue #130)

A/B for converting the mob path from one `vkCmdDraw` per visible mob part to one
instanced draw per populated static part batch per render pass (camera + 3
shadow cascades). Same machine, same build configuration, fresh current-HEAD
baseline captured immediately before the change (the PR #126 numbers in
[README.md](README.md) are historical context only).

## Protocol

- Windows, Release, MSVC (VS 18 2026 x64), NVIDIA GeForce RTX 4070 Ti
- Baseline: commit `5e772f2` (one draw per part instance). Post: this branch.
- `test_mob_render` offscreen harness: mixed 48-mob fixture (12 per species, 8×6
  grid, camera- and cascade-visible) and a 48-sheep worst-articulation fixture;
  160/80 frames respectively, stats from frame 40+/20+. Submission counters come
  from `MobRenderer::passStats` plus `vkCmdDraw` / `vkCmdBindDescriptorSets`
  hooks, not timing inference. The hook records every emitted command and the
  test verifies per-pass firstInstance continuity, per-batch geometry against
  `batchInfo()`, and instance sums; mutations (`instanceCount-1`,
  `firstInstance+1`, dropped last batch, wrong frame slot) all fail the suite —
  the dropped-batch mutation is caught only by the recorded-command proof.
- Primary metric: Release run **without** validation. Validation-enabled run
  reported separately (it magnifies per-command CPU cost).
- GPU numbers are **3 runs per side** (fresh binary per side); the table reports
  the per-run range.

## Submission invariants (measured, not inferred)

| Fixture | draws/pass (cam/s0/s1/s2) | instances/pass | vkCmdDraw per frame (4 passes) | before |
|---|---|---|---|---|
| Mixed 48 mobs | 42/42/42/42 | 504/504/504/504 | 168 | 2 016 |
| 48 sheep | 18/18/18/18 | 864/864/864/864 | 72 | 3 456 |

Per-pass batch instance sums equal the visible part-instance counts exactly;
every recorded draw has `1 ≤ instanceCount ≤ 48`; descriptor set-1 binds stay at
≤ 1 frame set + ≤ 3 texture runs per pass (sheep base/wool/undercoat).

## Results

| Metric | Before (5e772f2) | After | Δ |
|---|---|---|---|
| Command recording CPU, mixed 48, **no validation** | 0.043 ms mean, 0.057 p95 | **0.013 ms** mean, 0.030 p95 | ~3× lower |
| Command recording CPU, mixed 48, **validation on** | 2.75 ms mean, 3.04 p95 | **0.30 ms** mean | ~9× lower |
| Command recording CPU, 48 sheep, no validation | (same per-part path) | **0.012 ms** mean | — |
| Host submit + GPU wait, no validation | 0.173 ms | **0.142 ms** | −18% |
| Host submit + GPU wait, validation on | 3.00 ms | **0.48 ms** | −84% |
| `MobPrepare` CPU (new instrumentation) | not isolated | 0.029 ms mixed / 0.046 ms sheep mean | net host time still improved (rows above) |
| GPU Mobs + MobShadow0-2, mixed 48, 3 runs | 0.02506 / 0.02508 / 0.02519 ms mean; p95 0.0266 all runs | 0.02642 / 0.02644 / 0.02650 ms mean; p95 0.0276 all runs | **repeatable +1.4 µs mean (~+5% relative, beyond the ±0.13 µs run-to-run spread)** |
| Validation errors | 0 | 0 | both configs |

On the GPU delta, stated plainly: the relative change exceeds 5% and is
direction-consistent across all three runs and both mean and p95, so it is not
pure timestamp jitter — but it is only **+1.4 µs absolute on a 25 µs pass
(~0.01% of a 16 ms frame)**, and the change introduces no additional geometry,
fragment work or texture traffic (identical vertex/instance counts per pass are
asserted by the tests). The likely cause is different GPU scheduling for 42
multi-instance draws vs 504 single-instance draws. Accepted as the cost of the
~12× submission reduction; re-measure if mob population caps ever rise enough
to make the absolute delta material.

Draw calls now scale with the number of populated static part/material batches —
`O(populated static batches × passes)` instead of
`O(mobs × parts × passes)`. Raising `kMaxMobCount` no longer increases mob draw
count, because more instances land in the already-populated batches; adding new
baked parts or species still adds one batch (one draw per pass) each.

## Memory

Per-frame-in-flight instance buffer grew from 144 KiB (1536 instances) to 576 KiB
(6144 = 1536 × 4 pass slices); both frame slots total ~1.1 MiB. Budgets are
derived from the explicit `MobRenderer::kMaxPartsPerMob` contract (validated
against every loaded model in `init()`), and capacity throws deterministically
on populations past `kMaxMobCount` or a pass past `kMaxParts`.

## Environment notes

- `cave_emissive` visual-regression scene fails **identically at the baseline
  commit** on this machine (mean_abs=0.041943 rms=0.070555 hot=0.800881) —
  pre-existing reference/driver drift, not caused by the batching change; all
  15 other scenes including `mob_lighting` pass.
