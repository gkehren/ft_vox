# ft_vox stabilization pass — final report (post-Epic #204/#223)

Baseline: `main @ 345dfa12e55923b7611b8f0abe15dc88920fa806`.
Scope: the six-phase stabilization mission (raycast hardening, deferred-edit
consistency, crash-consistent persistence, entity-id exhaustion, item
performance benchmarks, GPU validation). No epic redesign, no ECS, no
networking, no new global state, no hot-path heap churn; the #204 CPU/GPU
boundary and the fixed-step rates (120/60/60 Hz) are preserved.

## 1. Implementation summary

PRs are **stacked** (each base is the previous PR; retarget to `main` as the
stack merges). CI (`cpu-boundary`) runs on the `main`-targeted PR #238 and
re-runs per PR as bases retarget; every step was additionally validated
locally on Windows MSVC + Linux GCC (WSL2).

| Phase | Status | PR | Files | Behavioral change | Compatibility |
|---|---|---|---|---|---|
| 1 — raycast validation + bounds | **completed** | [#238](https://github.com/gkehren/ft_vox/pull/238) | `src/Sim/GameSimulation.{hpp,cpp}`, `tests/test_gameplay.cpp`, docs | Invalid rays (NaN/Inf origin/dir/distance, degenerate/huge dirs, out-of-envelope origins) answer safe no-hit; march clamped to `kMaxRaycastDistance=64` + iteration-bounded; double-precision overflow-safe direction gate | Valid rays bit-exact historical (parity fixtures unchanged); UI range 2–32 unaffected |
| 2C — authoritative query unification (in PR1) | **completed** | [#238](https://github.com/gkehren/ft_vox/pull/238) | same | Targeting samples the EFFECTIVE state (scoped `WorldCollisionView`, pending-edit overlay) — same state as break/place/physics; one shared lock per march (cheaper than per-cell) | Fluids/vegetation/adjacent-fallback/unknown-cell rules preserved; `isVoxelActive` stays the creative seam |
| 2 — deferred edit/drop consistency | **completed** | [#239](https://github.com/gkehren/ft_vox/pull/239) | `src/World/VoxelWorld.cpp`, `tests/test_gameplay.cpp`, docs | Investigation: acceptance-boundary model already correct (one break = one drop; overlay-consistent physics/targeting; coalescing; unload bake). **Fixed gap**: `captureAllChunkEdits` now bakes canonical pending edits of in-transit/UNLOADED records at flush (previously a mid-transit flush persisted the pre-edit chunk beside post-edit gameplay state → crash resurrected the block while its drop survived) | Publication ordering (#114) untouched; digests unchanged; regression tests for deferred break/place/unload/border transactions |
| 4 — entity-id exhaustion | **completed** | [#240](https://github.com/gkehren/ft_vox/pull/240) | `src/Items/ItemEntitySystem.{hpp,cpp}`, `src/Items/ItemTypes.hpp`, `src/Sim/GameSimulation.{hpp,cpp}`, `src/Headless/HeadlessRunner.cpp`, tests, docs | Allocator never wraps: max assignable id `UINT64_MAX-1`, rests at `UINT64_MAX` (valid persisted NEXT); refused spawns consume nothing (`BreakResult::IdExhausted` before the world edit; `ItemResult::IdExhausted` before inventory mutation); id 0 permanently invalid | Append-only result vocabularies; normal spawn path unchanged; exhausted state survives save/reload |
| 5 — item benchmarks | **completed** | [#241](https://github.com/gkehren/ft_vox/pull/241) | `tests/bench_gameplay_items.cpp`, `tests/CMakeLists.txt`, `benchmarks/gameplay-items.md`, `benchmarks/results/*` | New manual CPU-only benchmark (7 scenarios, JSON/CSV, ≥5 reps, warm-up, baseline-comparable source); added to the CPU link-closure guard | Not in ctest; no runtime change |
| 3 — crash-consistent checkpoints | **completed** (2 PRs) | [#242](https://github.com/gkehren/ft_vox/pull/242) (ADR + format/recovery layer), [#243](https://github.com/gkehren/ft_vox/pull/243) (runtime integration) | `docs/adr/gameplay-persistence-transactions.md`, `src/World/WorldCheckpoint.{hpp,cpp}`, `src/World/WorldSave.{hpp,cpp}`, `src/World/WorldPersistence.{hpp,cpp}`, `src/World/VoxelWorld.{hpp,cpp}`, `src/Sim/GameSimulation.cpp`, `CMakeLists.txt`, tests, docs | Generation-based checkpoints + atomic manifest; `savePlayerState` = flush barrier → ONE transactional commit (success only when the manifest committed); shutdown/close carry-forward commits; recovery selects the newest fully valid generation, re-seeds the live set, routes gameplay reads; corrupt generation → whole fallback; none valid → explicit refusal; chunk edits INSIDE the checkpoint (scope guard met) | Legacy saves open unchanged (additive migration); root state files + live chunks maintained as rollback shims; `flushWorld` keeps its intermediate-barrier meaning (documented); `PlayerPersistState` moved to `worldsave` with a global alias (source-compatible) |
| 6 — GPU validation | **completed** | [#244](https://github.com/gkehren/ft_vox/pull/244) | `docs/gpu-validation.md`, `docs/visual-regression.md`, `tests/test_visual_regression.cpp`, 2 new reference PNGs, `.github/workflows/gpu-validation.yml`, execution log | GPU profile doc; `item_population` (128) + `item_max_population` (1024) goldens; `item_rendering_checks` semantic suite (interpolation/culling/atlas reload/cap/epoch reset); optional GPU CI workflow | **No existing golden modified**; strict gate passes unchanged; CPU CI never depends on a GPU |

## 2. Findings (evidence-based)

**P1 — none found.**

**P2 (important correctness/robustness, all fixed):**
1. **Raycast UB / non-termination** (#238): NaN/Inf origins reached `floor()→int`
   conversions (UB); `+Inf`/huge `maxDist` stalled the float accumulator
   (`t += 0.05f` stops advancing at the precision wall) → infinite loop;
   finite-but-huge directions overflowed the float `dot` gate to `+Inf` and
   normalized to zero. Evidence: code inspection + new validation fixtures
   (the `+Inf` case cannot even be run against the old code without hanging).
2. **Targeting/collision query divergence** (#238): the march read raw backing
   (`isVoxelActive`) while gameplay commands/physics read the effective
   pending-edit overlay — a deferred deletion stayed targetable and a deferred
   placement untargetable until publication. Evidence:
   `testRaycastPendingEditSemantics` (raw backing vs effective assertions).
3. **Mid-transit flush durability gap** (#239): `captureAllChunkEdits` skipped
   in-transit records, so a flush during a mesh/gen transit persisted the
   pre-edit chunk while `items.state` carried the accepted break's drop; a
   crash before the next flush resurrected the block on reopen → duplication
   on re-break. Evidence: file-level assertion in
   `testDeferredEditDropConsistency` (fails on the old code).
4. **Non-transactional multi-file persistence** (#242/#243): independent
   atomic writes of player/inventory/items/chunks allowed mixed-generation
   crash states (the general form of finding 3). Evidence: ADR analysis +
   the fault-injection matrix (10 deterministic points, all passing).

**P3 (minor risk / debt):**
5. **Entity-id wraparound** (#240): at `m_nextId == UINT64_MAX` the allocator
   wrapped to the invalid id 0 and `idHighWater()` underflowed — theoretical
   (2^64 spawns) but violates the stable-id contract; guarded.
6. **Misleading documentation** (#238/#239): `gameplay-loop.md` claimed drop
   trajectories are computed "against the pre-edit cell" — false since the
   collision overlay exists; corrected.
7. **TSan on WSL2** (environment, documented): `unexpected memory mapping`
   fatal at startup under WSL2 ASLR; `setarch -R` workaround used locally;
   GitHub `ubuntu-latest` CI runs TSan natively and passes.
8. **Allocation observations (not bugs, measured)**: pickup-burst event-deque
   block allocations (bounded, 0.17/tick averaged) and streaming
   terrain-generation allocations (~484/tick) — attributed in the benchmark
   report; the steady-state item path is allocation-free (0/tick).

## 3. Test results (actual)

| Gate | Environment | Result |
|---|---|---|
| CPU-only suite | Windows MSVC (VS 18 2026) Release, `build-cpu-fp` | **PASSED 26/26** (final stack) |
| CPU-only suite | Linux GCC 14.2 (WSL2 Debian) Release Ninja, `build-cpu` | **PASSED 27/27** incl. `CpuOnlyBuild` clean-configure gate (148 s) |
| CPU-only CI | GitHub `cpu-boundary` on PR #238 | **PASSED**: cpu-linux, cpu-windows, cpu-sanitizers, tsan, graphics-linux |
| ASan + UBSan | Linux GCC Debug, `build-asan-linux`, leak detection on, CI exclusions (`CpuOnlyBuild`, `TerrainGeneration`) | **PASSED 26/26** (incl. `WorldCheckpoint`, `GameplayLoop`, `HeadlessRunner` 153 s) |
| TSan | Linux GCC Debug, `build-tsan-linux`, CI scope (`NetworkIntegrationTest`, `HeadlessSmoke`); `setarch -R` for the WSL2 ASLR issue | **PASSED** (18 network checks, 0 failures; smoke digest ok). Excluded per CI: `TerrainGeneration`, full `HeadlessRunner` (expected TSan overhead) |
| Headless runner | sync + async workers, canonical fixture | **PASSED** — digest `9b5013db5e550fe2` unchanged; sync/async parity; multi-process save/reload; gameplay ops (give/select/drop/break/placeSelected/expect) |
| Persistence recovery | fault-injection points 1–10 (1–5,7–10 in `test_world_checkpoint`; 6 via the SaveService write seam in `test_gameplay`) | **PASSED** — pre-publication faults recover the previous complete generation; post-publication faults recover the new one; never a mixture; legacy compatibility + live-reseed isolation verified |
| Vulkan device tests | canonical RTX 4070 Ti, MSVC Release, validation on | **PASSED 8/8** (resource smoke + error probe, mesh upload, mesh arena, mob render, post resize, world persistence w/ checkpoints, engine reload) |
| Visual regression — smoke | canonical GPU, ctest | **PASSED** (all scenes incl. the 2 new item scenes, 74 s) |
| Visual regression — **strict** | canonical GPU, direct `--strict` | **PASSED — 24 scenes within tolerance**, all standalone checks green |
| Gameplay item checks | `item_rendering_checks` on the canonical GPU | **PASSED** (interpolation, culling, atlas reload, cap, epoch reset; 0 new validation errors) |
| Remaining graphical suite | UI shell, input routing, bridge, facekey, chunk lifecycle, etc. | **PASSED 38/38** |
| Gameplay UI (interactive) | ImGui-free harness by design | CPU logic gates **PASSED**; interactive GPU smoke = documented manual checklist (**not automated**) |

Validation errors: zero from `WorldRenderer::init` onward on every device
run; 7 device-init errors tolerated as the documented RTSS/OBS overlay
baseline (`docs/vulkan-validation.md`).

## 4. Performance results

Full report: `benchmarks/gameplay-items.md`; raw data: `benchmarks/results/`.
Hardware: AMD Ryzen 7 9800X3D, 28 GB; comparison pair both on WSL2 Debian
GCC 14.2 `-O3`, workers 0, seed 42, 5 reps × (240 warm-up + 600 measured)
ticks; baseline = `345dfa12` worktree with the identical benchmark source.

- **No systematic regression**: frame-time deltas within overlapping per-rep
  variability (light −1.0%, medium −4.1%, max −12.3%, streaming −2.6%,
  save +8.9% — noise of the same magnitude in both directions; PR1–PR3 do
  not touch the per-tick item path). 0-item overhead: 0.12 µs/frame on both
  commits.
- **Scaling**: ~0.117 µs/item/tick (linear); 1024-cap item step **0.12 ms**
  (GCC) / 0.20 ms (MSVC) — ~0.7% / 1.2% of a 60 Hz frame budget; ~7.5
  collision cells/item/tick.
- **Allocations**: 0/tick steady state (all static scenarios); pickup burst
  0.17/tick (bounded event plumbing); streaming 484/tick = terrain
  generation, not items.
- **Pickup burst**: 1024 hotbar-first merges in one 0.125 ms tick (GCC) /
  0.246 ms (MSVC); full conservation.
- **Save/reload @1024 items**: 69,866 bytes; save 0.40 ms (GCC) / 4.25 ms
  (MSVC); reopen dominated by terrain bootstrap (27/62 ms); verified.
- **Checkpoint overhead (measured)**: canonical fixture 802 B checkpoints
  (2 retained generations + manifests) vs 340 B live set (~2.4×, bounded by
  retention, independent of world size); staging memory = one payload set +
  one file-copy buffer.
- GPU item rendering: validation-clean at the 1024 cap (one instanced draw);
  GPU timings are the water-audit/visual-harness domain, not re-measured here.

## 5. Remaining risks / recommended follow-ups

1. **Power-loss durability is best-effort** (ADR §2.9): the guarantee is
   process-crash consistency. Follow-up issue: full fsync discipline
   (file + directory) on the live SaveService writes and a Windows directory
   flush strategy if power-loss guarantees are required.
2. **Crash recovery discards uncommitted mid-session progress by design**:
   in checkpoint mode, edits flushed after the last commit but before a crash
   are discarded at recovery (the live set is re-seeded from the generation).
   This is the transactional semantics that eliminates mixed generations —
   legacy mode previously reopened with mixed state. Documented; hosts that
   want finer recovery granularity should call `savePlayerState` at their
   chosen checkpoints.
3. **Retention = 2 generations**: corrupt latest + corrupt previous →
   explicit unrecoverable refusal (no silent fresh fallback). Follow-up:
   optional deeper retention / backup rotation policy.
4. **Stacked PRs**: CI triggers on `main`-targeted PRs; retarget each PR as
   its base merges (the stack order is #238 → #239 → #240 → #241 → #242 →
   #243 → #244).
5. **GPU coverage is one canonical machine/driver**: other GPUs run smoke
   mode; the optional `gpu-validation.yml` workflow needs a self-hosted GPU
   runner to execute in CI (until then: manual dispatch on the canonical
   machine, log committed).
6. **Interactive UI GPU smoke is manual** (harness is ImGui-free by design).
   Follow-up: an ImGui-rendering smoke harness if UI pixel regression becomes
   a concern.
7. **Benchmarks are workers=0 only**: async-worker item timing is not
   measured (digest parity is covered by `HeadlessRunner`). Follow-up: an
   async timing scenario if worker scheduling becomes a performance question.

## 6. Review corrections (post-review pass on #243 / #244)

- **#243 — durability barrier (P1)**: `forceCloseWorldForShutdown` now
  commits a checkpoint ONLY when the async drain AND the chunk flush both
  succeeded; otherwise the commit is skipped with a distinct diagnostic, the
  service is still released unconditionally, and the previous committed
  generation stays authoritative. `closeWorld` / `savePlayerState` were
  audited (already guarded) and are now covered by tests A–D
  (`testCheckpointDurabilityBarrier`, incl. the drain-failure-with-succeeding-
  flush case). Commit `ba83382`.
- **#243 — migration contract (P2)**: transactional guarantees explicitly
  begin at the first successful checkpoint commit; a failed first migration
  recovers via the legacy per-file policy (non-destructive, not a
  transactional snapshot). ADR §2.3/§2.7 + gameplay docs corrected;
  `testFirstMigrationFailureBoundary` covers the boundary deterministically.
- **#244 — GPU workflow trust (P2)**: Option A strict model —
  `workflow_dispatch` honored only on `refs/heads/main`, no PR trigger
  (untrusted/fork code never reaches the persistent runner), minimal
  `contents: read` permissions, `persist-credentials: false`, no secrets, no
  privileged installation (runner prerequisites verified, not installed),
  explicit not-executed reporting. Enforced on every CPU CI run by the new
  `GpuWorkflowTrustGuard` CTest; docs updated. Visual references and the
  Vulkan/visual test steps unchanged.

## 7. Definition of Done

- [x] Invalid raycast inputs are safe (no-hit, bounded, no UB).
- [x] Deferred voxel interactions cannot duplicate or lose resources.
- [x] Gameplay targeting observes coherent authoritative world state.
- [x] Entity IDs cannot overflow or be reused.
- [x] Persistent checkpoints recover to a consistent generation after injected failures (all 10 points).
- [x] Legacy save compatibility verified (unchanged open + rollback shims).
- [x] Item performance scenarios benchmarked and documented (baseline vs current).
- [x] GPU validation procedures implemented and **executed** on the canonical physical GPU.
- [x] CPU-only architecture boundaries intact (CoreIncludeGuard, HeadlessDeps, link-closure assertions, CpuOnlyBuild gate).
- [x] All available required tests pass (CPU Win/Linux, ASan/UBSan, TSan, headless, device, visual smoke + strict).
- [x] Final report with PR links, evidence and remaining risks.
