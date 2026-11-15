# Simulation / render contracts (Epic #204, Phases A–C — issues #205 / #206 / #207)

This is the authoritative design note for the boundary between the
**CPU-owned simulation** (authoritative world + entities + player) and the
**derived render world** (meshes, lighting caches, GPU uploads, camera, debug
UI). It records the dependency direction, component ownership, the value
contracts introduced in Phase A, and the migration checkpoints that the later
child issues must follow.

Phase A **created the boundary; it did not migrate behavior.** Phase B
(issue #206) **moved the authoritative world across it**: `VoxelWorld` +
`WorldChunk` (CPU-only, in `ft_vox_core`) now own voxel storage, residency,
logical edits, generation publication and persistence. Phase C (issue #207)
**moved the derived render pipeline across it**: the renderer-owned
`RenderWorldBridge` now produces meshes, lighting caches, GPU uploads and
draw/shadow lists from the authoritative world, and the temporary
`ChunkManager` render adapter and `ChunkCollisionView` light wrapper were
**removed**. The bridge only *references* the host-owned world — it never owns
simulation state — and the world runs headless with no bridge registered. No
gameplay, terrain, save-format, input or UI behavior changed: the full test
suite and the visual golden comparisons hold (§10,
`benchmarks/issue-206-phase-b.md`, `benchmarks/issue-207-phase-c.md`).

Related documents:

- [`engine-architecture.md`](engine-architecture.md) — engine loop, chunks, streaming, terrain gen
- [`vulkan-graphics.md`](vulkan-graphics.md) — Vulkan device path, pass graph, shaders
- [`terrain-generation.md`](terrain-generation.md) — noise graphs, biome/block catalog
- [`benchmarks/issue-205-pre-refactor.md`](benchmarks/issue-205-pre-refactor.md) — pre-refactor baselines (criterion #5)
- [`benchmarks/issue-206-phase-b.md`](benchmarks/issue-206-phase-b.md) — Phase B verification vs those baselines
- [`benchmarks/issue-207-phase-c.md`](benchmarks/issue-207-phase-c.md) — Phase C verification (render bridge) vs those baselines

---

## 1. Authoritative vs derived

The simulation owns the **authoritative** state. Everything the renderer
consumes is **derived** from it and may be rebuilt, dropped, or recomputed
without changing the world.

| Authoritative (CPU-owned, must survive headless) | Derived (render-only, rebuildable) |
|---|---|
| Voxel occupancy / block types (`WorldChunk` voxel backing, owned by `VoxelWorld`) | Greedy-meshed vertex/index buffers |
| Sparse block edits + their logical revision (`WorldPersistence`, `ChunkEdit`) | Light/smoothed-light caches (`RenderWorldBridge::sampleSmoothedLightUnlocked`) |
| Player state: position, velocity, grounded, medium (`PlayerController`) | GPU uploads / staging / descriptor state |
| Mob state: position, velocity, AI, species (`MobSystem`) | Frustum/visibility culling, draw & shadow lists |
| Save identity: seed, generator version, world name, save revision (`WorldMeta`) | Camera, view/projection, render settings & timing |
| Terrain generation inputs (seed, biome/height/cave noise) | Debug UI panels, biome-map tiles, overlay highlights |
| | Mob **render** snapshots (`MobRenderState` = `sim::MobSnapshot`) |

The mob **simulation** state is authoritative; the per-frame
`MobRenderState` snapshot handed to `MobRenderer` is derived. The contracts
header names the snapshot type `sim::MobSnapshot` to make that direction
explicit.

---

## 2. Dependency arrows

### Current (Phase C, after the render-bridge extraction)

```
                 SDL3 / ImGui / Vulkan (volk, VMA)
                              │
                              ▼
        Engine.cpp/.hpp  ── owns ──►  WorldRenderer, GameUI, ImGuiLayer,
        (main loop, input,            VkContext/Swapchain/FrameContext,
         wiring, camera)              Camera, RenderSettings
              │
              │ owns BOTH (world first, bridge second — the bridge
              │            is destroyed before the world)
              ├──────────────────────────────┐
              ▼                              ▼
        VoxelWorld  ◄── AUTHORITATIVE   RenderWorldBridge  ── DERIVED projection
        (residency/streaming, logical        │ references the world (world()),
         edits + mirrors, gen workers,       │ never owns sim
         WorldPersistence, voxel/border      │ mesh/light dispatch, GPU upload,
         pools, WorldChunk records)          │ visibility, commit groups, deferred
              │                              │ GPU retire, light caches, debug trace
              │ WorldChunk records           │ render handles (Chunk*) bound 1:1
              ▼                              ▼
        WorldCollisionView ◄────────── Chunk  (GPU ranges, draw caches, mesh
        (CPU-only queries)             payloads, light caches, dirty sections,
              ▲                        visibility) — forwards every voxel op
              │                        to its bound WorldChunk
        ChunkMobWorld ─wraps─► WorldCollisionView + bridge light seam
        (temporary mob adapter, Phase D)
              │
              ▼
        ┌─────────────────────────────────────────────────────────────┐
        │  ft_vox_core  (CPU-only static library)                      │
        │  VoxelWorld, WorldChunk, WorldCollisionView,                 │
        │  VoxelPool, BorderPool, TerrainGenerator, TerrainVegetation, │
        │  MobSystem, MobModel, WorldSave, WorldPersistence            │
        │  + Core/SimContracts.hpp, Core/ThreadPool.hpp,               │
        │    Core/WorkloadTelemetry.hpp                                │
        └─────────────────────────────────────────────────────────────┘
```

`ft_vox_core` depends only on GLM, FastNoise2 and `Threads::Threads`. It has
**no** dependency on SDL3, Vulkan, volk, VMA, ImGui or miniz, and never
includes `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`, `ImGui*/` or `Network/`.
The world event stream (`WorldEvent`: Loaded / Unloaded / GenQueued /
Generated / EditApplied) is the CPU→render change channel: the bridge drains
it on the main thread right after each world call and keeps its render handle
map in sync (bind/unbind, generation render flags, edit invalidation). On
construction the bridge also projects any already-resident records, so it can
attach to a live world (Phase E attaches it to a running `GameSimulation`).

The collision seam is CPU-only: `WorldCollisionView` implements
`physics::VoxelCollisionWorld` directly over `VoxelWorld` with the exact
historical collision semantics. Phase C **removed** the `ChunkCollisionView`
light wrapper: physics consumes `WorldCollisionView` directly, and the derived
entity light sampling lives on the bridge (`sampleSmoothedLight`). The
temporary `ChunkMobWorld` adapter wraps `WorldCollisionView` + the bridge light
seam and is removed in Phase D (§10).

### Target (Epic #204, after all child issues land)

```
   SDL3 / ImGui / Vulkan
          │
          ▼
   GameRuntime / graphical adapter        (#209, phase E)
          │  render snapshot + commands
          ▼
   RenderWorldBridge (derived)            (#207, phase C)
          │  reads authoritative state
          ▼
   GameSimulation (CPU)                   (#208, phase D)
   AuthoritativeWorld + PlayerController + MobSystem   (#206, phase B)
          │
          ▼
   ft_vox_core  ──►  ft_vox_headless runner            (#210, phase F)
```

The arrow that matters: **render depends on simulation, never the reverse.**
Phase A establishes the library + contracts that make that arrow enforceable;
phases B–E move the authoritative world, the bridge, the simulation facade and
the graphical runtime across it.

---

## 3. Component ownership and coupling points

| Component | Location | Side | Current coupling | Migrating phase |
|---|---|---|---|---|
| `Engine` | `src/Engine/Engine.cpp/.hpp` | Wiring / adapter | Owns SDL window, Vulkan device/swapchain/frame, `WorldRenderer`, `GameUI`, `ImGuiLayer`, `Camera`, `RenderSettings`, **and** `VoxelWorld` + `RenderWorldBridge` (the bridge references the world), `TerrainGenerator`, `ThreadPool`, `ChunkPool`, `PlayerController`, `MobSystem`. Sim and render are wired in one object. | E (#209) splits graphical runtime from `GameSimulation` |
| `VoxelWorld` | `src/World/VoxelWorld.hpp/.cpp` | **Authoritative (CPU-only)** | Single owner of chunk residency/streaming, the deferred/coalesced logical-edit queue with border mirrors, generation worker publication, `WorldPersistence`, and the voxel/border backing pools. Render-free public API (plain position/forward/settings values); publishes the `WorldEvent` change stream. In `ft_vox_core`. | B (#206) — **landed** |
| `WorldChunk` | `src/World/WorldChunk.hpp/.cpp` | **Authoritative (CPU-only)** | The chunk record: voxel backing, occupancy metadata, border shell, persistent-edit map, `sim::ChunkIdentity` (generation + revision — the single source of the pair), lifecycle state, in-transit flag, per-column generation outputs. In `ft_vox_core`. | B (#206) — **landed** |
| `RenderWorldBridge` | `src/Renderer/RenderWorldBridge.hpp/.cpp` | Derived (renderer-owned projection) | **References** the host-owned `VoxelWorld` (`world()`), never owns sim. Keeps render handle map, mesh/light dispatch, GPU uploads, commit groups, visibility, light-cache intent + sampling, deferred GPU retirement, debug trace. Drains the world event stream and projects already-resident records on attach. Replaces the removed `ChunkManager` adapter. | C (#207) — **landed**; E (#209) attaches it to `GameSimulation` |
| `Chunk` | `src/Chunk/Chunk.hpp` | Derived (render handle) | GPU arena ranges, draw caches, pending mesh payloads, entity light caches, dirty sections, visibility. Binds to a `WorldChunk` (world-owned in game; privately owned for standalone tests/tools) and forwards every voxel read/write. The bridge's per-chunk render record. | C (#207) — **landed** (mesh/light payloads owned by the bridge) |
| `WorldCollisionView` | `src/World/WorldCollisionView.hpp` | **Authoritative query seam (CPU-only)** | Implements `physics::VoxelCollisionWorld` over `VoxelWorld` with the exact historical collision semantics (scoped shared lock, 16-entry cache, boundary envelope, unknown cells, pending-edit overlay). In `ft_vox_core`. | B (#206) — **landed** |
| `ChunkMobWorld` | `src/Chunk/ChunkMobWorld.hpp` | Adapter (seam, temporary) | Implements `entities::MobWorld`; wraps `WorldCollisionView` (CPU) + the bridge light seam (`sampleSmoothedLightUnlocked`) + `TerrainGenerator`; `surface()` uses biome + voxel sampling. Phase C removed the `ChunkCollisionView` wrapper it used to hold. | D (#208) removes it |
| `GameUI` | `src/Engine/GameUI.*` | Derived | ImGui panels; reads sim state + render settings + `WorldPersistence::Status` via a plain frame struct. Render/UI-only. | E (#209) |
| `WorldPersistence` / `SaveService` | `src/World/WorldPersistence.*` | **Authoritative (CPU-only)** | Save facade + async I/O worker; **owned by `VoxelWorld`** (moved out of `ChunkManager` in Phase B — the key inversion the epic required). In `ft_vox_core`. | B (#206) — **landed** |
| `MobSystem` | `src/Entities/MobSystem.*` | **Authoritative (CPU-only)** | Fixed-step 60 Hz passive mobs over `entities::MobWorld`. In `ft_vox_core`. `MobRenderState` snapshots are derived. | D (#208) |
| `PlayerController` | `src/Physics/PlayerController.hpp` | **Authoritative (CPU-only)** | Fixed-step 120 Hz swept-collision player over `physics::VoxelCollisionWorld`. Header-only; in core's include closure. | D (#208) |
| `TerrainGenerator` | `src/Chunk/TerrainGenerator.*` | **Authoritative (CPU-only)** | FastNoise2 biomes/height/caves/vegetation. In `ft_vox_core`. Referenced by `VoxelWorld` (seed + generator version = world identity); the Engine keeps the object's lifetime until Phase D. | B (#206) — **landed** |

The critical inversions the Epic required are **done**: `WorldPersistence`
(authoritative) is owned by `VoxelWorld` (CPU-only), not by a GPU-coupled
manager (Phase B); and the derived render pipeline lives in the
renderer-owned `RenderWorldBridge`, which only *references* the world, while
the temporary `ChunkManager` adapter and `ChunkCollisionView` light wrapper
are **removed** (Phase C). A headless consumer can open, edit, flush, close
and reopen a world with no renderer present (`test_voxel_world`, links only
`ft_vox_core`); the bridge attach/detach/reattach boundary is covered by
`test_render_bridge`.

---

## 4. The CPU-only library: `ft_vox_core`

A named CMake **static library** (preferred over hidden source-list reuse)
holding the translation units that compile independently of the renderer.
Phase A extracted the six already-independent units; Phase B added the
authoritative world and its backing pools:

```
src/World/WorldSave.cpp
src/World/WorldPersistence.cpp
src/World/WorldChunk.cpp          (Phase B)
src/World/VoxelWorld.cpp          (Phase B)
src/Chunk/TerrainGenerator.cpp
src/Chunk/TerrainVegetation.cpp
src/Chunk/VoxelPool.cpp           (Phase B: authoritative backing pool)
src/Chunk/ChunkBorders.cpp        (Phase B: authoritative backing pool)
src/Entities/MobSystem.cpp
src/Entities/MobModel.cpp
```

Phase B also **relocated** two CPU-only headers out of the forbidden `Engine/`
directory so the world can use them: `Engine/ThreadPool.hpp` →
`Core/ThreadPool.hpp` and `Engine/WorkloadTelemetry.hpp` →
`Core/WorkloadTelemetry.hpp` (contents unchanged; all includers updated).

Public properties (propagate to every consumer):

- `target_include_directories(... PUBLIC src/)`
- `target_compile_definitions(... PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)`
- `target_link_libraries(... PUBLIC glm FastNoise2 Threads::Threads)`
- Release codegen matches the **game target** (`/O2` MSVC, `-O3` elsewhere,
  `-march=native` on non-Apple), so the extracted units compile exactly as they
  did in the monolithic game build. This is a **same-target** claim: the CPU
  test targets compile these sources with their own flags (no `-march=native`),
  so cross-target bit-identity is **not** guaranteed — FMA contraction can
  differ on Linux. A canonical shared flag set is deferred to #210.

The game executable compiles `FT_VOX_GAME_SOURCES = FT_VOX_SOURCES −
FT_VOX_CORE_SOURCES` and links `ft_vox_core` **first**, followed by the existing
`FT_VOX_COMMON_LIBS` (SDL3, glm, FastNoise2, miniz, volk, VMA, Threads). The six
core TUs are therefore compiled exactly once (in the library), with no duplicate
symbols.

**Deliberately excluded** — two distinct categories (the include guard, §6,
enforces both):

- **Render-coupled — never:** `Chunk`, `ChunkMobWorld`, `ChunkGpu`,
  `RenderWorldBridge`, and everything under `Renderer/`, `Vulkan/`,
  `Camera/`, `Engine/`, `ImGui*/`, `Network/`. These pull
  Vulkan/VMA/Camera/Engine/Renderer headers and must never enter the core, in
  any phase. (Phase B moved the authoritative voxel state OUT of `Chunk` into
  `WorldChunk`; `Chunk` itself remains a render handle. Phase C removed
  `ChunkManager` / `ChunkCollisionView` entirely.)
- **Render-derived — never (bridge-owned visual work):** `ChunkLightPool`,
  `ChunkLightHalo`, `ChunkPool`, `ChunkMeshResult`, `ChunkMeshScratch`. These
  are **CPU-only** (std/glm/`utils.hpp` includes only) but are owned by the
  render-side `RenderWorldBridge`. Phase C (#207) re-homed the derived
  mesh/light payloads under the bridge and classified them here permanently:
  they are excluded **not** because of GPU coupling but because meshing, light
  caches and draw visibility are visual-only work a headless world never
  schedules. (`VoxelPool` was lifted into the core by Phase B because it is
  authoritative backing, not derived.)

`ChunkBorders` is CPU-only and inside the core closure
(`TerrainGenerator.hpp` includes it); it is **not** excluded. The Vulkan/visual
test targets still compile the core sources directly and do **not** link
`ft_vox_core`, so their build wiring is unchanged (the chunk test targets
additionally compile `FT_VOX_WORLD_SOURCES`).

### `EngineDefs.hpp` — split deferred (not needed in Phase A)

The issue calls for splitting `EngineDefs` "so simulation settings/types do not
pull rendering concerns *if needed*." `src/Engine/EngineDefs.hpp` was examined:
it is render-coupled, holding `ShaderParameters` (atmosphere/lighting/water),
`RenderSettings` (render distance, visibility, VSync, streaming budgets),
`RenderTiming`, `GraphicsQualityPreset`/`PostProcessSettings`, `VoxelHighlight`
and the atmosphere derivation helpers. It is included by `Chunk.hpp`,
`RenderWorldBridge.hpp` and the renderer.

**No split is required in Phase A.** The CPU-only slice does not include
`EngineDefs.hpp` at all — the include guard (§6) forbids `Engine/` and passes,
which proves no core header transitively pulls it in. The simulation types the
core needs (tick rates, `PlayerController`, `MobSettings`, save types) already
live in render-free `Physics/`, `Entities/`, `World/` headers.

A split became necessary in a later phase: `RenderSettings` mixes
**streaming budgets** (`loadPerSec`, `genPerSec`, `meshPerSec`, `uploadPerSec`,
`maxStreamMs`, `streamFrontBias`) with render-only concerns. **Phase B did the
simulation-side half of this split:** `WorldStreamSettings`
(`World/VoxelWorld.hpp`) carries the render-free streaming subset
(`maxRenderDistance`, `minRenderDistance`, `streamFrontBias`) that the world
needs; the adapter converts `RenderSettings` into it, and the per-call integer
budgets stay with the host's frame pacing. `EngineDefs.hpp` itself remains
render-side and is still not included by any core header (the guard proves
it). The remaining budget/UI separation belongs to D/E.

### CPU-only test targets (criterion #2)

`tests/test_core_contracts.cpp` → `test_core_contracts` links **only**
`ft_vox_core` (transitively glm, FastNoise2, Threads). Its link line contains no
SDL3, Vulkan, volk, VMA or miniz. It builds, links and passes without
initializing a GPU or SDL window. Registered as CTest `CoreContracts`.

`tests/test_voxel_world.cpp` → `test_voxel_world` (Phase B) also links **only**
`ft_vox_core` and proves the issue #206 acceptance list end-to-end without a
graphics runtime: headless lifecycle, per-seed byte identity, border/corner
mirrors, deferred + coalesced edits, negative coordinates, unknown terrain,
safe recycling, out-of-order generation completion, stale revision rejection,
the exact collision semantics, and the issue #180 save contracts (restart,
sparse normalization, identity guards, close failure/retry, concurrent edits
during pending async I/O, stranded-edit close refusal). Registered as CTest
`VoxelWorld` (211 checks).

---

## 5. Value contracts: `src/Core/SimContracts.hpp`

A small, render-independent header in namespace `sim` defining **plain values
plus the existing abstract sampling seam** (`WorldQuery` =
`physics::VoxelCollisionWorld`), only where the next phases require them. Names
may evolve in later phases; this header introduces no services, no
`std::function` DI, no event bus.

| Contract | Alias / definition | Backing type |
|---|---|---|
| Tick rates | `kPlayerTickHz = 120.0`, `kMobTickHz = 60.0` | `static_assert` pins `PlayerController::fixedStep == 1/120` |
| Player command | `sim::PlayerCommand` | `physics::PlayerInput` |
| World query | `sim::WorldQuery` | `physics::VoxelCollisionWorld` |
| Voxel medium / cell | `sim::VoxelMedium`, `sim::VoxelCell` | `physics::Medium`, `physics::Cell` |
| Availability vocabulary | `sim::VoxelAvailability{Unknown,Loaded}`, `availabilityOf(cell)` | maps `Cell.available` |
| Sim body / query stats | `sim::SimBody`, `sim::QueryStats` | `physics::Body`, `physics::QueryStats` |
| Revision identity | `sim::ChunkIdentity{chunkX,chunkZ,generation,revision}` | physical incarnation + logical content pair |
| Voxel edit | `sim::VoxelEdit` | `worldsave::ChunkEdit` |
| World identity | `sim::WorldIdentity` | `worldsave::WorldMeta` |
| Mob render snapshot | `sim::MobSnapshot` | `entities::MobRenderState` (derived) |
| Entity id | `sim::EntityId` | `entities::EntityId` (`uint64_t`) |

The header includes only `Physics/VoxelCollision.hpp`,
`Physics/PlayerController.hpp`, `Entities/MobTypes.hpp`, `World/WorldSave.hpp`
and `<cstdint>` — all inside the CPU-only include closure.

`ChunkIdentity` carries the **generation** (physical incarnation, bumped on
pool recycle) and **revision** (logical content, bumped on edit) as a pair so
later phases can reject stale edits/snapshots after a chunk is recycled. This
mirrors the historical `meshGeneration()` / pending-edit matching now carried
by `WorldCollisionView`.

**Duplication resolved (Phase B):** `MeshBuildResult`
(`Chunk/ChunkMeshResult.hpp`) no longer carries its own generation/revision
pair — it holds a `sim::ChunkIdentity` stamped at dispatch from the
`WorldChunk` record, which is the **single source** of the pair.
`Chunk::meshGeneration()/meshRevision()/chunkIdentity()` forward to the
record; `publishMeshResult` validates against it. Phase C's bridge mesh
publish continues to validate against `sim::ChunkIdentity` (no duplicate
pair); `test_render_bridge` drives a stale-generation result and asserts it is
discarded back to its pool.

---

## 6. Include-chain guard (criterion #3)

`tests/check_core_includes.py` (Python 3 stdlib only) performs a DFS over the
transitive `#include` graph of the CPU-only public headers and core TUs and
fails on any forbidden edge:

- **Forbidden directories:** `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`,
  `ImGui/`, `ImGuiFileDialog/`, `Network/`.
- **Chunk/ allowlist (fail-closed):** only the known CPU-only headers
  `LightSample.hpp`, `ChunkBorders.hpp`, `VoxelPool.hpp` (lifted by Phase B),
  `StreamHelpers.hpp`, `BiomeRegionGrid.hpp`, `TerrainGenerator.hpp`,
  `TerrainProfiles.hpp` may be reached. Every other Chunk/ header is a
  violation, in one of two categories:
  - **Render-coupled — never:** `Chunk.hpp`, `ChunkMobWorld.hpp` (pull
    Vulkan/VMA/Camera/Engine/Renderer transitively). Phase C removed
    `ChunkManager.hpp` / `ChunkCollisionView.hpp`.
  - **Render-derived — never (bridge-owned visual work):** `ChunkLightPool.hpp`,
    `ChunkLightHalo.hpp`, `ChunkPool.hpp`, `ChunkMeshResult.hpp`,
    `ChunkMeshScratch.hpp` (CPU-only but owned by the `RenderWorldBridge`;
    meshing/light caches/draw visibility are visual-only, never headless).

  An unlisted Chunk/ header fails as **unclassified** until someone classifies
  it, so a future render-side header cannot slip through unnoticed.
- **Forbidden external headers:** SDL/SDL3, `vulkan/`, `volk.h`,
  `vk_mem_alloc.h`, imgui, glad/GL/GLFW, boost, miniz.

On violation it prints the full include chain and exits non-zero. Registered as
CTest `CoreIncludeGuard`; Python 3 is **REQUIRED** whenever `BUILD_TESTING` is
ON, so the gate cannot silently disappear on a machine without an interpreter.
Run manually:

```bash
python3 tests/check_core_includes.py --verbose
```

The guarded roots are the contracts header, the relocated Core headers
(`ThreadPool`, `WorkloadTelemetry`), every core TU/header (`WorldSave`,
`WorldPersistence`, `WorldChunk`, `VoxelWorld`, `WorldCollisionView`,
`TerrainGenerator`, `TerrainVegetation`, `VoxelPool`, `ChunkBorders`,
`MobSystem`, `MobModel`, `VoxelCollision`, `PlayerController`, `BlockPhysics`,
`MobTypes`, `LightSample`).

---

## 7. Migration checkpoints

Each child issue must leave the game buildable and tests passing. "Create the
boundary before migrating behavior."

| Phase | Issue | Checkpoint | Gate |
|---|---|---|---|
| **A** | **#205** | `ft_vox_core` + `SimContracts.hpp` + include guard + baselines + this doc. No behavior change. | CPU-only target builds/links/passes without GPU/SDL; guard green; game + tests unchanged — **landed (PR #212)** |
| **B** | **#206** | `VoxelWorld` + `WorldChunk` own voxels, residency, edits, generation publication **and `WorldPersistence`**; `WorldCollisionView` carries the collision semantics; `sim::ChunkIdentity` is the single (generation, revision) source (`MeshBuildResult` copy deleted); `Chunk`/`ChunkManager` were derived render adapters (§10). | Headless world load/edit/save without renderer (`VoxelWorld` suite, 211 checks); save identity stable; per-seed fixtures byte-identical; full suite + visual goldens hold — **landed (PR #214)** |
| **C** | **#207 (this)** | `RenderWorldBridge` (renderer-owned) produces derived meshes/lighting/uploads from authoritative state and **references** the host-owned world; mesh publish validates against `sim::ChunkIdentity` (no duplicate pair). **Removed** the `ChunkManager` render adapter and the `ChunkCollisionView` light wrapper; the bridge attaches to a live world and the world runs headless without it. | Render output unchanged (visual goldens identical); bridge consumes the event stream + resident snapshots, never owns sim; `RenderBridge` boundary suite + full suite + goldens hold — **landed** |
| D | #208 | `GameSimulation` facade owns `PlayerController` (120 Hz) + `MobSystem` (60 Hz) + authoritative world. | Fixed-step determinism preserved; no render includes |
| E | #209 | Graphical runtime / SDL3-ImGui-Vulkan adapter drives `GameSimulation` via commands + snapshots. | Game behaves identically; input/UI unchanged |
| F | #210 | `ft_vox_headless` executable links `ft_vox_core` (+ sim) with **no** SDL/Vulkan. | Headless run + save round-trip; remove top-level GPU configure requirement |
| G | #211 | Closure gate: full epic verification. | All invariants hold; baselines compared |

C (#207) and D (#208) may proceed in parallel after B; E (#209) follows both;
F (#210) follows D; G (#211) is last.

---

## 8. Remaining build-system dependency (for Phase F)

The `ft_vox_core` library itself needs only GLM, FastNoise2 and
`Threads::Threads`. However, the **top-level configure** still hard-requires the
graphical stack for *every* configuration — including one that only wants the
CPU library/tests. The remaining dependencies to remove/guard in Phase F are all
in `cmake/Dependencies.cmake`:

| Dependency | Call | Failure mode | Needed by `ft_vox_core`? |
|---|---|---|---|
| Vulkan loader/headers | `find_package(Vulkan REQUIRED)` (line 181) | **fatal** at configure | No |
| volk | CONFIG / FetchContent, `FATAL_ERROR` (line 214) | **fatal** at configure | No |
| SDL3 | CONFIG / pkg-config / FetchContent, `FATAL_ERROR` (line 117) | **fatal** at configure | No |
| Shader compiler (`glslc` / `glslangValidator`) | probe, `FATAL_ERROR` (line 344) | **fatal** at configure | No |
| Vulkan Memory Allocator | CONFIG / header / FetchContent (always resolves) | non-fatal | No |
| miniz | CONFIG / FetchContent | non-fatal | No |

Phase F (#210) must make the four **fatal** discoveries (Vulkan, volk, SDL3,
shader compiler) optional — or scope them to the graphical targets — so
`ft_vox_headless` and the CPU-only tests can configure and build on a machine
with no Vulkan SDK / SDL3 / shader toolchain. VMA and miniz are graphical-only
too but already resolve without a hard failure. These are the **only remaining
build-system dependencies to remove**; they are intentionally left in place in
Phase A to keep the normal configuration unchanged (criterion #4).

---

## 9. Invariants and non-goals

**Invariants (must hold across every phase):**

- Player simulation fixed step **120 Hz**; mob simulation fixed step **60 Hz**.
  Fixed stepping stays internal to the controllers; `maxSteps` catch-up remains
  bounded.
- Direct C++ calls and existing data layouts in tight loops (collision,
  meshing, terrain). No virtualized per-voxel hot path beyond the existing
  `VoxelCollisionWorld` seam.
- Authoritative state is CPU-owned and render-independent; derived state is
  rebuildable.
- Terrain determinism, save format and sparse-override semantics unchanged.

**Non-goals (Epic #204):**

- No universal `IService` / `std::function` dependency injection, no event bus.
- No new ECS, no multiplayer, no gameplay/crafting/inventory features.
- No renderer-quality, terrain, save-format, input or UI changes.
- No requirement of bitwise-identical pixels across GPUs.
- No premature move of GPU-coupled classes into the CPU library.

---

## 10. Phase B/C boundary (issues #206 / #207) — ownership, events, adapter plan

### What moved where

| Authoritative → `VoxelWorld` / `WorldChunk` (CPU-only) | Derived → `RenderWorldBridge` / `Chunk` (render) |
|---|---|
| Chunk record map + active list, load queue, streaming footprint/state, unload scan | Render handle map (`Chunk*` bound 1:1 to records), visibility, draw/shadow lists |
| `VoxelPool` + `BorderPool` backing, record pool (pointer-stable, same back-pressure contract as `ChunkPool`) | `ChunkPool` (render handles), `MeshResultPool`, `ChunkLightPool`, `LightHaloPool` |
| Deferred logical-edit queue: coalescing per (record, coord, kind), target+mirror groups, generation-stamped staleness drop, UNLOADED/in-transit deferral | Geometric commit groups (atomic GPU publication of border-coupled meshes), section dirty masks, `meshNeedsUpdate`, GENERATED/MESHED scheduling |
| Terrain worker dispatch + completion publication, saved-override re-application | Mesh/light worker dispatch, GPU upload phases, deferred GPU retirement |
| `WorldPersistence` (open/close/flush/capture, desired/durable state machine, failure/busy semantics) | Light-cache intent/residency, smoothed-light sampling, chunk lifecycle debug ring |
| `sim::ChunkIdentity` (generation + revision) — single source | `MeshBuildResult.identity` / `GpuReplacement` stamps (captures validated at publish) |

`Chunk` keeps its historical public surface but forwards every voxel
read/write to its bound `WorldChunk`; standalone chunks (tests/tools) own a
private record created lazily with the constructor's pools on first standalone
use, so the single-object API and move semantics are unchanged while an idle
(bound or free-list) handle carries no per-column backing. `Chunk::reset` acts
on the active record (owned or bound; a bound reset must keep the coordinate so
the world's map cannot desync); pool acquire/release only ever see unbound
handles.

### The world event stream (CPU → render change channel)

`VoxelWorld` emits main-thread `WorldEvent`s — `Loaded`, `Unloaded`,
`GenQueued`, `Generated`, `EditApplied` — drained via `takeEvents()`. The
bridge drains immediately after each world call and:

- **Loaded** → acquire a render handle, bind it to the record, register it;
- **Unloaded** → unbind, remove, park for frame-aware GPU retirement (the
  world already captured the record's edits and recycled it);
- **Generated** → publish the render flags the pre-split `generateTerrain()`
  set inline (`meshNeedsUpdate`, full dirty mask) + cross-chunk light-arrival
  invalidation;
- **EditApplied** → section invalidation + GENERATED re-arm + light-aware
  neighbor dirtying (the pre-split `queueOrApplyEdit` render side effects).

The stream is bounded (16384 events) so a headless consumer that never drains
cannot grow it without limit; functional events are never dropped at frame
cadence. Headless consumers may ignore it entirely. Because the world does not
re-emit `Loaded` for already-resident records, the bridge's constructor also
projects the current residency (`attachToWorld`), so it can attach to a live
world — the seam Phase E uses to attach a bridge to a running `GameSimulation`.

### Publication ordering (preserved exactly)

`RenderWorldBridge::processFinishedJobs()` stages the pump so the issue #114
ordering holds: **(1)** world generation publication → **(2)** render
mesh/light publication (the superseded-by-edit checks still see the pending
queue) → **(3)** `applyPendingEdits()` → render edit invalidation. The
persistence durability barrier (`closeWorld`/destructor) quiesces render and
world jobs together before the world captures/flushes/shuts down; a failed
flush or stranded edits keep persistence OPEN and retryable (issue #180
contracts unchanged).

### Locks

The world `shared_mutex` guards authoritative state; the bridge render
`shared_mutex` guards the handle map and light caches. `WorldCollisionView`
holds the world lock for its lifetime (scoped-view contract: no edits/unloads
during a view); the temporary `ChunkMobWorld` adapter additionally holds the
render lock for light reads (world → render). Mesh dispatch acquires the
reverse order (render → world, via `ensureShellPopulated`). Both orders are
safe ONLY because every acquisition is main-thread; workers take only the
completion-queue and pool mutexes, so the two map locks never contend across
threads. Phase D/E must revisit this before threading either path.

### Temporary adapters — removal status

Per the issue's architectural constraints, these facades were **documented and
must be removed before Epic #204 completes**:

1. **`ChunkManager` render adapter** — **REMOVED in Phase C (#207).** Mesh/light
   dispatch, GPU uploads, commit groups, visibility and the debug trace moved
   into the renderer-owned `RenderWorldBridge`, which references (never owns)
   the world. Phase E (#209) lifts `VoxelWorld` ownership from the Engine into
   `GameSimulation`; the bridge's `world()` accessor is the read seam.
2. **`ChunkCollisionView` light wrapper** — **REMOVED in Phase C (#207).**
   Physics consumes `WorldCollisionView` (CPU-only) directly; the derived
   light-sampling seam lives on the bridge (`sampleSmoothedLight`).
3. **`ChunkMobWorld` mob adapter** — still present (wraps `WorldCollisionView`
   + the bridge light seam); **Phase D (#208)** removes it when mobs move behind
   `GameSimulation`.
4. **`Chunk` render handle** — Phase C re-homed the mesh/light payloads under
   the bridge; the handle is now the bridge's per-chunk render record. The
   authoritative `WorldChunk` is unaffected.

### Verification

`benchmarks/issue-206-phase-b.md` and `benchmarks/issue-207-phase-c.md` record
the post-refactor comparisons against the Phase A baselines: full CTest suite,
the `VoxelWorld` and `RenderBridge` CPU-only suites, per-seed byte identity,
persistence/streaming/mob/physics benchmarks, the in-game benchmark smoke run,
and the visual golden comparison (only the pre-existing `cave_emissive` driver
mismatch, #213, with identical metrics).
