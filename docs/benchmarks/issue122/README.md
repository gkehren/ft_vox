# Issue #109 / #122 — cached per-chunk indirect draw descriptors (A/B)

Fix: `perf(renderer): cache per-chunk indirect draw descriptors`. Baseline:
`main` @ `d5e5ae9` (post-#123). Both binaries built Release from clean trees,
each run from its own worktree (branch `D:/Projects/ft_vox`, baseline
`D:/Projects/ft_vox_main108`). Reports are labeled `45270eb`: that is this
fix's code — the commit message was amended afterwards only (now `b2e7c1b`).

## Protocol

Per #122: interleaved 4-pair A/B, seed 42, 30 s, view 512, vsync off
(`M,P,M,P,M,P,M,P`, 2026-09-06 ~17:2x). Caveat: this session was the noisiest
observed so far — frame avg ~2.5-3.0 ms (vs ~2.0 in earlier sessions) and
Acquire 1.4-2.3 ms on BOTH sides, so absolute values are inflated; the
interleaved comparison stays valid.

## Results (4 pairs)

| Metric | main `d5e5ae9` | PR `45270eb` | Δ |
|---|---|---|---|
| Record avg (ms) | 0.4054 / 0.4073 / 0.4074 / 0.4077 → **0.4070** | 0.3538 / 0.3894 / 0.3796 / 0.3766 → **0.3749** | **−7.9 %** |
| Record p50 | ~0.389 | ~0.355 | −8.7 % |
| Record p95 | ~0.606 | ~0.563 | −7.1 % |
| Score | 9834-9887 (S) | 9820-9890 (S) | parity |
| GPU arena bytes | 671088640 | 671088640 | exact parity |

## Acceptance status (#122)

- "Record avg ≤ main d677d9d baseline (0.31 ms)": **not decidable in this
  session** — every scope is inflated by background load on both sides. The
  relative result (PR −8 % under the same load) is consistent with the fix
  doing real work; a quiet-session re-measure should re-judge the absolute
  criterion. #122 stays open for that.
- The commit message's preliminary claim (Record 0.36 → 0.18-0.20 ms) did
  **not** reproduce on the standard circular benchmark; the numbers above are
  the reference.

## Mechanism recap

Each pass previously walked every visible chunk's `SectionGpuSlot[16]` and
rebuilt `VkDrawIndexedIndirectCommand` descriptors — ×5 per frame
(OpaquePass + 3 shadow cascades + WaterPass). The cache is rebuilt only when
GPU-side truth changes: `uploadSectionSlots` commit, LOD commit (sync +
async), full↔LOD transition, and empty-stream fallback. `collect*Draws` are
now a single contiguous `insert`; move semantics and release paths zero the
cache; capacity matches `m_sectionGpu` (16) so the LOD entry (slot 0) and
full sections cannot overflow.
