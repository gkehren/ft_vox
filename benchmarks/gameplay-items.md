# Dropped-item gameplay benchmark (stabilization Phase 5)

Repeatable CPU-only measurement of the gameplay systems introduced by Epic
#223 (dropped items, pickup, persistence), with a baseline comparison against
`main @ 345dfa12`. All numbers below are **measured**, not estimated. GPU
rendering time is out of scope for this CPU-only bench (the graphical
validation profile lives in `docs/gpu-validation.md`, Phase 6); "frame" here
means the **CPU simulation frame** the host drives, not a presented frame.

## 1. Tool

`bench_gameplay_items` (source: `tests/bench_gameplay_items.cpp`) links ONLY
`ft_vox_core` — no GPU, window, camera or ImGui. It is a **manual** benchmark,
deliberately not registered with ctest. It uses only public facade APIs that
exist at the baseline commit, so the identical source measures both commits.

Scenarios (fixed seed 42, fixed host dt = 1/60, fixed streaming budgets,
mobs disabled to isolate the item cost, flight-mode host-driven player):

| Scenario | Items | Purpose |
|---|---:|---|
| `baseline` | 0 | fixed gameplay overhead |
| `light` | 100 | normal gameplay |
| `medium` | 512 | stress |
| `max` | 1024 | population-cap behavior |
| `pickup` | 1024 | inventory merge + removal burst (feet armed onto the landed cluster at measurement start) |
| `streaming` | 100→1024 ramp | observer crossing chunk boundaries at 8 blocks/s while spawning |
| `save` | 1024 | `savePlayerState` / close / reopen + serialized bytes |

Measured independently per scenario:

