# Simulation / render contracts (Epic #204, Phase A — issue #205)

This is the authoritative design note for the boundary between the
**CPU-owned simulation** (authoritative world + entities + player) and the
**derived render world** (meshes, lighting caches, GPU uploads, camera, debug
UI). It records the dependency direction, component ownership, the value
contracts introduced in Phase A, and the migration checkpoints that the later
child issues must follow.

Phase A **creates the boundary; it does not migrate behavior.** No gameplay,
graphical, terrain, save-format, input or UI behavior changes in this phase
(acceptance criterion #6). The game and all affected tests build unchanged on
the normal configuration (criterion #4). The only structural change is that
already-independently-compilable CPU translation units are extracted into a
named static library, `ft_vox_core`, and a small render-independent contracts
header is added.

Related documents:

- [`engine-architecture.md`](engine-architecture.md) — engine loop, chunks, streaming, terrain gen
- [`vulkan-graphics.md`](vulkan-graphics.md) — Vulkan device path, pass graph, shaders
- [`terrain-generation.md`](terrain-generation.md) — noise graphs, biome/block catalog
- [`benchmarks/issue-205-pre-refactor.md`](benchmarks/issue-205-pre-refactor.md) — pre-refactor baselines (criterion #5)

---

## 1. Authoritative vs derived

The simulation owns the **authoritative** state. Everything the renderer
consumes is **derived** from it and may be rebuilt, dropped, or recomputed
without changing the world.

| Authoritative (CPU-owned, must survive headless) | Derived (render-only, rebuildable) |
|---|---|
| Voxel occupancy / block types (`Chunk` voxel backing) | Greedy-meshed vertex/index buffers |
| Sparse block edits + their logical revision (`WorldPersistence`, `ChunkEdit`) | Light/smoothed-light caches (`ChunkManager::sampleSmoothedLightUnlocked`) |
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

### Current (Phase A, after extraction)

```
                 SDL3 / ImGui / Vulkan (volk, VMA)
                              │
                              ▼
        Engine.cpp/.hpp  ── owns ──►  WorldRenderer, GameUI, ImGuiLayer,
        (main loop, input,            VkContext/Swapchain/FrameContext,
         wiring, camera)              Camera, RenderSettings
              │
              │ owns
              ▼
        ChunkManager  ── owns ──►  WorldPersistence (CPU-only facade)
        (streaming, GPU             ChunkPool, ThreadPool, chunk map,
         upload, light cache)       pendingEdits, smoothed-light cache
              │
              │ voxel backing + edits
              ▼
   ChunkCollisionView ─implements─► physics::VoxelCollisionWorld   (adapter)
   ChunkMobWorld      ─implements─► entities::MobWorld            (adapter)
              │                              │
              │                              ▼
              │                    PlayerController (120 Hz), MobSystem (60 Hz)
              ▼
        ┌─────────────────────────────────────────────┐
        │  ft_vox_core  (CPU-only static library)      │
        │  TerrainGenerator, TerrainVegetation,        │
        │  MobSystem, MobModel,                        │
        │  WorldSave, WorldPersistence                 │
        │  + Core/SimContracts.hpp (value contracts)   │
        └─────────────────────────────────────────────┘
```

`ft_vox_core` depends only on GLM, FastNoise2 and `Threads::Threads`. It has
**no** dependency on SDL3, Vulkan, volk, VMA, ImGui or miniz, and never
includes `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`, `ImGui*/` or `Network/`.

The two adapters (`ChunkCollisionView`, `ChunkMobWorld`) are the **seam**:
they implement render-independent physics/mob interfaces
(`physics::VoxelCollisionWorld`, `entities::MobWorld`) but currently reach the
voxels through the GPU-coupled `ChunkManager` (they `#include
<Chunk/ChunkManager.hpp>`). They are deliberately **not** in `ft_vox_core` yet.

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
| `Engine` | `src/Engine/Engine.cpp/.hpp` | Wiring / adapter | Owns SDL window, Vulkan device/swapchain/frame, `WorldRenderer`, `GameUI`, `ImGuiLayer`, `Camera`, `RenderSettings`, **and** `ChunkManager`, `TerrainGenerator`, `ThreadPool`, `ChunkPool`, `PlayerController`, `MobSystem`. Sim and render are wired in one object. | E (#209) splits graphical runtime from `GameSimulation` |
| `ChunkManager` | `src/Chunk/ChunkManager.hpp` | Mixed | Distance streaming + async gen/mesh + **GPU upload** + smoothed-light cache; owns `WorldPersistence`, `ChunkPool`, `ThreadPool`, chunk map, `pendingEdits`. GPU-coupled. | B (#206) extracts authoritative world; C (#207) extracts derived bridge |
| `Chunk` | `src/Chunk/Chunk.hpp` | Mixed | Flat `uint8_t` voxel backing (authoritative) **plus** greedy mesh + `VkBuffer`/VMA GPU resources (derived). GPU-coupled. | B/C split voxel storage from mesh/GPU |
| `ChunkCollisionView` | `src/Chunk/ChunkCollisionView.hpp` | Adapter (seam) | Implements `physics::VoxelCollisionWorld`; main-thread scoped lock + chunk cache over `ChunkManager`; `sampleLight` reads the derived light cache. Includes `ChunkManager.hpp`. | B (#206) re-point at authoritative world |
| `ChunkMobWorld` | `src/Chunk/ChunkMobWorld.hpp` | Adapter (seam) | Implements `entities::MobWorld`; wraps `ChunkCollisionView` + `TerrainGenerator`; `surface()` uses biome + voxel sampling. | B (#206) re-point at authoritative world |
| `GameUI` | `src/Engine/GameUI.*` | Derived | ImGui panels; reads sim state + render settings + `WorldPersistence::Status` via a plain frame struct. Render/UI-only. | E (#209) |
| `WorldPersistence` / `SaveService` | `src/World/WorldPersistence.*` | **Authoritative (CPU-only)** | Save facade + async I/O worker; **currently owned by `ChunkManager`** (`chunkManager->worldPersistence()`). In `ft_vox_core`. | B (#206) moves ownership to authoritative world |
| `MobSystem` | `src/Entities/MobSystem.*` | **Authoritative (CPU-only)** | Fixed-step 60 Hz passive mobs over `entities::MobWorld`. In `ft_vox_core`. `MobRenderState` snapshots are derived. | D (#208) |
| `PlayerController` | `src/Physics/PlayerController.hpp` | **Authoritative (CPU-only)** | Fixed-step 120 Hz swept-collision player over `physics::VoxelCollisionWorld`. Header-only; in core's include closure. | D (#208) |
| `TerrainGenerator` | `src/Chunk/TerrainGenerator.*` | **Authoritative (CPU-only)** | FastNoise2 biomes/height/caves/vegetation. In `ft_vox_core`. | B (#206) |

The critical inversion the Epic requires: **`WorldPersistence` (authoritative)
is today owned by `ChunkManager` (GPU-coupled).** Phase B must move save
ownership into the authoritative world so the headless runner can persist
without any renderer. Phase A documents this coupling; it does not move it.

---

## 4. The CPU-only library: `ft_vox_core`

A named CMake **static library** (preferred over hidden source-list reuse)
holding the translation units that already compile independently of the
renderer:

```
src/World/WorldSave.cpp
src/World/WorldPersistence.cpp
src/Chunk/TerrainGenerator.cpp
src/Chunk/TerrainVegetation.cpp
src/Entities/MobSystem.cpp
src/Entities/MobModel.cpp
```

Public properties (propagate to every consumer):

- `target_include_directories(... PUBLIC src/)`
- `target_compile_definitions(... PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)`
- `target_link_libraries(... PUBLIC glm FastNoise2 Threads::Threads)`
- Release codegen matches the game (`/O2` MSVC, `-O3` elsewhere, `-march=native`
  on non-Apple) so extracted objects are byte-identical to the previous
  monolithic build.

The game executable compiles `FT_VOX_GAME_SOURCES = FT_VOX_SOURCES −
FT_VOX_CORE_SOURCES` and links `ft_vox_core` **first**, followed by the existing
`FT_VOX_COMMON_LIBS` (SDL3, glm, FastNoise2, miniz, volk, VMA, Threads). The six
core TUs are therefore compiled exactly once (in the library), with no duplicate
symbols.

**Deliberately excluded** (GPU-coupled; do not move prematurely):
`Chunk`, `ChunkManager`, `ChunkPool`, `VoxelPool`, `ChunkLightPool`,
`ChunkGpu`, `ChunkMeshResult`, `ChunkCollisionView`, `ChunkMobWorld`,
`ChunkBorders`, everything under `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`,
`ImGui*/`, `Network/`. The Vulkan/visual test targets still compile the core
sources directly and do **not** link `ft_vox_core`, so their build wiring is
unchanged.

### `EngineDefs.hpp` — split deferred (not needed in Phase A)

The issue calls for splitting `EngineDefs` "so simulation settings/types do not
pull rendering concerns *if needed*." `src/Engine/EngineDefs.hpp` was examined:
it is render-coupled, holding `ShaderParameters` (atmosphere/lighting/water),
`RenderSettings` (render distance, visibility, VSync, streaming budgets),
`RenderTiming`, `GraphicsQualityPreset`/`PostProcessSettings`, `VoxelHighlight`
and the atmosphere derivation helpers. It is included by `Chunk.hpp`,
`ChunkManager.hpp` and the renderer.

**No split is required in Phase A.** The CPU-only slice does not include
`EngineDefs.hpp` at all — the include guard (§6) forbids `Engine/` and passes,
which proves no core header transitively pulls it in. The simulation types the
core needs (tick rates, `PlayerController`, `MobSettings`, save types) already
live in render-free `Physics/`, `Entities/`, `World/` headers.

A split becomes necessary in a later phase: `RenderSettings` mixes
**streaming budgets** (`loadPerSec`, `genPerSec`, `meshPerSec`, `uploadPerSec`,
`maxStreamMs`, `streamFrontBias`) with render-only concerns. When
`GameSimulation` (D, #208) or the authoritative world (B, #206) needs those
budgets without render settings, the streaming subset must be separated into a
render-free header. That is recorded here as a migration checkpoint, not done
now (criterion #6: no behavior change).

### CPU-only test target (criterion #2)

`tests/test_core_contracts.cpp` → `test_core_contracts` links **only**
`ft_vox_core` (transitively glm, FastNoise2, Threads). Its link line contains no
SDL3, Vulkan, volk, VMA or miniz. It builds, links and passes without
initializing a GPU or SDL window. Registered as CTest `CoreContracts`.

---

## 5. Value contracts: `src/Core/SimContracts.hpp`

A small, render-independent header in namespace `sim` defining **plain-value**
contracts only where the next phases require them. Names may evolve in later
phases; this header introduces no services, no `std::function` DI, no event bus.

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
mirrors the existing `meshGeneration()` / pending-edit matching in
`ChunkCollisionView`.

---

## 6. Include-chain guard (criterion #3)

`tests/check_core_includes.py` (Python 3 stdlib only) performs a DFS over the
transitive `#include` graph of the CPU-only public headers and core TUs and
fails on any forbidden edge:

- **Forbidden directories:** `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`,
  `ImGui/`, `ImGuiFileDialog/`, `Network/`.
- **Forbidden basenames (GPU-coupled chunk headers):** `Chunk.hpp`,
  `ChunkManager.hpp`, `ChunkGpu.hpp`, `ChunkMeshResult.hpp`,
  `ChunkCollisionView.hpp`, `ChunkMobWorld.hpp`, `ChunkPool.hpp`,
  `VoxelPool.hpp`, `ChunkLightPool.hpp`.
- **Forbidden external headers:** SDL/SDL3, `vulkan/`, `volk.h`,
  `vk_mem_alloc.h`, imgui, glad/GL/GLFW, boost.

On violation it prints the full include chain and exits non-zero. Registered as
CTest `CoreIncludeGuard` (skipped with a status message if no Python 3
interpreter is found). Run manually:

```bash
python3 tests/check_core_includes.py --verbose
```

The guarded roots are the contracts header plus every core TU/header
(`WorldSave`, `WorldPersistence`, `TerrainGenerator`, `TerrainVegetation`,
`MobSystem`, `MobModel`, `VoxelCollision`, `PlayerController`, `BlockPhysics`,
`MobTypes`, `LightSample`).

---

## 7. Migration checkpoints

Each child issue must leave the game buildable and tests passing. "Create the
boundary before migrating behavior."

| Phase | Issue | Checkpoint | Gate |
|---|---|---|---|
| **A** | **#205 (this)** | `ft_vox_core` + `SimContracts.hpp` + include guard + baselines + this doc. No behavior change. | CPU-only target builds/links/passes without GPU/SDL; guard green; game + tests unchanged |
| B | #206 | Authoritative world owns voxels, edits, **and `WorldPersistence`**; adapters re-point off `ChunkManager`. | Headless world load/edit/save without renderer; save identity stable |
| C | #207 | `RenderWorldBridge` produces derived meshes/lighting/uploads from authoritative state. | Render output unchanged; bridge consumes snapshots, never owns sim |
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
