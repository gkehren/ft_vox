# Issue #210: Phase F verification (Epic #204 — headless simulation runner)

Verification that the standalone CPU simulation runner (`ft_vox_headless`)
and the CPU-only CMake configuration prove the Epic #204 boundary: the
authoritative simulation is reusable by a process with **no SDL3, Vulkan,
ImGui, camera, renderer or GPU resource pool**, and the CPU slice configures
and builds with the graphics stack **not discovered at all**. No simulation,
terrain, save-format or gameplay behavior changed.

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
§7 (checkpoint F), §8 (resolved build dependency) and §10 (Phase F contract),
and [`../headless-runner.md`](../headless-runner.md) for the runner reference.

## What changed

- **`ft_vox_headless`** (`src/Headless/`: `HeadlessMain`, `HeadlessRunner`,
  `SimScript`, `SimDigest`) — a graphical-free host of the SAME
  `sim::GameSimulation` facade the Engine hosts. Links **only `ft_vox_core`**.
  Explicit headless interest source (plain point + radius) drives residency;
  fixed per-frame streaming budgets (8 loads / 8 generations); deterministic
  synchronous generation (`--workers 0`) or real async dispatch with a
  bounded per-frame quiesce (`--workers N`). Canonical FNV-1a state digest,
  machine-readable SUMMARY, distinct error exit codes (2/3/4/5/6).
- **`VoxelWorld::generatePendingVoxelsSync`** — the synchronous counterpart
  of `generatePendingVoxels` (same nearest-first selection, saved-override
  re-application and GenQueued/Generated events; main-thread only; registered
  records are never recycled on allocation failure). The only core addition;
  no existing behavior touched.
- **CMake CPU-only path** — `FT_VOX_GRAPHICS` option (default ON). With OFF,
  `cmake/Dependencies.cmake` runs **no** discovery for SDL3/Vulkan/volk/VMA/
  miniz/shader compilers, and the shader/game/graphical-test targets are not
  defined. `FT_VOX_PORTABLE_FP` (default OFF) selects the canonical shared FP
  flag set deferred from Phase A (no `-march=native`, `-ffp-contract=off` /
  `/fp:precise`). Configure-time transitive link-closure assertion keeps the
  runner graphics-free in both configurations.
- **Guards / tests** — `tests/check_headless_deps.py` (CTest `HeadlessDeps`:
  include closure + real link artifacts + cache discovery check),
  `tests/test_headless_runner.py` (CTest `HeadlessRunner`), `HeadlessSmoke`,
  and `cmake/CheckCpuOnlyBuild.cmake` (CTest `CpuOnlyBuild`: a separate clean
  `FT_VOX_GRAPHICS=OFF` configure/build/test run). Canonical fixture:
  `tests/fixtures/sim-actions.txt`.
- **Docs** — `docs/headless-runner.md` (new), updates to
  `simulation-render-contracts.md`, `engine-architecture.md`, `building.md`,
  `AGENTS.md`, `README.md`.

## Environment

- **Windows:** AMD Ryzen 7 9800X3D, Windows 11 Pro, MSVC (VS 2022, MSBuild
  18.3), vcpkg toolchain for the full configuration; the CPU-only gate uses
  **no** vcpkg toolchain. Same reference system as the Phase A–E baselines.
- **Linux:** WSL2 Debian, GCC 14.2.0, CMake 3.31.6, Ninja, Python 3.14 —
  configured with `FT_VOX_GRAPHICS=OFF` and **no** Vulkan SDK / SDL3 / shader
  toolchain discovery (GLM/FastNoise2 reused from the parent build's
  FetchContent sources).

## Acceptance results

### 1. Clean CPU-only configure/build, graphics not discovered or linked

- **WSL2 Debian (GCC 14, Ninja):** `cmake -DFT_VOX_GRAPHICS=OFF
  -DFT_VOX_DEP_MODE=system` configured successfully; `CMakeCache.txt` carries
  **zero** `Vulkan_* / SDL3_* / volk_* / VulkanMemoryAllocator_* / miniz_* /
  GLSLC / GLSLANG_VALIDATOR / FT_VOX_VMA_*` entries. All CPU targets built.
- **Windows (MSVC):** `HeadlessDeps` passes — the runner's include closure is
  render-free, its `.vcxproj` link fields (`AdditionalDependencies` +
  `ProjectReference`) contain only `ft_vox_core.lib`, `FastNoise.lib` and
  Windows system libs, and the configure-time link-closure assertion holds.
- **`CpuOnlyBuild` gate:** separate clean configure/build/test run (see below).

### 2. Fixed-seed scripted determinism

`ft_vox_headless --seed 42 --ticks 600 --script tests/fixtures/sim-actions.txt`
run twice → identical SUMMARY and digest **`3a068dd4be8ab57e`** (Windows,
Release). The async-worker runs (`--workers 2` and `--workers 4`) produce the
**same** digest as the synchronous run — worker scheduling never leaks into
authoritative state (per-frame quiesce). Cross-platform FP limits documented
in `headless-runner.md` §5; `FT_VOX_PORTABLE_FP` enables cross-machine
comparison within a platform family.

### 3. Save → process exit → reload → edit verification

Three separate processes in an isolated temporary saves root:
place+save (exit 0, `save=ok`, `world.meta` + `player.state` written) →
reload+verify+delete+save (exit 0) → reload+verify revert (exit 0). All pass.

### 4. Load/unload, pending async jobs, failure/teardown

- Interest-source churn (pin probe far → unload spawn region → follow →
  reload) exits 0 in both sync and async modes.