- **item simulation CPU time** — wall time of `advanceItems` alone;
- **full sim frame time** — `advancePlayer` + `advanceMobs` + `advanceItems`
  + streaming maintenance + `pumpWorld` + snapshot fill + event drain (the
  host's per-frame CPU cost);
- **snapshot generation** — `itemSnapshots` into a reused buffer;
- **collision-query cost** — `physics::QueryStats` cells/iterations counters;
- **allocations per tick** — global `new`/`delete` counter delta over the
  measured window (sample storage is reserved before the window so the
  benchmark does not count itself);
- **peak resident memory** — platform RSS high-water;
- **save duration / serialized bytes / reopen duration** — `save` scenario;
- **pickup burst** — the burst lives in the first measured frames, so the
  median hides it: the peak item-step time and `PickedUp` event counts are
  reported separately.

Methodology: Release builds; 240-tick warm-up before measuring; 600 measured
ticks; **5 repetitions**, each a fresh simulation; percentiles pool the
per-frame samples of all repetitions; variability is the spread of the
per-rep medians (`frameMedianAcrossReps` in the JSON). Workers = 0
(fully synchronous generation) for determinism and clean attribution.

## 2. Environment

| | Baseline vs current comparison | Supplementary |
|---|---|---|
| Machine | AMD Ryzen 7 9800X3D (8C/16T), 28 GB RAM | same |
| OS | WSL2 Debian (Linux 6.x) on Windows 11 | Windows 11 native |
| Compiler | GCC 14.2.0 (Debian), `-O3` Release | MSVC (VS 18 2026), Release |
| Build | `cmake -G Ninja -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release` | `build-cpu-fp` (CPU-only, portable FP) |
| Workers | 0 (synchronous) | 0 |
| GPU | not used (CPU-only) | not used |

Both commits were built and run **on the same machine, in the same session,
with the same flags**. The baseline was built in a separate worktree at
`345dfa12` with the benchmark source copied in (no other change).

## 3. Reproduction

```bash
# Current (repo root):
cmake -S . -B build-cpu -G Ninja -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu -j --target bench_gameplay_items
./build-cpu/tests/bench_gameplay_items --reps 5 --ticks 600 --warmup 240 \
    --workers 0 --json benchmarks/results/current-linux-gcc14.json \
    --csv benchmarks/results/current-linux-gcc14.csv

# Baseline (worktree at 345dfa12, benchmark source + CMake target copied in):
git worktree add ../ft_vox-baseline 345dfa12e55923b7611b8f0abe15dc88920fa806
cp tests/bench_gameplay_items.cpp ../ft_vox-baseline/tests/
# add to ../ft_vox-baseline/tests/CMakeLists.txt:
#   add_executable(bench_gameplay_items bench_gameplay_items.cpp)
#   target_link_libraries(bench_gameplay_items PRIVATE ft_vox_core)
cmake -S ../ft_vox-baseline -B ../ft_vox-baseline/build-cpu -G Ninja \
    -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build ../ft_vox-baseline/build-cpu -j --target bench_gameplay_items
../ft_vox-baseline/build-cpu/tests/bench_gameplay_items --reps 5 --ticks 600 \
    --warmup 240 --workers 0 --json baseline.json --csv baseline.csv

# Windows (MSVC, CPU-only):
cmake --build build-cpu-fp --config Release --target bench_gameplay_items
.\build-cpu-fp\tests\Release\bench_gameplay_items.exe --reps 5 --json out.json
```

Raw results: `benchmarks/results/{current,baseline-345dfa1}-linux-gcc14.{json,csv}`
and `benchmarks/results/current-windows-msvc.{json,csv}`.

## 4. Results — Linux GCC 14 (baseline vs current)

CPU simulation frame time, milliseconds (p50 / p95 / p99 over 5×600 frames):

| Scenario | Baseline 345dfa1 | Current (PR1–PR4 stack) | Δ p50 |
|---|---|---|---|
| baseline (0) | 0.00012 / 0.00013 / 0.00015 | 0.00012 / 0.00012 / 0.00013 | 0% |
| light (100) | 0.01463 / 0.01726 / 0.03136 | 0.01449 / 0.01742 / 0.02650 | −1.0% |
| medium (512) | 0.06499 / 0.09332 / 0.17916 | 0.06233 / 0.08538 / 0.11505 | −4.1% |
| max (1024) | 0.14637 / 0.20015 / 0.33645 | 0.12844 / 0.17006 / 0.26317 | −12.3% |
| streaming | 4.19972 / 5.89204 / 7.13258 | 4.09131 / 5.61370 / 6.45727 | −2.6% |
| save | 0.13441 / 0.18184 / 0.25607 | 0.14630 / 0.21708 / 0.36994 | +8.9% |

Item-step time (`advanceItems` alone), p50 ms:

| Scenario | Baseline | Current |
|---|---|---|
| light | 0.01384 | 0.01369 |
| medium | 0.06077 | 0.05806 |
| max | 0.13903 | 0.12002 |
| streaming | 0.02914 | 0.02598 |
| pickup burst (peak tick) | 0.23738 | 0.12536 |

**Verdict: no systematic regression.** Every delta is within the per-rep
variability (e.g. `max` rep medians: baseline 0.130–0.153, current
0.123–0.131; `save`: baseline 0.132–0.145, current 0.135–0.150 — overlapping
ranges). The +8.9% on `save` p50 and the −12.3% on `max` p50 are noise of the
same magnitude in opposite directions; an earlier unpaired run showed the
reverse ordering. This is expected: **PR1–PR3 do not touch the per-tick item
path** (raycast validation, flush capture and the id guard are outside the
measured loop), so any true difference would be a red flag — none is
observed. The 0-item baseline is 0.12 µs/frame on both commits: the fixed
gameplay overhead with no items is negligible.

### Scaling and hotspots (current, Linux)

- Item cost scales **linearly**: ≈0.117 µs per item per tick
  (100 → 13.7 µs, 512 → 58.1 µs, 1024 → 120 µs).
- Collision queries: ≈7.5 cells / item / tick (765 / 3826 / 7669 cells),
  1 swept-move iteration per item — the shared `physics::move` solver, no
  per-item allocation.
- Snapshot fill: 0.74 µs (100) → 7.3 µs (1024), in-place buffer reuse.
- At the 1024 cap the whole item step is **0.12 ms/tick** — ~0.7% of a 60 Hz
  frame budget (16.7 ms). No FPS acceptance threshold is invented; the
  population cap is respected (final population == 1024, never above).

### Allocations

- **Steady state: 0 allocations/tick** in every static scenario (baseline,
  light, medium, max, save) — the normal item path is allocation-free.
- Pickup burst: 0.17 allocs/tick averaged over the window — the bounded
  spawn/remove event deque allocating blocks while 1024 `Removed` events
  stream in one tick (cap 4096, drained every frame). Bounded, not growth.
- Streaming: 483.9 allocs/tick — **terrain generation**, not the item path
  (voxel/border pool growth + FastNoise2 per generated chunk; the item step
  itself stays at 26 µs with 0 item-side allocations). Documented so the
  number is not misattributed to items.

### Pickup burst

1024 items merged hotbar-first into the 36-slot inventory in a single tick:
peak item-step **0.125 ms** (Linux) / **0.246 ms** (Windows MSVC); all 1024
`PickedUp` events per rep (5120 over 5 reps), final population 0, inventory
conservation verified by the scenario gate. No duplication, no remainder
loss (capacity 2304 ≥ 1024).

### Streaming

Frame p50 4.09 ms is dominated by **synchronous chunk generation** (budget 4
chunks/frame, ~1 ms/chunk on this machine) — the item contribution is
26 µs. Population ramps 100 → 800 over the window (spawn cadence 100/120
ticks), never exceeds the cap. Peak RSS 36.6 MB (chunk residency), stable
across reps — no unbounded growth.

### Save / reload (1024 items)

| Metric | Linux GCC | Windows MSVC |
|---|---|---|
| `savePlayerState` | 0.40 ms | 4.25 ms |
| close (flush + shutdown) | 0.23 ms | 2.59 ms |
| reopen (bootstrap + restore) | 27.2 ms | 61.6 ms |
| serialized bytes | 69,866 | 69,866 |
| population verified after reopen | yes | yes |

Byte accounting: 1024 × 68 B entities + 20 B items header + 153 B inventory
+ 45 B player + 16 B world.meta = 69,866 (no chunk edits in this scenario).
Reopen cost is dominated by terrain bootstrap regeneration, not the item
restore. Windows filesystem latency explains the save/close delta; both are
sub-5 ms. **These are pre-checkpoint numbers** — Phase 3 adds the staging +
manifest commit overhead, measured separately in its own PR.

### Memory

Peak RSS: 9.7 MB (static scenarios), 36.6 MB (streaming), 37.3 MB (save) —
bounded by chunk residency + the 1024-entity cap; identical across reps.

## 5. Supplementary — Windows MSVC (current only)

Frame p50/p95/p99 ms: baseline 0.0001/0.0002/0.0002; light 0.0197/0.0271/0.0439;
medium 0.0972/0.1268/0.1609; max 0.2078/0.2861/0.3688; streaming
9.3423/12.7595/15.4856; save 0.2153/0.2820/0.3021. Pickup burst peak 0.2464 ms.
Steady-state allocations 0/tick; streaming 500.6/tick (terrain generation).
The MSVC item step is ~1.6× the GCC cost at the cap (0.20 vs 0.12 ms) —
compiler codegen difference, same algorithm; **not** comparable across
platforms as if equivalent (documented per the methodology).

## 6. Limitations

- WSL2 timer granularity is 100 ns; sub-µs medians (0-item baseline) are at
  the measurement floor — read them as "negligible", not exact.
- One machine, one compiler per comparison; no cross-hardware claims.
- Workers = 0 only; async-worker item cost is covered by the headless
  sync/async digest parity gate (`HeadlessRunner`), not timed here.
- GPU submission/render time for items is measured by the Phase 6 graphical
  profile, not this bench.
