# Issue #230: Epic #223 closure gate (Phase G — headless gameplay + final verification)

The epic's closure gate: the gameplay loop becomes **scriptable and
digest-covered** in `ft_vox_headless`, and the epic closes with the full
verification + documentation gate (the #211 pattern). This document records
the final verification; the gameplay model reference is
[`../gameplay-loop.md`](../gameplay-loop.md), the runner reference
[`../headless-runner.md`](../headless-runner.md), and the boundary contracts
[`../simulation-render-contracts.md`](../simulation-render-contracts.md).

## What Phase G changed

- **Script grammar** (`src/Headless/SimScript`): `give`, `select`, `drop`
  (fixed +Z), `break`, `placeSelected`, `expect inventory`, `expect items`;
  parse-time bounds (exit 3), assertion mismatches exit 5. Auto-pickup is a
  tick rule exercised through `wait`.
- **Runner frame loop**: `sim.advanceItems(kFrameDt, false)` after
  `advanceMobs` (step 8 of 9) — the same tick rule the graphical host runs.
- **Canonical digest**: deliberate format bump `FTVHDIG1` → `FTVHDIG2`;
  appended selected hotbar slot + 36-slot inventory (fixed order), id-sorted
  item population (quantized motion at a 1/1024-block grid + age) and the
  entity-id allocator high-water.
- **SUMMARY**: `items=` / `inventory=` / `item_high_water=` (key=value + JSON).
- **Tests**: `tests/test_headless_runner.py` 38 → **72 checks** (gameplay
  loop, sync/async parity with items in flight, 3-process gameplay
  round-trip, grammar/assertion exits, JSON gameplay fields).
- **Docs**: new `docs/gameplay-loop.md`; headless-runner / contracts /
  engine-architecture / AGENTS updates.

**No new gameplay behavior** (all landed in A–F), no networking, no ECS.
The only graphical-source change in #230 itself is a refreshed step-9b
comment in `Engine.cpp` — the compiled game code is identical to the
pre-#230 `main` (`4c335ea`).

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8c/16t), Windows 11 Pro, NVIDIA RTX 4070 Ti
  (Vulkan 1.4) — the same reference system as the Epic #204 baselines.
