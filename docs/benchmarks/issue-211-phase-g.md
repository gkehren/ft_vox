# Issue #211: Phase G closure gate (Epic #204 — final verification)

The epic's closure gate: enforce the CPU/GPU boundary with repeatable automated
checks, prove behavior and performance parity against the pre-epic baseline,
remove transitional debt, and finalize documentation. This document records the
final verification; the per-phase records are
[`issue-205-pre-refactor.md`](issue-205-pre-refactor.md) (baseline) and
[`issue-206-phase-b.md`](issue-206-phase-b.md) …
[`issue-210-phase-f.md`](issue-210-phase-f.md).

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
for the boundary design and the A–G checkpoints.

## What Phase G changed

- **Hardened dependency-direction enforcement** (acceptance #1, "not only a
  comment"):
  - The configure-time transitive **link-closure** assertion
    (`ft_vox_assert_cpu_link_closure`) now covers `ft_vox_core` **itself**,
    `ft_vox_headless`, and **every CPU-only test target** — not just the runner.
    Any SDL3/Vulkan/volk/VMA/miniz/ImGui in a CPU target's transitive link
    closure fails the configure, in either configuration.
  - `tests/check_headless_deps.py` (CTest `HeadlessDeps`) now scans the real
    link artifacts of **both** `ft_vox_core` and `ft_vox_headless`
    (Ninja/Makefile/MSVC), is fail-closed on a configured tree, and checks the
    include closure + CPU-only cache discovery.
- **Added a reproducible performance-parity protocol driver**
  (`tests/bench_parity_211.py`): repeated **interleaved** in-game `--benchmark`
  trials (baseline/current), per-metric median + per-trial appendix, consistent
  -degradation detection above a noise band (investigate, don't hide).
- **Cleanup audit** (see below): transitional adapters confirmed removed; no
  dead includes of removed headers; Engine ownership carries no authoritative
  gameplay state.
- **Documentation**: executable/module graph + enforcement-gate table added to
  `engine-architecture.md` §1; this closure record; contracts §7 checkpoint G.

No simulation, terrain, save-format, renderer or gameplay behavior changed.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8c/16t), Windows 11 Pro, NVIDIA RTX 4070 Ti
  (Vulkan 1.4) — the same reference system as the Phase A–F baselines.
- **Build:** MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`, `glslc`
  `D:/VulkanSDK/1.3.290.0/Bin/glslc.exe`; GLM/FastNoise2 FetchContent.
- **Pre-epic baseline:** `4f931d6` (direct parent of #205), built in a separate
  worktree (`ft_vox_baseline_204`) with the **same** flags/toolchain; the
  benchmark/profile binaries were built from that tree.
- **Linux cross-check:** WSL2 Debian, GCC 14.2, CMake 3.31, Ninja.

## 1. Dependency direction — automated enforcement

| Gate | Enforces | Repeatability |
|---|---|---|
| `ft_vox_assert_cpu_link_closure` (CMake) | transitive link closure of `ft_vox_core`, `ft_vox_headless`, all CPU tests is graphics-free | every configure |
| `CoreIncludeGuard` | core public-header/TU include closure render-free (Chunk/ allowlist fail-closed) | CTest |
| `HeadlessDeps` | core **and** runner include closure + real link artifacts + CPU-only cache discovery | CTest |
| `FT_VOX_GRAPHICS=OFF` / `CpuOnlyBuild` | clean CPU-only configure/build/test, graphics stack not discovered | configure / opt-in CTest |

All green on Windows/MSVC and WSL2 GCC. The graphical target depends on the
simulation; the simulation never depends on the render side.

## 2. Headless + graphical consume the same algorithms

`ft_vox` (Engine host) and `ft_vox_headless` (HeadlessRunner host) both drive the
**same** `sim::GameSimulation` facade — one owner of authoritative state, no
copied player/mob/world algorithm. The CPU profiles below are bit-identical
between the pre-epic and post-refactor trees, confirming the algorithms survived
the migration unchanged.

## 3. Performance parity (the protocol)

### In-game benchmark — 5 interleaved trials (`--seed 42 --benchmark 20`)

Driver: `tests/bench_parity_211.py`; full table + per-trial appendix in
[`issue-211-parity.md`](issue-211-parity.md). Median of 5 interleaved trials:

| Metric | Baseline | Current | Delta |
|---|---:|---:|---:|
| Score / Grade | 9902 / **S** | 9904 / **S** | parity |
| Frame p50 (ms) | 1.462 | 1.519 | +3.9% (noise; trial 4 current faster) |
| Frame p95 (ms) | 3.945 | 3.855 | **−2.3%** |
| Frame p99 (ms) | 4.708 | 4.704 | −0.1% |
| Avg FPS | 530.6 | 537.9 | +1.4% |
| 1% low FPS | 212.4 | 212.6 | +0.1% |
| Frames > 33.3 ms | 0 | 0 | parity |
| GPU avg / p95 (ms) | 1.23 / 1.705 | 1.187 / 1.681 | −3.4% / −1.4% |
| TerrainGen (ms/chunk) | 2.469 | 2.469 | 0.0% |
| MeshBuild (ms/mesh) | 4.058 | 4.096 | +1.0% |
| Peak chunks | 3313 | 3260 | −1.6% |
| Queue peaks Gen/Mesh | 4 / 5 | 3 / 5 | parity |
| Resident voxel bytes (peak) | 1.512e8 | 1.486e8 | −1.7% |
| GPU live bytes (peak) | 6.04e8 | 6.04e8 | 0.0% |

**Verdict: no consistent regression above the 5% noise band across 5 interleaved
trials.** Frame tail (p95/p99), average, FPS, 1% low, GPU time and resident
memory are parity-or-better; the deterministic workload character (chunks drawn,
meshes built, queue peaks, resident bytes) is unchanged. The CPU streaming scope
shows a small median increase (~11%, event-boundary cost) that does **not**
affect GPU-bound frame time.

### CPU simulation profiles — bit-identical determinism

| Metric | Baseline | Current | Deterministic output |
|---|---:|---:|---|
| Terrain gen (ms/chunk, `--profile 32 1337`) | 2.411 | 2.449 | +1.5% (noise) |
| Mob update mean (ms, 48 mobs) | 0.03484 | 0.03481 | cells/tick **1596**, travelled **2753.91**, dropped **0** — identical |
| Player physics avg (ms, real voxels) | 0.000634 | 0.000609 | cells/tick **39.9123**, travelled **682**, dropped **0** — identical |

The mob/physics deterministic outputs (cells sampled, distance travelled,
dropped steps) are **bit-identical**; timing deltas are sub-microsecond noise.

### Synthetic streaming/edit loop — investigated, explained, tracked

`bench_persistence` (200 edits/s, view 96, pure CPU, no GPU render) shows a
**consistent ~+42% CPU streaming-scope cost** (cool-machine, counterbalanced:
baseline ~3.85 ms → current ~5.45 ms frame avg). Investigated per the protocol
(investigate, don't hide):

- **Real, not thermal:** stable after a 75 s cool-down with current-first
  ordering; baseline steady at ~3.85 ms. (Phase B had documented the
  non-alternating thermal-drift trap; this measurement controls for it.)
- **Not edit-specific:** present with zero edits (~1.25 → ~1.78 ms), so it lives
  in the streaming/generation/mesh-dispatch path.
- **Equivalent scope:** the harness streaming block is byte-identical between
  versions.
- **Source:** the CPU→render **event-stream boundary** the epic required —
  world event emission + `takeEvents()` deque→vector copy + per-event render-lock
  dispatch (bind/unbind handles, publish flags, invalidate sections) + the
  render-handle indirection (`ensureShellPopulated` world-lock per mesh dispatch).
  The pre-epic `ChunkManager` did this inline. The debug trace is **not** the
  cause (`m_chunkTraceEnabled` defaults false).
- **Bounded impact:** in-game frame time is at parity (GPU-bound); both fit far
  within a 60 fps budget (3.85/5.45 ms ≪ 16.7 ms); CPU sim algorithms
  bit-identical. This is an **explained** architectural trade-off, not an
  unexplained regression.

Tracked for boundary-preserving optimization in
**[#219](https://github.com/gkehren/ft_vox/issues/219)** (reusable drain buffer,
redundant-drain consolidation, batched shell-population locks). It is not a
correctness/data-loss/stale/race issue and does not block closure.

## 4. World invariants

Covered by the existing CPU/render suites (all pass): `VoxelWorld` (211 checks:
incarnation/revision validation, deferred-edit coalescing, unknown terrain,
cross-chunk border mirrors, stale-job rejection, stream unload/recycle,
persistence flush/close refusal), `RenderBridge` (geometry commit groups, epoch
stamp), `GameSimulation` (reload/save/restore, placement rule), `WorldPersistence`
(issue #180 contracts), `ChunkLifecycle`, `MeshFaceKey`, `MeshUploadLifecycle`.

## 5. Graphical / render parity

### Windows full CTest suite (Release, validation on)

`ctest -C Release --output-on-failure -E CpuOnlyBuild` → **44 / 45 PASS**.
The sole failure is `VisualRegression` → scene **`cave_emissive`**:

```
mean_abs=0.041943 rms=0.070555 hot=0.800885
```

**Identical** to the Phase A–F baselines — the pre-existing driver-vs-golden
mismatch tracked in [#213](https://github.com/gkehren/ft_vox/issues/213). Every
other golden scene passes (water/shore/cave/underwater, god-ray alignment,
auto-exposure meter + adaptation, SSAO, cascade transition, midnight/noon/
sunset, mob lighting, AA silhouette, resize-check). Phase G touches **no**
renderer/shader/camera/Vulkan code, so rendered output is unchanged from `main`.

Covered graphical/render contracts (all pass):

| Suite | Parity evidence |
|---|---|
| `EngineReload` | world/seed reload through the epoch barrier; no stale draw/shadow/mob snapshot or async job survives; bridge epoch == sim epoch |
| `RenderBridge` | derived projection references the world; geometry commit groups; `sim::ChunkIdentity` publish validation; epoch stamp |
| `MeshUploadLifecycle` | staging-full deferral keeps the exact result; retry without rebuild; partial rollback; old GPU mesh untouched |
| `MeshArena` / `PostResizeFormats` | arena suballocation/retirement; swapchain format/color-space resize keeps pipeline layouts valid |
| `MobVulkanRender` | mob pipelines, texture rollback, frame slots |
| `VulkanResourceSmoke` / validation probe | device path + validation error reporting |
| `DebugUiCore` / `UiShell` / `InputRouting` / `FramePacing` | UI/player snapshot, input routing, VSync pacing |

In-game benchmark resource health: `staging.failures=0`, `upload.deferred=0`,
`pool.rejected=0`, `mesh.pool.growEvents=0`, stable arena page count — no
persistent pool growth or staging exhaustion. No new implicit GPU waits (the
epoch barrier keeps the single pre-existing device-idle + retire flush around
the bridge swap; per-frame uploads stay inside `recordFrame`).

### Linux (WSL2 Debian, GCC 14) — CPU-only

`cmake -DFT_VOX_GRAPHICS=OFF -DFT_VOX_DEP_MODE=system` (Ninja, no Vulkan SDK /
SDL3 / shader toolchain discovered; GLM/FastNoise2 reused from FetchContent
sources). `ctest --output-on-failure`:

```text
100% tests passed, 0 tests failed out of 25
  HeadlessSmoke ......... Passed    0.14 sec
  HeadlessRunner ........ Passed    3.73 sec   (38-check acceptance driver)
  HeadlessDeps .......... Passed    1.08 sec   (core + runner link artifacts)
  CpuOnlyBuild .......... Passed   93.12 sec   (nested clean configure/build/test)
  (+ CoreContracts, VoxelWorld, GameSimulation, CoreIncludeGuard, PlayerPhysics,
   TerrainGeneration, WorldSave, PassiveMobs, pools, telemetry, … all Passed)
```

The CPU-only boundary checks run **without a GPU**. The full graphical Linux
build was author-verified in Phase E (WSL2 + llvmpipe + userspace `glslc`); its
two device-dependent failures (`WorldPersistence` GCC FMA sparse-diff,
`VisualRegression` on llvmpipe) are pre-existing platform issues verified on the
base commit, not Phase G regressions — cross-GPU pixel identity is an explicit
epic non-goal. Device-only GPU tests skip (77) where no Vulkan device exists.

**Cross-generator guard fix (Phase G):** extending `HeadlessDeps` to scan
`ft_vox_core` initially failed on Ninja because a static-library link edge
carries no `LINK_LIBRARIES` variable (it archives objects; dependencies appear
on the build statement line). The scan now reads the whole link edge (build
line + block), covering executables and static libraries across Ninja /
Makefile / MSVC. Verified green on both Windows/MSVC and WSL2/Ninja.

## 6. Cleanup audit

- **Transitional adapters removed:** `ChunkManager`, `ChunkCollisionView`,
  `ChunkMobWorld` exist only as historical comments — zero code references, zero
  includes of removed headers (`grep` verified).
- **Engine ownership:** `Engine` owns only graphical resources + host-owned
  `ThreadPool`/`ChunkPool` + `sim::GameSimulation` + derived `RenderWorldBridge`;
  no authoritative gameplay state, no unused old ownership members.
- **No duplicated simulation path:** both executables consume the one facade.
- **Dead includes/link deps:** none found for removed headers; the CPU link
  closure is asserted clean (core links only glm + FastNoise2 + Threads).

## 7. Child-issue acceptance

| Phase | Issue | Acceptance |
|---|---|---|
| A | #205 | CPU-only target + contracts + include guard + baselines — met |
| B | #206 | authoritative world owns voxels/edits/persistence; headless world suite — met |
| C | #207 | derived render bridge references world; adapters removed; goldens hold — met |
| D | #208 | GameSimulation facade; headless sim suite; rates preserved — met |
| E | #209 | graphical host; epoch barrier; placement query; full suite — met |
| F | #210 | headless runner; CPU-only configure; determinism; save/reload — met |
| G | #211 | this gate: enforcement + parity + cleanup + docs — met |

## 8. Epic definition of done

- [x] CPU simulation link graph does not transitively depend on Vulkan/SDL/ImGui/VMA; builds without a Vulkan SDK (configure-time link-closure assertion + `CpuOnlyBuild`).
- [x] Graphical and headless executables consume the same world/physics/mob algorithms (one facade; bit-identical CPU profiles).
- [x] Fixed-seed scripted headless runs reproduce logical results (Phase F digest determinism, sync == async).
- [x] Solo interaction, UI, streaming, edit persistence, pause/focus, reload, benchmarking, animation, lighting working (full suite + visual goldens).
- [x] Interleaved async meshing/edit/unload + revision rules safe; no use-after-free/stale publish (VoxelWorld/RenderBridge/EngineReload suites).
- [x] GPU visual regression + Vulkan validation green where supported (canonical GPU; pre-existing #213 `cave_emissive` mismatch unchanged).
- [x] Linux, Windows/MSVC, macOS build paths preserved; CPU-only checks run without a GPU (documented skips for absent devices).
- [x] Docs current: engine-architecture, AGENTS, README/building, CMake comments, contracts.
- [x] Performance: no **unexplained** sustained regression (in-game parity; the explained streaming-scope cost tracked in #219).

## Conclusion

Phase G closes Epic #204. The CPU/GPU boundary is **enforced by repeatable
automated checks** (configure-time transitive link-closure assertion on the
core, the runner and every CPU test; `CoreIncludeGuard`; `HeadlessDeps` scanning
core + runner include closure and real link artifacts; the `FT_VOX_GRAPHICS=OFF`
clean-configure gate) — not merely documented. The graphical and headless
executables consume the **same** world/physics/mob algorithms (one
`GameSimulation` facade; CPU profiles bit-identical).

**Performance:** the in-game benchmark is at parity (Grade **S**, 5 interleaved
trials; frame tail / GPU / resident memory parity-or-better). The CPU simulation
profiles are bit-identical deterministic. The synthetic `bench_persistence`
streaming/edit loop shows a consistent, **explained** ~+42% CPU streaming-scope
cost from the required event-stream boundary — investigated per the protocol
(real, not thermal; not edit-specific; equivalent measurement scope; source
identified), with **no user-facing frame-time regression** (GPU-bound; both fit
far within a 60 fps budget). It is tracked for boundary-preserving optimization
in **#219**, not hidden. No unexplained sustained regression remains.

**Verification:** Windows/MSVC full suite **44/45** (sole failure = pre-existing
`cave_emissive` #213, identical metrics; no renderer code touched); WSL2 GCC
CPU-only **25/25** including the clean-configure gate. All world-invariant,
graphical-parity and render/Vulkan suites pass; validation layers enabled.

**Cleanup:** transitional adapters (`ChunkManager`, `ChunkCollisionView`,
`ChunkMobWorld`) confirmed removed (comments only); no dead includes of removed
headers; Engine carries no authoritative gameplay state; no duplicated
simulation path.

**Docs:** executable/module graph + enforcement-gate table
(`engine-architecture.md` §1), this closure record, the parity protocol output,
contracts §7 checkpoint G, AGENTS/README/building updates.

All child issues (#205–#210) meet their acceptance criteria; the epic definition
of done is satisfied. The one discovered performance trade-off is explained and
tracked (#219); it is not a correctness/data-loss/stale/race regression and does
not block closure.
