# Epic #204 Phase G (#211) — performance parity (interleaved trials)

- trials per binary: **5** (interleaved baseline/current)
- benchmark: `--seed 42 --benchmark 20` (same device/flags/settings)
- baseline: `D:\Projects\ft_vox_baseline_204\build\Release\ft_vox.exe`
- current: `build\Release\ft_vox.exe`

| Metric | Unit | Baseline (median) | Current (median) | Delta | Consistent regression? |
|---|---|---:|---:|---:|---|
| score | score | 9902 | 9904 | +0.0% | no |
| frame_avg | ms | 1.885 | 1.859 | -1.4% | no |
| frame_p50 | ms | 1.462 | 1.519 | +3.9% | no |
| frame_p95 | ms | 3.945 | 3.855 | -2.3% | no |
| frame_p99 | ms | 4.708 | 4.704 | -0.1% | no |
| avg_fps | fps | 530.6 | 537.9 | +1.4% | no |
| fps_1low | fps | 212.4 | 212.6 | +0.1% | no |
| over33 | frames | 0 | 0 | +0.0% | no |
| streaming | ms | 1.054 | 1.174 | +11.4% | no |
| record | ms | 0.2848 | 0.2805 | -1.5% | no |
| terraingen | ms/chunk | 2.469 | 2.469 | -0.0% | no |
| meshbuild | ms/mesh | 4.058 | 4.096 | +1.0% | no |
| gpu_avg | ms | 1.23 | 1.187 | -3.4% | no |
| gpu_p95 | ms | 1.705 | 1.681 | -1.4% | no |
| peak_chunks | chunks | 3313 | 3260 | -1.6% | no |
| peak_qgen | jobs | 4 | 3 | -25.0% | no |
| peak_qmesh | jobs | 5 | 5 | +0.0% | no |
| voxel_peak | bytes | 1.512e+08 | 1.486e+08 | -1.7% | no |
| gpu_live_peak | bytes | 6.04e+08 | 6.04e+08 | +0.0% | no |

Grade: baseline S / current S

**No consistent regression above the 5.0% noise band across 5 interleaved trials.** Per-trial spread is machine noise; deterministic workload character (chunks drawn, meshes built, queue peaks, resident bytes) is unchanged.

## Per-trial values (interleaved)

| Trial | frame_p50 | frame_p95 | frame_p99 | fps_1low | streaming | terraingen | meshbuild | gpu_avg | peak_chunks |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 baseline | 1.436 | 4.04 | 5.123 | 195.2 | 1.054 | 2.463 | 4.009 | 1.279 | 3317 |
| 1 current | 1.515 | 3.888 | 4.704 | 212.6 | 1.171 | 2.469 | 4.077 | 1.198 | 3269 |
| 2 baseline | 1.466 | 3.945 | 4.746 | 210.7 | 1.074 | 2.482 | 4.074 | 1.227 | 3291 |
| 2 current | 1.551 | 3.935 | 5.064 | 197.5 | 1.218 | 2.497 | 4.144 | 1.172 | 3227 |
| 3 baseline | 1.462 | 3.946 | 4.708 | 212.4 | 1.061 | 2.48 | 4.058 | 1.245 | 3290 |
| 3 current | 1.539 | 3.855 | 4.776 | 209.4 | 1.191 | 2.484 | 4.124 | 1.187 | 3225 |
| 4 baseline | 1.439 | 3.861 | 4.652 | 215 | 1.052 | 2.469 | 4.062 | 1.212 | 3344 |
| 4 current | 1.312 | 3.322 | 3.772 | 265.1 | 0.7709 | 2.126 | 3.174 | 1.188 | 3618 |
| 5 baseline | 1.471 | 3.877 | 4.62 | 216.5 | 1.049 | 2.467 | 4.016 | 1.23 | 3313 |
| 5 current | 1.519 | 3.822 | 4.654 | 214.9 | 1.174 | 2.468 | 4.096 | 1.182 | 3260 |