- Run timeout (`--ticks 1000000 --timeout 1`) exits 6 with bounded teardown.
- Failure exit codes verified: invalid/missing script (3), script exceeding
  `--ticks` (3), `expect voxel` mismatch (5), `--expect-digest` mismatch (5),
  seed-mismatch world open (4), unknown option (2).

### 5. Integration tests: timeouts + deterministic end conditions, no GPU

Every runner invocation has a fixed tick budget and a wall-clock timeout;
the driver wraps each process in a 180 s backstop. No test needs a GPU,
window or shader asset.

## Test runs

### Windows — headless acceptance driver

`python tests/test_headless_runner.py --runner build/Release/ft_vox_headless.exe
--fixtures tests/fixtures` → **31 checks, 0 failures** (smoke, sync/async
determinism, expect-digest gate, 3-process save/reload, all failure exit
codes, timeout, JSON, interest churn sync+async).

`python tests/check_headless_deps.py --build-dir build` → **OK** (graphics-free).
`python tests/check_core_includes.py` → **OK** (core boundary intact).

<!-- FULL-SUITE-RESULTS -->

### Windows — full CTest suite (graphical configuration, Release)

`ctest -C Release --output-on-failure --timeout 300 -E CpuOnlyBuild` →
**44 / 45 passed**. The full game (`ft_vox`) and every graphical test built
and ran: `EngineReload`, `RenderBridge`, `WorldPersistence`, `ChunkLifecycle`,
`MeshUploadLifecycle`, `PostResizeFormats`, `MobVulkanRender`, the Vulkan
resource/validation probes, etc. The new headless tests pass in the full
configuration too: `HeadlessSmoke` (0.29 s), `HeadlessRunner` (7.29 s,
31 checks), `HeadlessDeps` (0.13 s).

The single failure is `VisualRegression` → scene **`cave_emissive`**:
`reference mismatch: mean_abs=0.041943 rms=0.070555 hot=0.800885`. This is the
**pre-existing driver mismatch (#213)** already recorded in
[`issue-209-phase-e.md`](issue-209-phase-e.md) with identical metrics — every
other scene passes. Phase F touches **no** renderer, shader, camera or
render-bridge code (the only core change is the new, game-uncalled
`VoxelWorld::generatePendingVoxelsSync`), so the rendered output is unchanged
from the base commit; this is not a Phase F regression.

<!-- WSL-CPU-RESULTS -->

### WSL2 Debian (GCC 14) — clean CPU-only configure/build/test

Configured with `-G Ninja -DFT_VOX_GRAPHICS=OFF -DFT_VOX_DEP_MODE=system
-DCMAKE_BUILD_TYPE=Release` (GLM/FastNoise2 reused from the parent build's
FetchContent sources). `CMakeCache.txt` carries **no** graphics discovery
entries. All 86 targets built; `ctest --output-on-failure --timeout 300`:

```text
100% tests passed, 0 tests failed out of 25
  HeadlessSmoke ......... Passed    0.14 sec
  HeadlessRunner ........ Passed    3.47 sec   (31-check acceptance driver)
  HeadlessDeps .......... Passed    1.09 sec
  CpuOnlyBuild .......... Passed   91.67 sec   (nested clean configure/build/test)
  (+ CoreContracts, VoxelWorld, GameSimulation, CoreIncludeGuard, PlayerPhysics,
   TerrainGeneration, WorldSave, PassiveMobs, pools, telemetry, … all Passed)
Label Time Summary: cpu-only = 91.67 sec*proc (1 test)
```

This proves acceptance criteria 1–5 on Linux with **no Vulkan SDK, SDL3 or
shader toolchain discovered**: the runner, the CPU tests and the dependency
guards all build and pass, and the `CpuOnlyBuild` gate independently
re-configures/builds/tests a fresh CPU-only tree.

<!-- CPU-ONLY-GATE-RESULTS -->

### Clean CPU-only configure/build gate (`CpuOnlyBuild`)

The gate performs a **separate fresh** `FT_VOX_GRAPHICS=OFF` configure (no
vcpkg toolchain), builds every CPU target, and runs the nested CPU-only CTest
suite — independently of the parent build:

| Platform | Result | Time |
|---|---|---|
| Windows / MSVC (no vcpkg) | **Passed** | 148.30 s |
| WSL2 Debian / GCC 14 / Ninja | **Passed** | 91.67 s |

Both nested caches carry `FT_VOX_GRAPHICS:BOOL=OFF` and **zero** graphics
discovery entries. The Windows nested runner (built without vcpkg) reproduces
the exact digest `3a068dd4be8ab57e` from the full-config build — the package
manager and graphics stack have no influence on authoritative simulation
output.

## Conclusion

All issue #210 acceptance criteria are met on **Windows/MSVC** and
**Linux/GCC**:

1. ✅ Clean CPU-only configure/build produces the runner + CPU tests with
   graphics dependencies **not discovered or linked** (cache + link-artifact
   + include-closure guards; `CpuOnlyBuild` gate on both platforms).
2. ✅ Fixed-seed scripted simulation twice → identical authoritative digest;
   sync == async; cross-platform FP limits documented (`FT_VOX_PORTABLE_FP`).
3. ✅ Save → process exit → reload → edit verification in an isolated temp dir
   (3 processes, both platforms).
4. ✅ Chunk load/unload/reload churn, pending async jobs and failure/teardown
   paths exit cleanly; bounded timeout (exit 6), no stalls.
5. ✅ Integration tests use timeouts + deterministic end conditions; no
   GPU/hardware dependence.
6. ✅ Documentation gives copy/paste Linux, Windows and macOS smoke commands
   (`docs/headless-runner.md`, `docs/building.md`).

The graphical game and full test suite are unchanged (44/45; the sole failure
is the pre-existing `cave_emissive` driver mismatch #213, unrelated to Phase F).
