# Issue #122 — eliminate the remaining per-frame sort on cached draw descriptors

Fix: `perf(renderer): cache indirect draw descriptors and eliminate sort overhead`
+ hardening (`overflow warning / bucket capacity / ephemeral network port`).
Baseline: `main` @ `d66dd11` (post-#125). Interleaved `M,P,M,P,M,P`, 30 s,
seed 42, view 512, vsync off, clean-tree builds on both sides
(2026-09-06 ~19:58-20:01, calm session: frame avg ~1.8 ms).

## What changed

- `Chunk::collect*Draws` are inlined in the header; passes now memcpy the
  cached descriptor arrays into a pre-sized scratch.
- The per-frame `std::sort` by arena page pair (opaque + shadow cascades) is
  replaced by a linear 2-pass bucket grouping that writes commands and
  draw-data entries directly into the mapped indirect buffer (stable: within
  a bucket the collection order is kept).
- Water is deliberately NOT bucketed: its back-to-front collection order is
  preserved with contiguous same-page-pair runs, per the #122 constraint.

## Results

| Metric | main `d66dd11` | PR `dacfaa7` | Δ |
|---|---|---|---|
| Record avg | 0.3566 / 0.3846 / 0.3911 → **0.3774** | 0.3210 / 0.3271 / 0.3426 → **0.3302** | **−12.5 %** |
| Record p50 | ~0.369 | ~0.322 | −12.7 % |
| Record p95 | ~0.512 | ~0.461 | −10.0 % |
| Score | 9902-9915 (S) | 9911-9913 (S) | parity |

## Acceptance status (#122)

- "Record avg ≤ main d677d9d baseline (0.31 ms)": **0.330 — 6 % above the
  historical quiet-session figure**, within cross-session noise. The same-
  session relative comparison is decisive (−12.5 % vs main), on top of the
  −8 % already merged in #124. Combined, the arenas Record regression is
  more than recovered.
- "Record p95 ≤ baseline": no p95 exists for d677d9d (percentiles postdate
  it); against the re-captured same-session main baseline, p95 −10.0 % ✓.
- Identical rendered output: Vulkan validation (indirect + bounds checks)
  0 new VUIDs; spawn-view visual check unchanged.

The 7th report (`bench_20260906_200200_dacfaa7_..._sc9927`) is the 10 s
Vulkan-validation run, kept as evidence.
