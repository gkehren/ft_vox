# Issue #144: camera-underwater medium transport

Release / MSVC, NVIDIA GeForce RTX 4070 Ti, Vulkan 1.4.351, 2026-09-08,
rebased on the auto-exposure pipeline (issue #140).

## Reproduction

Run from the repository root after `cmake --build build --config Release`:

```powershell
$env:FT_VOX_VALIDATION = '1'
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --out build/issue144-qa/1080
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --audit-1440 --out build/issue144-qa/1440
```

The underwater block of the audit pins a submerged camera below the lake
surface and measures, per quality tier, the whole `GpuPass::Post` interval and
the nested `GpuPass::Composite` interval that carries the underwater medium
transport (depth reconstruction, Beer-Lambert extinction/in-scatter, gated
caustics). Exposure is pinned to the deterministic manual path so the audit's
determinism probes stay valid; GPU cost is exposure-independent.

## GPU measurements

The raw CSVs were removed from HEAD by the benchmark retention policy.
Historical copies remain available at the pre-retention revision:
[1080.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/issue144-underwater/1080.csv) and [1440.csv](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/issue144-underwater/1440.csv).
They contain the median of three interleaved preset sweeps
(ascending / descending / ascending; 8 synchronous GPU samples per tier and
sweep after 4 warmup frames). Tier 0/1/2/3 means Low/Medium/High/Cinematic.
`post_ms` covers the whole Post scope (exposure metering, SSAO, bloom, god
rays, composite); `composite_ms` is the nested composite interval including
the underwater path. These are fixed-fixture costs, not a gameplay
frame-rate claim. Runs were taken with the GPU otherwise idle; an earlier
back-to-back attempt showed clock-ramp drift and was discarded.

| Composite (underwater) | Low | Medium | High | Cinematic |
|------|-----|--------|------|-----------|
| 1080p | 0.065 ms | 0.077 ms | 0.090 ms | 0.117 ms |
| 1440p | 0.159 ms | 0.137 ms | 0.148 ms | 0.167 ms |

The underwater path adds roughly 15-40 us over the pre-existing composite
work (depth fetches + medium transport); the Medium baseline stays a
lightweight fullscreen effect as required by the issue. No additional render
targets were added; composite gained one depth binding (set 1, binding 5),
the exposure history SSBO moved to binding 6, and the frame set is bound as
set 0.

## Validation

- Release build, GLSL-to-SPIR-V compilation, and all 30 ctest cases pass
  (including the issue #140 auto-exposure tests and goldens, unchanged).
- Both water audits pass with exit 0 and zero renderer validation errors.
- `--scene underwater_optics`: near-surface vs deep upward views differ only
  in depth below the surface — the near view must stay clearer (luma) and the
  deep view more blue-shifted; this locks the water-distance physics (the
  extinction path ends at the surface plane for upward rays).
- `--scene underwater_exposure`: underwater + auto exposure over 40 frames —
  finite output, adapted exposure bounded (1.35-2.30, no feedback runaway),
  the medium stays visibly in effect.
