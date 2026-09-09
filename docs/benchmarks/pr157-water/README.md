# PR #157: water surface rework (continuous fragment waves, single composition, depth write)

Release / MSVC, NVIDIA GeForce RTX 4070 Ti, Vulkan 1.4.351, 2026-09-09.

## Reproduction

Run from the repository root after `cmake --build build --config Release`:

```powershell
$env:FT_VOX_VALIDATION = '1'
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --out build/pr157-1080
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --audit-1440 --out build/pr157-1440
```

Both runs reported `water audit errors=0` (valid frames, no NaN/Inf, no new
validation errors).

The audit was additionally run at both resolutions with synchronization
validation active:

```powershell
$env:FT_VOX_VALIDATION = '1'
$env:FT_VOX_SYNC_VALIDATION = '1'
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --out build/pr157-sync
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --audit-1440 --out build/pr157-sync-1440
```

`FT_VOX_SYNC_VALIDATION=1` enables `VK_EXT_validation_features` on the
instance (added to the extension list only when requested and supported,
with a clear warning otherwise) and chains
`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` into the
Khronos layer. Both sync runs reported `water audit errors=0` — no hazards
and no new validation errors — with the explicit Water → Sky barrier in
place (the barrier was in turn validated by the run before the extension
plumbing was corrected, and re-validated after).

## Files

Raw CSVs were removed from the tree by the retention policy; permalinks to
the pre-retention revision:

- [1080.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/pr157-water/1080.csv) / [1440.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/pr157-water/1440.csv) — per-tier Water pass and full-frame GPU time plus
  the per-pass Shadow / Opaque / Sky / SSAO / Post intervals (median of three
  interleaved preset sweeps, 8 samples each after 4 warmup frames).
- [underwater-1080.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/pr157-water/underwater-1080.csv) / [underwater-1440.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/pr157-water/underwater-1440.csv) — Post + nested Composite
  intervals with the camera fully submerged (issue #144 methodology).

## Comparison against the pre-retention baseline

[issue139-water/1080.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/issue139-water/1080.csv) (main's water pipeline, same fixture/method):

| Tier | Water before | Water after | Frame before | Frame after |
|---|---|---|---|---|
| Low | 0.137 ms | 0.178 ms | 0.882 ms | 0.722 ms |
| Medium | 0.151 ms | 0.199 ms | 1.003 ms | 0.920 ms |
| High | 0.563 ms | 0.287 ms | 1.502 ms | 1.285 ms |
| Cinematic | 0.688 ms | 0.354 ms | 1.736 ms | 1.467 ms |

The budget for this rework was "at most +0.3 ms Water Cinematic at 1080p":
the pass is in fact ~2x faster at High/Cinematic (no vertex displacement, no
blending), while Low/Medium pay ~0.04-0.05 ms for the fragment wave field
they previously got "for free" from the vertex stage. Full-frame time is
lower at every tier.
