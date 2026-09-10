# Passive mobs — 48-mob baseline (PR #126)

First archived baseline for the passive-mob renderer/simulation, produced by the
automated tests (not the in-engine streaming benchmark, which keeps mobs
disabled for historical comparability). Re-run after renderer or AI changes to
track regressions; there are no hard thresholds yet.

## Protocol

- Windows, Release, MSVC 2022 x64, NVIDIA GeForce RTX 4070 Ti
- `test_mob_render` (offscreen, `FT_VOX_VALIDATION=1`): 48 mobs, all camera- and
  cascade-visible, 160 frames, stats from frames 40+.
  Raw report: [gpu-profile_48mobs_rtx4070ti.txt](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/mobs/gpu-profile_48mobs_rtx4070ti.txt) (pre-retention revision)
- `test_mobs --profile` (synthetic flat world): 48 mobs, 6000 fixed 60 Hz ticks.
  Raw output: [cpu-profile_48mobs.txt](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/mobs/cpu-profile_48mobs.txt) (pre-retention revision)

## Results

| Metric | Value | Notes |
|---|---|---|
| GPU Mobs + MobShadow0-2 | **0.019 ms** mean, 0.020 p95 | color + 3 cascades (profiler timestamps) |
| Command recording CPU | **2.9 ms** mean, 3.6 p95 | includes Vulkan **validation layers**; significantly lower without |
| Host submit + GPU wait | 3.3 ms mean | full offscreen round trip, not CPU sim time |
| Simulation CPU | **0.024 ms** mean, 0.032 p95 | per frame (≤8 fixed steps), 1800 voxel cells/tick |
| Simulation allocations | **0** per tick | global allocator hook, 6000 ticks |
| Validation errors | 0 | layers enabled |

Simulation cost scales with population (cap 48) and stays well under one
 millisecond per frame; the renderer record cost is dominated by per-part
`vkCmdDraw` submission (~860 draw calls worst case for 48 sheep) and is the
first candidate for batching if populations grow (see PR #126 follow-ups).

> **Update (issue #130):** the per-part submission path described above was
> replaced by instanced batch draws (one draw per populated static part batch
> per pass). See [issue-130-instancing.md](issue-130-instancing.md) for the
> fresh same-machine A/B.