- **Build:** MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`; GLM/FastNoise2
  FetchContent. Validation layers on for the graphical suite.
- **Pre-epic baseline:** `81cb00e` (direct parent of #224/PR #231, the first
  Epic #223 merge), built in a separate worktree (`ft_vox_baseline_223`)
  with the **same** flags/toolchain and reused FetchContent sources.
- **Current:** `66bfe38` (`feat/230-headless-closure-gate`).
- **Linux cross-check:** WSL2 Debian, GCC 14.2, CMake 3.31, Ninja.

## 1. Headless acceptance (the closure proof)

`ctest -R Headless` green: `HeadlessSmoke`, `HeadlessRunner` (72 checks),
`HeadlessDeps`. The acceptance driver covers:

- **Gameplay loop end-to-end** — the canonical fixture
  (`tests/fixtures/sim-actions.txt`): locomotion + creative edits, then
  `give → select → break → wait (auto-pickup) → placeSelected → drop`,
  ending with a manual drop **in flight** at the digest boundary.
  End state: `edits=6`, `items=1`, `inventory=3`, `item_high_water=2`.
- **Sync vs async digest parity with item physics** — `--workers 0` vs
  `--workers 2`, twice each, per-frame quiesce unchanged: identical digests.
- **Multi-process save/reload round-trip** (3 processes, isolated temp saves
  root): inventory + item entities + id allocator restored; the second
  session's drop gets **id 2** (high-water resumed — no id reuse);
  `inventory.state` / `items.state` written and re-read.
- **Grammar error exits** (14 cases → exit 3): unknown item token, `None`
  sentinel, count > 65535 / negative, hotbar slot > 8 / negative, inventory
  slot > 35, cell-y out of world, `empty` with nonzero count, expected item
  count > cap / negative.
- **Assertion mismatches** (`expect inventory`, `expect items` → exit 5).
- **JSON gameplay summary** — `items` / `inventory` / `item_high_water` /
  `edits` / `digest` agree with the key=value run (the JSON object is located
  by its `{` line: `grantItems` logs may precede it).
- All #210 checks still pass (determinism, edit round-trip, residency churn,
  every failure exit code, timeout teardown).

### Determinism report (repeated runs, items in flight)

`--seed 42 --ticks 600 --script tests/fixtures/sim-actions.txt`, four runs:

| Run | Mode | Digest | items | inventory | item_high_water | edits |
|---|---|---|---:|---:|---:|---:|
| 1 | sync (`--workers 0`) | `9b5013db5e550fe2` | 1 | 3 | 2 | 6 |
| 2 | sync | `9b5013db5e550fe2` | 1 | 3 | 2 | 6 |
| 3 | async (`--workers 2`) | `9b5013db5e550fe2` | 1 | 3 | 2 | 6 |
| 4 | async | `9b5013db5e550fe2` | 1 | 3 | 2 | 6 |

Identical authoritative state across repeated runs and generation modes,
with a live item entity (quantized motion + age) hashed at the boundary.
Digest format `FTVHDIG2`; the bump is deliberate and documented — no
expected digest is hardcoded in tests/fixtures (computed at runtime).

## 2. Guards (CPU/GPU boundary, final module set)

| Gate | Result |
|---|---|
| `ft_vox_assert_cpu_link_closure` (configure-time) | green — covers `ft_vox_core`, `ft_vox_headless` and every CPU test added by the epic (`test_items`, `test_gameplay`, …); re-run at the CPU-only reconfigure |
| `CoreIncludeGuard` (`check_core_includes.py`) | green — roots cover `Items/` (all 8 files) + the core closure |
| `HeadlessDeps` (`check_headless_deps.py`) | green — runner include closure (the new `Items/` includes are core-legal), core/runner link artifacts, CPU-only cache discovery |
| `CpuOnlyBuild` / `FT_VOX_GRAPHICS=OFF` | see §5 |

## 3. Full graphical CTest suite (Windows/MSVC Release, validation on)

`ctest --test-dir build -C Release --output-on-failure` →
**47/47 PASS** (189.6 s), including:

- **VisualRegression** — all 22 scenes within tolerance: the existing
  goldens **hold** (the harness draws no ImGui; reference scenes contain no
  items) and the Phase F `item_entities` scene passes against its committed
  reference.
- `Items`, `GameplayLoop`, `GameSimulation`, `WorldSave`, `WorldPersistence`
  (the epic's CPU suites, also linked into the graphical build),
  `EngineReload`, `UiShell`, `RenderBridge`, `HeadlessRunner`, guards, and
  the full render/Vulkan suite.

## 4. In-game benchmark parity vs the pre-epic baseline

Protocol: `tests/bench_parity_211.py`, **3 interleaved trials** of
`--seed 42 --benchmark 20`, baseline `81cb00e` vs current `66bfe38`, same
device/flags/settings. Full table + per-trial appendix:
[`issue-230-parity.md`](issue-230-parity.md). Median of 3:

| Metric | Baseline | Current | Delta |
|---|---:|---:|---:|
| Score / Grade | 9886 / **S** | 9880 / **S** | parity |
| Frame avg (ms) | 2.129 | 2.142 | +0.6% |
| Frame p50 (ms) | 1.454 | 1.543 | +6.1% (median; not all trials — see investigation) |
| Frame p95 (ms) | 4.642 | 4.689 | +1.0% |
| Frame p99 (ms) | 5.248 | 5.538 | +5.5% (median; not all trials) |
| Avg FPS | 469.7 | 466.8 | −0.6% |
| 1% low FPS | 190.6 | 180.6 | −5.2% (median; not all trials) |
| Frames > 33.3 ms | 0 | 0 | parity |
| Streaming scope (ms) | 0.8317 | 0.9267 | +11.4% (all trials — investigated below) |
| Record (ms) | 0.2999 | 0.3005 | +0.2% |
| TerrainGen (ms/chunk) | 2.556 | 2.545 | −0.4% |
| MeshBuild (ms/mesh) | 4.193 | 4.238 | +1.1% |
| GPU avg / p95 (ms) | 1.551 / 2.334 | 1.507 / 2.324 | **−2.8% / −0.4%** |
| Peak chunks | 3307 | 3262 | −1.4% |
| Queue peaks Gen/Mesh | 4 / 6 | 4 / 5 | parity-or-better |
| Resident voxel bytes (peak) | 1.43e8 | 1.412e8 | −1.3% |
| GPU live bytes (peak) | 6.04e8 | 6.04e8 | 0.0% |

### Investigation (protocol: investigate, don't hide)

**The item step early-outs at zero items — confirmed.** Benchmark scenes
contain no items. `Engine::tickItems` at zero population runs empty loops
(no per-entity work, no collision queries, no light sampling, no instance
draws); the GPU `Mobs` scope (which now includes the item draw) measures
**3.5e-06 ms ≈ 0** on both binaries. The zero-item cost is a profiler scope
+ a scoped collision-view construction + empty snapshot/event drains —
microseconds, invisible in the frame percentiles.

**Streaming scope +11.4% (consistent across trials) — explained, bounded,
no extra work.** Investigated per the protocol:

- **The streaming code path is byte-identical** between `81cb00e` and
  `66bfe38`: no diff in `Engine::tickStreaming`, the bridge's per-frame
  path (the only bridge change is the edit-path `publishEditResult`
  refactor, #227 — zero edits in the benchmark), `VoxelWorld` streaming, or
  `Chunk/`.
- **The deterministic work counters are identical** (direct 20 s pair,
  baseline vs current): frames **8837 / 8836**, avg FPS **444.0 / 444.0**,
  stream maintenance `zeroWork` **80.24% / 80.15%**, `queueSorts`
  **1746 / 1754**, `rowsVisited` **137934 / 138566**, enter/exit
  **~20380 / ~20365**, `unloadScans` **456 / 451**, TerrainGen jobs
  **11003 / 10947**, LightCache jobs **1920 / 1920**, queue peaks parity.
  The streaming tick does the **same work per second**.
- **Every other CPU scope is parity-or-better** in the direct pair:
  Acquire 0.961 → 0.925, Record 0.298 → 0.282, ImGui 0.043 → 0.041; GPU avg
  −2.8%. In that pair the current frame p50 is **faster** (1.551 vs 1.639).
- **Conclusion:** the ~0.06–0.09 ms shift of the *measured Streaming scope
  average* is an attribution/code-layout effect of the epic's added
  per-frame steps (Items tick, item-renderer prepare/record, inventory
  snapshot for the UI) sharing the frame — not additional streaming work.
  It is bounded (0.93 ms ≪ 16.7 ms budget), with **zero frames > 16.7 ms**
  and no user-facing throughput change (frames/FPS/score identical).
- **The frame p50/p99/1%-low "median regressions" are machine-state
  spread, not consistent:** they flip sign across runs (parity trial 1
  current p50 **16% faster**; the direct post-protocol pair current p50
  faster; trials 2–3 slightly slower). The per-trial appendix shows the
  spread; no trial-unanimous frame-time regression exists.
- **Attribution:** #230 itself changes **zero compiled graphical code**
  (one comment). Any delta vs the pre-epic baseline is attributable to
  phases A–F, each merged with its own full-suite + golden verification.

**Verdict: no unexplained sustained regression.** Grade S/S, throughput and
workload character identical, GPU parity-or-better, the item step provably
zero-cost at zero items, and the one consistent scope shift investigated
and explained (same work, bounded, no user-facing impact).

## 5. CPU-only gate (`FT_VOX_GRAPHICS=OFF`)

**Windows/MSVC + Ninja** (`build-r2-ninja`, MSVC 14.44, Release, no vcpkg
toolchain needed): explicit reconfigure (re-runs the configure-time
transitive link-closure assertion on `ft_vox_core`, `ft_vox_headless` and
every CPU test — green), incremental rebuild, then
`ctest --output-on-failure`:

```text
100% tests passed out of 25   (78.6 s)
  PlayerPhysics · CoreContracts · VoxelWorld · GameSimulation · GameplayLoop
  Items · CoreIncludeGuard · VisualLookDefaults · AutoExposure ·
  GraphicsSettings · StreamOptHelpers · TerrainGeneration · BiomeMapGeneration
  VoxelPool · BorderPool · MeshResultPool · ProfilerWorkerConsistency ·
  WorkloadTelemetry(+Disabled) · PassiveMobs · FramePacing · WorldSave ·
  HeadlessSmoke · HeadlessRunner (72 checks) · HeadlessDeps
```

The graphics stack is **not discovered at all** in this configuration
(`HeadlessDeps` discovery check green: no Vulkan/SDL3/volk/VMA/miniz/shader
entries in the cache). The epic's CPU suites (`Items`, `GameplayLoop`) and
the extended `HeadlessRunner` run without a GPU, window or shader asset.

## 6. Linux cross-check (WSL2 Debian, GCC 14.2)

The existing CPU-only Ninja tree (`~/ft_vox-cpu-224`, configured against the
current source, `-DFT_VOX_GRAPHICS=OFF -DFT_VOX_DEP_MODE=system`, no Vulkan
SDK / SDL3 / shader toolchain discovered) reconfigured + rebuilt + tested:

```text
100% tests passed, 0 tests failed out of 26   (60.6 s)
  (the 25 above + NetworkIntegrationTest — Boost is available on WSL;
   test_network is covered by the same link-closure assertion)
  HeadlessRunner ........ Passed   (72-check driver, GCC build)
  HeadlessDeps .......... Passed   (Ninja core static-lib link-edge scan)
```

The gameplay grammar, digest (`FTVHDIG2`) and round-trips behave identically
under GCC; the digest is reproducible per binary/platform (cross-compiler
bit-identity is an explicit non-goal — `FT_VOX_PORTABLE_FP` exists for
cross-machine comparison within a platform family).

## 7. Child-issue acceptance (Epic #223)

| Phase | Issue | PR | Acceptance |
|---|---|---|---|
| A | #224 | #231 | item domain contracts + `test_items` — met |
| B | #225 | #232 | inventory ownership + commands + snapshot — met |
| C | #226 | #233 | item entities + break/drop/pickup loop — met |
| D | #227 | #234 | sim raycast + place-from-hotbar + commands-only host — met |
| E | #228 | #235 | FTVI/FTVJ persistence + id durability + round-trips — met |
| F | #229 | #236 | hotbar HUD + inventory panel + ItemEntityRenderer + golden scene — met |
| G | #230 | this PR | grammar + digest + runner tests + guards + docs + this gate — met |

## 8. Epic definition of done

- [x] The full loop is playable in-game (break → entity → pickup → hotbar →
      place consumes; panel manages stacks; survives save/close/reopen) —
      #226–#229 + author in-game validation.
- [x] The same loop is scriptable headlessly with identical sync/async
      results; the digest covers inventory + item entities; a multi-process
      round-trip passes — §1.
- [x] No gameplay logic in `Engine` / `RenderWorldBridge` / `GameUI`; every
      interaction is an explicit sim command with a result code — #227
      rewiring + guards (§2).
- [x] Item/entity ids stable across save/reload; allocator high-water
      persisted; snapshots id-sorted and bounded — #224/#226/#228 + §1
      round-trip (no id reuse).
- [x] CPU-only build passes with the new modules; `CoreIncludeGuard` /
      `HeadlessDeps` / link-closure green — §2, §5, §6.
- [x] Full CTest + visual goldens hold; docs updated and cross-linked — §3,
      `docs/gameplay-loop.md` + the four updated references.

## Conclusion

Phase G closes Epic #223. The gameplay loop is **scriptable and
digest-covered** end-to-end in the graphics-free runner (72-check acceptance
driver; sync/async parity with items in flight; 3-process round-trip with no
id reuse), the CPU/GPU boundary gates are green on the final module set
(Windows/MSVC CPU-only **25/25**, WSL2 GCC **26/26**), the full graphical
suite and all visual goldens hold (**47/47**), and the in-game benchmark is
at parity with the pre-epic baseline (Grade S/S; the item step early-outs at
zero cost; the one consistent scope shift investigated and explained — same
work, no user-facing impact). The epic definition of done is fully ticked.