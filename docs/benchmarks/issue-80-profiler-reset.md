# Issue #80: complete profiler capture reset

`clearHistory()` now resets all completed capture state immediately: frame
time, scope hierarchy, graph, averages, percentile, spikes, displayed worker
snapshot, and old pending worker aggregates. Capture enabled/paused state and
registered worker names are retained. The UI reads profiler metrics directly
instead of substituting stale Engine frame/FPS values after a reset.

Reset advances an atomic capture epoch. Each bucket checks the sample epoch
under its existing count/time mutex. A writer that reaches a bucket before
the reset sweep can retire old data and install the new epoch; the sweep
preserves that new data. Old-epoch submissions are rejected. Terrain, mesh,
queue-wait, and biome-map measurements carry the epoch from job submission,
so late completion cannot relabel pre-reset work as a new sample.

An open frame keeps its instrumentation stack intact, but its completed
capture is discarded. The next full frame starts the fresh history. Engine
benchmark sampling skips the discarded frame, including with zero warmup.
Reset, snapshot, frame operations, and UI reads belong to the capture thread;
worker submissions may run concurrently.

## Regression coverage

`test_profiler` covers:

- idle reset with populated history, scopes, spikes, displayed worker data,
  and additional pending data; repeated reset while capture is paused;
- reset within nested scopes, preserving the live stack and parent links,
  discarding the interrupted frame, and publishing a fresh hierarchy next;
- rejection of both pending and late old-epoch samples before the first
  new frame, modeling reload with zero warmup;
- four gated workers straddling reset: exactly four new samples survive;
- 1,000 resets racing stale/new worker submissions, retaining only valid
  count/time pairs from the current epoch;
- existing concurrent snapshot and registration tests (240,000 worker
  samples drained with exact counts/times and zero split violations).

Windows/MSVC Release builds of `ft_vox` and `test_profiler` pass. The full
ten-suite CTest run includes terrain, Vulkan resources, biome maps, and the
profiler. No sanitizer or Linux/macOS run was performed.

## Zero-warmup runtime check

The new optional `--benchmark-warmup` argument allows this case to be
reproduced without changing source code; the default warmup is unchanged.

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 5 --benchmark-warmup 0 --benchmark-map 0.1
```

2026-09-04, base revision `b6e774db1246` plus this change, RTX 4070 Ti,
1920x1080, view distance 512, IMMEDIATE presentation:

- report confirms warmup **0 s**, measured **5.00107 s**, **976 frames**;
- **2,385** TerrainGen jobs, **1,771** MeshBuild jobs, **5** completed maps;
- mean frame **5.01713 ms**, minimum **4.9539 ms** (no zero-duration reset
  frame included).

This runtime smoke exercises reload/reset and subsequent worker capture.
The deterministic tests, rather than aggregate runtime timings, establish
the old-epoch rejection contract.

[Raw runtime report](bench_20260904_103355_b6e774db1246__s42_sc9996.txt)
