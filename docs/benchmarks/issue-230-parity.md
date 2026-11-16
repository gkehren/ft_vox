# Epic #223 closure (#230) — performance parity vs the pre-epic baseline (interleaved trials, #211 protocol driver)

- trials per binary: **3** (interleaved baseline/current)
- benchmark: `--seed 42 --benchmark 20` (same device/flags/settings)
- baseline: `ft_vox.exe` (git `81cb00e2087c`)
- current: `ft_vox.exe` (git `66bfe38b0c14`)

| Metric | Unit | Baseline (median) | Current (median) | Delta | Investigate? |
|---|---|---:|---:|---:|---|
| score | score | 9886 | 9880 | -0.1% | no |
| frame_avg | ms | 2.129 | 2.142 | +0.6% | no |
| frame_p50 | ms | 1.454 | 1.543 | +6.1% | **INVESTIGATE** (median; not all trials) |
| frame_p95 | ms | 4.642 | 4.689 | +1.0% | no |
| frame_p99 | ms | 5.248 | 5.538 | +5.5% | **INVESTIGATE** (median; not all trials) |
| avg_fps | fps | 469.7 | 466.8 | -0.6% | no |
| fps_1low | fps | 190.6 | 180.6 | -5.2% | **INVESTIGATE** (median; not all trials) |
| over33 | frames | 0 | 0 | +0.0% | no |
| streaming | ms | 0.8317 | 0.9267 | +11.4% | **INVESTIGATE** (all trials) |
| record | ms | 0.2999 | 0.3005 | +0.2% | no |
| terraingen | ms/chunk | 2.556 | 2.545 | -0.4% | no |
| meshbuild | ms/mesh | 4.193 | 4.238 | +1.1% | no |
| gpu_avg | ms | 1.551 | 1.507 | -2.8% | no |
| gpu_p95 | ms | 2.334 | 2.324 | -0.4% | no |
| peak_chunks | chunks | 3307 | 3262 | -1.4% | no |
| peak_qgen | jobs | 4 | 4 | +0.0% | no |
| peak_qmesh | jobs | 6 | 5 | -16.7% | no |
| voxel_peak | bytes | 1.43e+08 | 1.412e+08 | -1.3% | no |
| gpu_live_peak | bytes | 6.04e+08 | 6.04e+08 | +0.0% | no |

Grade: baseline S / current S

**Median regressions above the 5.0% noise band: frame_p50, frame_p99, fps_1low, streaming — investigate (do not hide).** '(all trials)' = consistent across every interleaved trial; '(median; not all trials)' = the median regresses but at least one trial was faster (machine-state spread). Both warrant investigation.

## Per-trial values (interleaved)

| Trial | frame_p50 | frame_p95 | frame_p99 | fps_1low | streaming | terraingen | meshbuild | gpu_avg | peak_chunks |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 baseline | 1.834 | 4.914 | 5.747 | 174 | 0.8824 | 2.554 | 4.2 | 1.687 | 3183 |
| 1 current | 1.539 | 4.689 | 5.712 | 175.1 | 0.944 | 2.567 | 4.266 | 1.486 | 3285 |
| 2 baseline | 1.454 | 4.517 | 5.248 | 190.6 | 0.8317 | 2.556 | 4.193 | 1.48 | 3330 |
| 2 current | 1.555 | 4.621 | 5.333 | 187.5 | 0.9265 | 2.543 | 4.231 | 1.507 | 3262 |
| 3 baseline | 1.437 | 4.642 | 5.24 | 190.8 | 0.8039 | 2.562 | 4.188 | 1.551 | 3307 |
| 3 current | 1.543 | 4.707 | 5.538 | 180.6 | 0.9267 | 2.545 | 4.238 | 1.523 | 3259 |
