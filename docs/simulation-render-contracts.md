# Simulation / render contracts (Epic #204, Phases A–G — issues #205 / #206 / #207 / #208 / #209 / #210 / #211)

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
**removed**. Phase D (issue #208) **moved the simulation across it**: the
CPU-only `sim::GameSimulation` facade now owns the authoritative world, the
120 Hz `PlayerController` and the 60 Hz `MobSystem`, with explicit
commands/lifecycle and read-only value snapshots; the temporary
`ChunkMobWorld` mob adapter was **removed** (its CPU seam is
`WorldMobView`, and derived entity light sampling stays on the bridge,
consumed by the host after the mob snapshot). Phase E (issue #209)
**completed the graphical host integration**: `Engine` is a pure
SDL3/Vulkan/ImGui host of `GameSimulation` — documented frame orchestration,
a single host suspension policy, the player-aware placement query in
the simulation (`canPlaceVoxel`, which hosts gate edits with), and an explicit
world-generation epoch barrier
(`Engine::resetWorldProjection` + the bridge's epoch stamp) so no stale
render snapshot or async job survives a world reload. The bridge only
*references* the simulation-owned world — it never owns simulation state —
and the simulation runs headless with no bridge registered. Phase F
(issue #210) **proved the boundary with a standalone executable**:
`ft_vox_headless` hosts the same `GameSimulation` facade with no SDL3,
Vulkan, ImGui, camera or renderer, and a `FT_VOX_GRAPHICS=OFF` CMake
configuration builds the CPU library + runner + CPU tests with the graphics
stack **not discovered at all** (see [`headless-runner.md`](headless-runner.md)).
No gameplay,
terrain, save-format, input or UI behavior changed: the full test suite and
the visual golden comparisons hold (§10,
`benchmarks/issue-206-phase-b.md`, `benchmarks/issue-207-phase-c.md`,
`benchmarks/issue-208-phase-d.md`, `benchmarks/issue-209-phase-e.md`,
`benchmarks/issue-210-phase-f.md`).

Epic #223 (persistent gameplay loop) builds on these contracts. Its Phase A
(issue #224) **extended the CPU core with the item domain** — `src/Items/`
(`ItemId` / `ItemStack` / `ItemRegistry` / `Inventory` + pure stack math),
contracts-only with no behavior change, exactly like Phase A of Epic #204
created the boundary before migrating behavior (§5, "Item domain"). Phase B
(issue #225) **integrated it into the simulation**: `GameSimulation` owns the
inventory and exposes the first gameplay commands + `InventorySnapshot`,
still with no host behavior change (§10, "Phase B inventory contract").
Phase C (issue #226) **closed the survival-lite loop inside the simulation**:
`ItemEntitySystem` (dropped items, 60 Hz), `breakBlock`/`dropFromSlot`
commands, deterministic auto-pickup, id-sorted snapshots + a spawn/remove
event stream (§10, "Phase C item-entity contract"). Phase D (issue #227)
**moved the last interaction rules into the simulation and rewired the
host**: `sim::raycastVoxel` (the historical march, verbatim),
`placeSelectedBlock` (consume-on-success), Engine input → commands, one
bridge publication path (§10, "Phase D interaction contract"). Phase E
(issue #228) **persisted the loop**: `inventory.state` + `items.state`
(the exact `WorldSave` discipline), full gameplay capture/restore on the
`player.state` lifecycle seams, and entity-id allocator durability
(§10, "Phase E gameplay-persistence contract"). Phase F (issue #229) added the
**derived presentation**: hotbar HUD, inventory panel and the
`ItemEntityRenderer` (snapshot-driven, command-only mutations, epoch-barrier
reset; §10, "Phase F gameplay-presentation contract"). Phase G (issue #230)
**closed the epic**: the headless script grammar drives the full gameplay
loop, the canonical digest (`FTVHDIG2`) covers inventory + item population +
id allocator, and the closure gate (full suite, CPU-only build, goldens,
benchmark parity) is recorded in `benchmarks/issue-230-closure.md` (§10, "Phase G
headless gameplay + closure contract"; [`gameplay-loop.md`](gameplay-loop.md)
is the authoritative gameplay reference).

Related documents:

- [`gameplay-loop.md`](gameplay-loop.md) — the authoritative Epic #223 gameplay model (items, commands, entities, persistence, replication readiness)
- [`headless-runner.md`](headless-runner.md) — the Phase F standalone simulation runner (CLI, scripts, digest, CPU-only build)
- [`engine-architecture.md`](engine-architecture.md) — engine loop, chunks, streaming, terrain gen
- [`vulkan-graphics.md`](vulkan-graphics.md) — Vulkan device path, pass graph, shaders
- [`terrain-generation.md`](terrain-generation.md) — noise graphs, biome/block catalog
- [`benchmarks/issue-205-pre-refactor.md`](benchmarks/issue-205-pre-refactor.md) — pre-refactor baselines (criterion #5)
- [`benchmarks/issue-206-phase-b.md`](benchmarks/issue-206-phase-b.md) — Phase B verification vs those baselines
- [`benchmarks/issue-207-phase-c.md`](benchmarks/issue-207-phase-c.md) — Phase C verification (render bridge) vs those baselines
- [`benchmarks/issue-208-phase-d.md`](benchmarks/issue-208-phase-d.md) — Phase D verification (simulation facade) vs those baselines

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
| Inventory: 36 slots + hotbar selection (`items::Inventory`, Epic #223) | Hotbar HUD / inventory panel presentation, UI-local drag cursor (`GameUI`) |
| Dropped-item entities: id, stack, body, age, pickup delay + id allocator (`items::ItemEntitySystem`) | Item **render** snapshots (`ItemEntitySnapshot`), `ItemEntityRenderer` instances, atlas icon descriptors |
| Gameplay persistence: `inventory.state` (FTVI) + `items.state` (FTVJ) incl. the allocator high-water (`WorldPersistence`) | |
| | Mob **render** snapshots (`MobRenderState` = `sim::MobSnapshot`) |

The mob **simulation** state is authoritative; the per-frame
`MobRenderState` snapshot handed to `MobRenderer` is derived. The contracts
header names the snapshot type `sim::MobSnapshot` to make that direction
explicit.

---

## 2. Dependency arrows

### Current (Epic #223 Phase G, after the gameplay loop landed)

```
                 SDL3 / ImGui / Vulkan (volk, VMA)
                              │
                              ▼
        Engine.cpp/.hpp  ── owns ──►  WorldRenderer, GameUI, ImGuiLayer,
        (graphical HOST: main loop,   VkContext/Swapchain/FrameContext,
         input mapping, camera,       Camera, RenderSettings
         pause/focus/benchmark policy,
         world-epoch barrier)
              │
              │ owns BOTH (simulation first, bridge second — the bridge
              │            is destroyed before the simulation)
              ├──────────────────────────────┐
              ▼                              ▼
        sim::GameSimulation ◄─ AUTHORITATIVE  RenderWorldBridge ─ DERIVED projection
        (CPU facade: owns TerrainGenerator,   │ references sim->world(),
         VoxelWorld, PlayerController 120 Hz, │ stamped with the sim's world
         MobSystem 60 Hz, Inventory +         │ epoch at attach; mesh/light
         ItemEntitySystem 60 Hz; lifecycle,   │ dispatch, GPU upload,
         spawn/restore, flight, commands      │ visibility, commit groups,
         incl. the player-aware placement     │ deferred GPU retire, light
         rule, value snapshots)               │ caches, debug trace
              │                               │
              │ owns                          ▼
              ▼                         Chunk  (GPU ranges, draw caches, mesh
        VoxelWorld                             payloads, light caches, dirty sections,
        (residency/streaming, logical          visibility) — forwards every voxel op
         edits + mirrors, gen workers,         to its bound WorldChunk
         WorldPersistence, voxel/border
         pools, WorldChunk records)
              │
              │ WorldChunk records
              ▼
        WorldCollisionView ◄── physics (CPU-only queries)
        WorldMobView ─wraps─► WorldCollisionView + TerrainGenerator
        (CPU-only mob seam: collision + surface; NO render light)
              │
              ▼
        ┌─────────────────────────────────────────────────────────────┐
        │  ft_vox_core  (CPU-only static library)                      │
        │  GameSimulation, VoxelWorld, WorldChunk, WorldCollisionView, │
        │  WorldMobView, VoxelPool, BorderPool, TerrainGenerator,      │
        │  TerrainVegetation, MobSystem, MobModel, WorldSave,          │
        │  WorldPersistence, Items (ItemRegistry, ItemStack,           │
        │  Inventory, ItemEntitySystem)                                │
        │  + Core/SimContracts.hpp, Core/ThreadPool.hpp,               │
        │    Core/WorkloadTelemetry.hpp                                │
        └─────────────────────────────────────────────────────────────┘
```

The derived item presentation hangs off the same arrow: the host fills
`ItemEntityRenderer` from the id-sorted `sim::ItemEntitySnapshot`s (the mob
snapshot pattern) and the hotbar HUD / inventory panel from
`sim::InventorySnapshot` — both are rebuildable views, never authoritative
(Epic #223 Phases F/G; [`gameplay-loop.md`](gameplay-loop.md) §7).

`ft_vox_core` depends only on GLM, FastNoise2 and `Threads::Threads`. It has
**no** dependency on SDL3, Vulkan, volk, VMA, ImGui or miniz, and never
includes `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`, `ImGui*/` or `Network/`.
The world event stream (`WorldEvent`: Loaded / Unloaded / GenQueued /
Generated / EditApplied) is the CPU→render change channel: the bridge drains
it on the main thread right after each world call and keeps its render handle
map in sync (bind/unbind, generation render flags, edit invalidation). On
construction the bridge also projects any already-resident records, so it can
attach to a live world — the seam the graphical host uses after every world
transition (Phase E): the fresh bridge is stamped with the simulation's
`worldGenerationId()` epoch at attach.

The collision seam is CPU-only: `WorldCollisionView` implements
`physics::VoxelCollisionWorld` directly over `VoxelWorld` with the exact
historical collision semantics. Phase C **removed** the `ChunkCollisionView`
light wrapper: physics consumes `WorldCollisionView` directly. Phase D
**removed** the `ChunkMobWorld` mob adapter: mobs run behind `GameSimulation`
over the CPU-only `WorldMobView` (collision + biome/grass surface scan), and
the derived entity light sampling stays on the bridge
(`sampleSmoothedLight`), consumed by the graphical host AFTER taking the mob
snapshots — no path nests the world and render locks anymore.

### Target (Epic #204, after all child issues land)

```
   SDL3 / ImGui / Vulkan
          │
          ▼
   Engine graphical host                  (#209, phase E — landed)
          │  commands + snapshots
          ├────────────────────────────┐
          ▼                            ▼
   GameSimulation (CPU)          RenderWorldBridge (derived)   (#207, phase C)
   AuthoritativeWorld +          references sim->world(),
   PlayerController + MobSystem  mesh/light/uploads/visibility
   (#208 facade, #206 world)
          │
          ▼
   ft_vox_core  ──►  ft_vox_headless runner            (#210, phase F — landed)
```

The arrow that matters: **render depends on simulation, never the reverse.**
Phase A establishes the library + contracts that make that arrow enforceable;
phases B–E move the authoritative world, the bridge, the simulation facade and
the graphical runtime across it. The epic's suggested `GameRuntime` layer is
realized by the `Engine` host itself (issue #209 kept the wiring in one host
object — no extra adapter indirection was needed once the facade and bridge
existed); the headless runner (#210) consumes the same facade directly —
**landed**: `ft_vox_headless` links only `ft_vox_core`, and the
`FT_VOX_GRAPHICS=OFF` configuration builds it without discovering the
graphics stack at all ([`headless-runner.md`](headless-runner.md)).

---

## 3. Component ownership and coupling points

| Component | Location | Side | Current coupling | Migrating phase |
|---|---|---|---|---|
| `Engine` | `src/Engine/Engine.cpp/.hpp` | Graphical host / adapter | Owns SDL window, Vulkan device/swapchain/frame, `WorldRenderer`, `GameUI`, `ImGuiLayer`, `Camera`, `RenderSettings`, `ThreadPool`, `ChunkPool`, **and** `sim::GameSimulation` + `RenderWorldBridge` (the bridge references the sim's world). Maps SDL input to commands, owns the single suspension policy (`simulationSuspended`), the day-cycle presentation parameter and the world-epoch barrier (`resetWorldProjection`); `run()` is the documented 16-step frame orchestration. No authoritative gameplay state lives here. | E (#209) — **landed** |
| `GameSimulation` | `src/Sim/GameSimulation.hpp/.cpp` | **Authoritative (CPU-only)** | The Phase D facade: owns the shared world authority (`TerrainGenerator`, `VoxelWorld`, `MobSystem` 60 Hz, **`items::ItemEntitySystem` 60 Hz, Epic #223**) plus per-player sessions (`PlayerController` 120 Hz + **`items::Inventory`**, flight mode, block selection, spawn/restore) indexed by `sim::PlayerId` — the Epic #245 Phase A split (#246, §11); the local player anchors the legacy solo APIs as thin forwards. Reload counter, explicit lifecycle (initialize/bootstrap/finalize/reload/save/shutdown — gameplay state captured/restored on the `player.state` seams), per-frame advance with plain values (dt, commands, observer), read-only bounded snapshots (`PlayerSnapshot`, `MobSnapshot`, `WorldSnapshot`, `InventorySnapshot`, `ItemEntitySnapshot`). Phase E added the player-aware placement query (`canPlaceVoxel`; hosts gate edits with it, `placeVoxel` does not call it). Epic #223 added the gameplay commands (hotbar/inventory, `grantItems`, `breakBlock`, `dropFromSlot`, `placeSelectedBlock`, `raycastVoxel`) with explicit result vocabularies. Epic #245 A2 (#247, §12) made the interactions **actor-aware** (per-player break/place/drop/targeting, all-body placement rule, deterministic ascending-`PlayerId` shared pickup, `kInteractionReach` targeting); legacy solo forms remain thin forwards. Render-free; in `ft_vox_core`. | D (#208) — **landed**; E (#209) placement query; Epic #223 B–E (#225–#228); Epic #245 A1 (#246) player sessions, A2 (#247) actor-aware interactions |
| `VoxelWorld` | `src/World/VoxelWorld.hpp/.cpp` | **Authoritative (CPU-only)** | Single owner of chunk residency/streaming, the deferred/coalesced logical-edit queue with border mirrors, generation worker publication, `WorldPersistence`, and the voxel/border backing pools. Render-free public API (plain position/forward/settings values); publishes the `WorldEvent` change stream. In `ft_vox_core`; owned by `GameSimulation` since Phase D. | B (#206) — **landed** |
| `WorldChunk` | `src/World/WorldChunk.hpp/.cpp` | **Authoritative (CPU-only)** | The chunk record: voxel backing, occupancy metadata, border shell, persistent-edit map, `sim::ChunkIdentity` (generation + revision — the single source of the pair), lifecycle state, in-transit flag, per-column generation outputs. In `ft_vox_core`. | B (#206) — **landed** |
| `RenderWorldBridge` | `src/Renderer/RenderWorldBridge.hpp/.cpp` | Derived (renderer-owned projection) | **References** the simulation-owned `VoxelWorld` (`world()`), never owns sim. Keeps render handle map, mesh/light dispatch, GPU uploads, commit groups, visibility, light-cache intent + sampling, deferred GPU retirement, debug trace. Drains the world event stream and projects already-resident records on attach. Stamped with the host's world epoch (`worldGenerationId()`) at attach. Replaces the removed `ChunkManager` adapter. | C (#207) — **landed**; E (#209) epoch stamp + host attach |
| `Chunk` | `src/Chunk/Chunk.hpp` | Derived (render handle) | GPU arena ranges, draw caches, pending mesh payloads, entity light caches, dirty sections, visibility. Binds to a `WorldChunk` (world-owned in game; privately owned for standalone tests/tools) and forwards every voxel read/write. The bridge's per-chunk render record. | C (#207) — **landed** (mesh/light payloads owned by the bridge) |
| `WorldCollisionView` | `src/World/WorldCollisionView.hpp` | **Authoritative query seam (CPU-only)** | Implements `physics::VoxelCollisionWorld` over `VoxelWorld` with the exact historical collision semantics (scoped shared lock, 16-entry cache, boundary envelope, unknown cells, pending-edit overlay). In `ft_vox_core`. | B (#206) — **landed** |
| `WorldMobView` | `src/World/WorldMobView.hpp` | **Authoritative mob seam (CPU-only)** | Implements `entities::MobWorld` over `WorldCollisionView` + `TerrainGenerator`; `surface()` keeps the exact biome + grass eligibility scan. Replaces the removed `ChunkMobWorld` adapter; derived entity light sampling is NOT here (the host samples the bridge after the mob snapshot). In `ft_vox_core`'s include closure. | D (#208) — **landed** |
| `GameUI` | `src/Engine/GameUI.*` | Derived | ImGui panels; reads sim snapshots + render settings + `WorldPersistence::Status` via a plain frame struct (`GameUIFrame`). Biome-map requests/results are validated against the world generation + seed. Epic #223 F (#229) added the gameplay surfaces (`GameUIInventory.cpp` + `InventoryUi.hpp`): always-on hotbar HUD + `E` inventory panel — snapshot-only displays issuing command callbacks; the drag cursor is UI-local presentation discarded on close; item icons are per-atlas-layer ImGui descriptors. Render/UI-only. | E (#209) — **landed**; Epic #223 F (#229) gameplay surfaces |
| `WorldPersistence` / `SaveService` | `src/World/WorldPersistence.*` | **Authoritative (CPU-only)** | Save facade + async I/O worker; **owned by `VoxelWorld`** (moved out of `ChunkManager` in Phase B — the key inversion the epic required). Epic #223 Phase E (#228) added the gameplay state files `inventory.state` (FTVI) + `items.state` (FTVJ, incl. the entity-id allocator high-water) with the exact `WorldSave` discipline (atomic writes, strict validation, missing-fresh/corrupt-loud). In `ft_vox_core`. | B (#206) — **landed**; Epic #223 E (#228) gameplay files |
| `MobSystem` | `src/Entities/MobSystem.*` | **Authoritative (CPU-only)** | Fixed-step 60 Hz passive mobs over `entities::MobWorld`. In `ft_vox_core`; owned by `GameSimulation` since Phase D. `MobRenderState` snapshots are derived. | D (#208) — **landed** |
| `items::Inventory` | `src/Items/Inventory.*` | **Authoritative (CPU-only)** | Fixed 36-slot container (hotbar 0..8) + selected hotbar index; deterministic slot operations with `items::ItemResult` codes; trivially copyable, no heap churn. Owned by `GameSimulation` (Epic #223 B); mutations through commands only; `restore` is a lifecycle-only persistence seam. In `ft_vox_core`. | Epic #223 A/B (#224/#225) — **landed** |
| `items::ItemEntitySystem` | `src/Items/ItemEntitySystem.*` | **Authoritative (CPU-only)** | Dropped-item entities on the `MobSystem` pattern: fixed 60 Hz step, gravity + `physics::move` through `WorldCollisionView`, pickup-delay + proximity auto-pickup (hotbar-first, partial remainder stays), age despawn + population cap (oldest-first), monotonic never-reused id allocator (high-water persisted), id-sorted bounded snapshots + bounded spawn/remove event stream. Owned by `GameSimulation` (Epic #223 C); `restore` is lifecycle-only. In `ft_vox_core`. | Epic #223 C/E (#226/#228) — **landed** |
| `ItemEntityRenderer` | `src/Renderer/ItemEntityRenderer.*` | Derived (renderer-owned) | One shared atlas-textured unit cube, one instanced indexed draw for the frustum-culled visible population (bounded by the sim cap); host-side previous/current interpolation, per-instance atlas layer + bridge-sampled local light; receives directional shadows, casts none (v1); recorded in the opaque pass after mobs; holds no authoritative state (epoch-barrier reset). | Epic #223 F (#229) — **landed** |
| `PlayerController` | `src/Physics/PlayerController.hpp` | **Authoritative (CPU-only)** | Fixed-step 120 Hz swept-collision player over `physics::VoxelCollisionWorld`. Header-only; in core's include closure; owned by `GameSimulation` since Phase D. | D (#208) — **landed** |
| `TerrainGenerator` | `src/Chunk/TerrainGenerator.*` | **Authoritative (CPU-only)** | FastNoise2 biomes/height/caves/vegetation. In `ft_vox_core`. Referenced by `VoxelWorld` (seed + generator version = world identity); **owned by `GameSimulation`** since Phase D. | B (#206) — **landed** |

The critical inversions the Epic required are **done**: `WorldPersistence`
(authoritative) is owned by `VoxelWorld` (CPU-only), not by a GPU-coupled
manager (Phase B); the derived render pipeline lives in the renderer-owned
`RenderWorldBridge`, which only *references* the world, while the temporary
`ChunkManager` adapter and `ChunkCollisionView` light wrapper are **removed**
(Phase C); the player/mob simulation plus the authoritative world are owned
by the CPU-only `GameSimulation` facade, with the temporary `ChunkMobWorld`
mob adapter **removed** (Phase D); and the graphical `Engine` is a pure host
of that facade — commands in, snapshots out, no authoritative gameplay state,
a single suspension policy and an explicit world-epoch barrier (Phase E).
**All temporary migration adapters are removed.** A headless consumer can
open, edit, flush, close and reopen a world with no renderer present
(`test_voxel_world`, links only `ft_vox_core`), and can create, advance,
edit, save, reload and restore a full simulation — player and mobs included —
with no camera or renderer (`test_game_simulation`, links only
`ft_vox_core`); the bridge attach/detach/reattach boundary (including the
epoch stamp) is covered by `test_render_bridge`, and the graphical host's
epoch barrier by `test_engine_reload`.

---

## 4. The CPU-only library: `ft_vox_core`

A named CMake **static library** (preferred over hidden source-list reuse)
holding the translation units that compile independently of the renderer.
Phase A extracted the six already-independent units; Phase B added the
authoritative world and its backing pools; Phase D added the simulation
facade:

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
src/Sim/GameSimulation.cpp        (Phase D: authoritative simulation facade)
src/Items/ItemRegistry.cpp        (Epic #223 Phase A: item catalog + block mappings)
src/Items/ItemStack.cpp           (Epic #223 Phase A: pure stack math)
src/Items/Inventory.cpp           (Epic #223 Phase A: 36-slot container)
src/Items/ItemEntitySystem.cpp    (Epic #223 Phase C: dropped item entities)
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
  differ on Linux. Phase F (#210) added the canonical shared flag set as the
  `FT_VOX_PORTABLE_FP` option (no `-march=native`, `-ffp-contract=off` /
  `/fp:precise`) for cross-machine digest comparison; the default codegen is
  unchanged. See [`headless-runner.md`](headless-runner.md) §2/§5.

The game executable compiles `FT_VOX_GAME_SOURCES = FT_VOX_SOURCES −
FT_VOX_CORE_SOURCES` and links `ft_vox_core` **first**, followed by the existing
`FT_VOX_COMMON_LIBS` (SDL3, glm, FastNoise2, miniz, volk, VMA, Threads). The six
core TUs are therefore compiled exactly once (in the library), with no duplicate
symbols.

**Deliberately excluded** — two distinct categories (the include guard, §6,
enforces both):

- **Render-coupled — never:** `Chunk`, `ChunkGpu`,
  `RenderWorldBridge`, and everything under `Renderer/`, `Vulkan/`,
  `Camera/`, `Engine/`, `ImGui*/`, `Network/`. These pull
  Vulkan/VMA/Camera/Engine/Renderer headers and must never enter the core, in
  any phase. (Phase B moved the authoritative voxel state OUT of `Chunk` into
  `WorldChunk`; `Chunk` itself remains a render handle. Phase C removed
  `ChunkManager` / `ChunkCollisionView` entirely; Phase D removed
  `ChunkMobWorld` — its CPU seam is `World/WorldMobView.hpp`.)
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
budgets stay with the host's frame pacing. **Phase D completed the
simulation-side separation:** `GameSimulation` takes plain values only
(elapsed time, `sim::PlayerCommand`, observer point, available radius) and
exposes mob spawn/retire/interest settings (`entities::MobSettings`) without
any `Camera`/`RenderSettings` type; the per-second budgets and the day-cycle
presentation parameter remain host-side. `EngineDefs.hpp` itself remains
render-side and is still not included by any core header (the guard proves
it).

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

`tests/test_game_simulation.cpp` → `test_game_simulation` (Phase D) also links
**only** `ft_vox_core` and proves the issue #208 acceptance list without a
graphics runtime, camera or window: scripted fixed-seed determinism
(player/mob/voxel output identical across runs), physics edge fixtures
(jump/coyote/swimming/flight toggle, unknown-terrain hold, spawn recovery,
dropped-step cap, delta spikes, suspension without catch-up burst), mob
fixtures (group spawn/retire, no NaN, previous/current interpolation,
per-seed reproducibility with controlled residency/clock), reload +
save/player restore (refused close keeps the old world alive), and the
independent 120 Hz / 60 Hz update rates + bounded value snapshots. Registered
as CTest `GameSimulation`.

`tests/test_items.cpp` → `test_items` (Epic #223 Phase A, issue #224) also
links **only** `ft_vox_core` and proves the item-domain acceptance list
without a graphics runtime: registry invariants (unique ids/names,
append-only ordinals, every placeable item maps back to its block, drop-table
totality over breakable blocks, name-parsing round-trips), pure stack-math
edges (overflow at maxStack, partial merge, odd split, empty stacks,
`uint16_t` boundaries) and inventory edges (full-inventory partial add,
hotbar-first ordering, selection bounds, the move/swap/split result matrix,
4000-step deterministic repeated sequences). Registered as CTest `Items`.

`tests/test_gameplay.cpp` → `test_gameplay` (Epic #223 Phases B/C/D/E, issues
#225/#226/#227/#228) also links **only** `ft_vox_core` and proves the facade
integration without a graphics runtime. Phase B: the command result matrix
(invalid slots, empty stacks, full inventory, mismatches, partial
moves/splits), the logged `grantItems` admin seam with partial semantics,
2000-step deterministic grant→move→split→swap sequences (step-identical
across simulations + seeded replay), `InventorySnapshot` boundedness +
in-place fill, `initialize`/`reload` resets (incl. the refused-reload
contract), suspension without command replay, and the gameplay/creative
selection duality. Phase C: break→drop→pickup→inventory end-to-end,
pickup-delay/partial-pickup/full-inventory rules, the `breakBlock` result
matrix (bedrock/fluids/unknown/AIR/double-break), age despawn + the
deterministic oldest-first cap overflow, monotonic never-reused ids with the
exposed high-water, two fixed-seed runs with identical per-frame populations/
trajectories/events, 60 Hz rate independence (bit-identical across host dt
patterns, dropped-step cap, delta spikes), suspension freeze without replay,
id-sorted bounded snapshots and the bounded spawn/remove event stream.
Phase D: raycast parity fixtures against the verbatim historical Engine
march (hit/miss/adjacent-fallback/unknown/max-distance), the
`placeSelectedBlock` result matrix + consume-on-success-only + edit-result
exposure, and the full walking loop (break→drop→pickup→select→place
consumes) with the world edit verified at every stage. Phase E:
save→close→reopen restores inventory + entities (bit-exact) + allocator
high-water, ids never collide across sessions, corrupt/missing new files
degrade to empty with a loud log while the world opens, old-format saves
load unchanged, refused close keeps the old state, and the shutdown safety
net captures gameplay state. Registered as CTest `GameplayLoop`.
`tests/test_world_save.cpp` → CTest `WorldSave` gained the matching
format-layer coverage: inventory.state / items.state round trips,
exact-size checks, the full corruption taxonomy (magic/version/payload/
id-ordering/allocator/cap) and atomic-write hygiene.
`tests/test_ui_shell.cpp` → CTest `UiShell` gained the inventory presentation
logic (issue #229): panel/hotbar slot layout math, drag-cursor display
(whole/half) and the click→command planning matrix (move/swap/split/cancel).
`tests/test_engine_reload.cpp` → CTest `EngineReload` pins the item
presentation epoch reset; `tests/test_visual_regression.cpp` gained the
`item_entities` offscreen scene (committed reference).

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

### Item domain: `src/Items/` (Epic #223 Phase A, issue #224)

The gameplay loop (break → drop → pickup → inventory → place) needs an item
vocabulary that is **stable for saves and future replication**, independent
of the render-atlas `TextureType` enum (per-face variants, append-only for
rendering reasons). Phase A was contracts-only; **Phase B (issue #225) made
`sim::GameSimulation` the single owner of the inventory** and added the
gameplay commands + `InventorySnapshot` (§10); Phases C/D (#226/#227) closed
the loop and wired the host; **Phase E (issue #228) persisted it**
(inventory.state + items.state, §10). Still ahead: UI #229, headless
grammar #230:

| Contract | Definition | Notes |
|---|---|---|
| Item identity | `items::ItemId : uint16_t` | append-only registry ordinals (1..`kItemCount`), `None = 0` sentinel; saves/wire reference `ItemId`, never `TextureType` |
| Stack | `items::ItemStack{item,count}` | 4-byte plain value; empty = count 0; **canonical empty** `{None,0}` — every operation normalizes drained stacks to it (byte-equal determinism) |
| Results | `items::ItemResult` | `Ok / Partial / Full / InvalidSlot / EmptyStack / InvalidItem / ItemMismatch / NotPlaceable` — the future command/rejection vocabulary |
| Catalog | `items::ItemDefinition{id,name,maxStack,placeableBlock}` | one static table, one block item per breakable/placeable block (91 entries, `maxStack` 64); `static_assert`s pin ordinal/name/placeable alignment (the `biomeTypeString` / `kBlockLayers` discipline) |
| Block mappings | `itemForBlock / dropForBlock / isBreakableBlock` | total over the atlas; face variants alias their canonical block; explicit drop table: `STONE→COBBLESTONE`, grass block→`DIRT`, leaves/ores→themselves, `BEDROCK` unbreakable, `WATER`/`LAVA` not breakable by hand |
| Stack math | `mergeStacks / splitStack / splitHalf / transferStack / makeStack` | pure functions, explicit results, no heap; odd half-split takes the **larger** half (ceil); shared by the inventory and the future pickup/drop paths |
| Container | `items::Inventory` | fixed 36 slots (hotbar 0..8), selected hotbar index, deterministic `addStack` (hotbar-first merge, then empty slots, partial remainder) / `removeCount` / `moveStack` / `swapStacks` / `splitStack` / `setSelectedSlot`; trivially copyable, no heap churn |
| Grammar seam | `items::parseItemId` | the `textureTypeString` token discipline (case/space/hyphen-insensitive `UPPER_UNDERSCORE` or numeric ordinal), reused by the headless grammar in #230 |

Dependency rule: `ItemTypes.hpp` is std-only; the rest of the module includes
`utils.hpp` (the `TextureType` catalog) + std. The render-side face remapping
(`Renderer/MinecraftTextures.hpp`) is deliberately **not** included — the
registry owns its own variant→canonical mapping (`canonicalBlock`), pinned
row by row by `test_items` (`kFaceVariants`). The two mappings cannot be
cross-checked automatically (dependency rule), so a new per-face atlas
variant must update **all three together**: `canonicalBlock`, the render
`blockTopFace`/`blockBottomFace` remap, and the `kFaceVariants` pin table
(the `AGENTS.md` item-registry convention and the terrain-generation
block-addition procedure name them). The headers/TUs are guarded roots of
`CoreIncludeGuard` (§6).

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
  - **Render-coupled — never:** `Chunk.hpp` (pulls
    Vulkan/VMA/Camera/Engine/Renderer transitively). Phase C removed
    `ChunkManager.hpp` / `ChunkCollisionView.hpp`; Phase D removed
    `ChunkMobWorld.hpp` (the CPU mob seam is `World/WorldMobView.hpp`).
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
(`ThreadPool`, `WorkloadTelemetry`), the Phase D facade
(`Sim/GameSimulation.hpp/.cpp`), every core TU/header (`WorldSave`,
`WorldPersistence`, `WorldChunk`, `VoxelWorld`, `WorldCollisionView`,
`WorldMobView`, `TerrainGenerator`, `TerrainVegetation`, `VoxelPool`,
`ChunkBorders`, `MobSystem`, `MobModel`, `VoxelCollision`, `PlayerController`,
`BlockPhysics`, `MobTypes`, `LightSample`) and the Epic #223 item domain
(`Items/ItemTypes.hpp`, `Items/ItemRegistry.hpp/.cpp`,
`Items/ItemStack.hpp/.cpp`, `Items/Inventory.hpp/.cpp`,
`Items/ItemEntitySystem.hpp/.cpp`).

---

## 7. Migration checkpoints

Each child issue must leave the game buildable and tests passing. "Create the
boundary before migrating behavior."

| Phase | Issue | Checkpoint | Gate |
|---|---|---|---|
| **A** | **#205** | `ft_vox_core` + `SimContracts.hpp` + include guard + baselines + this doc. No behavior change. | CPU-only target builds/links/passes without GPU/SDL; guard green; game + tests unchanged — **landed (PR #212)** |
| **B** | **#206** | `VoxelWorld` + `WorldChunk` own voxels, residency, edits, generation publication **and `WorldPersistence`**; `WorldCollisionView` carries the collision semantics; `sim::ChunkIdentity` is the single (generation, revision) source (`MeshBuildResult` copy deleted); `Chunk`/`ChunkManager` were derived render adapters (§10). | Headless world load/edit/save without renderer (`VoxelWorld` suite, 211 checks); save identity stable; per-seed fixtures byte-identical; full suite + visual goldens hold — **landed (PR #214)** |
| **C** | **#207** | `RenderWorldBridge` (renderer-owned) produces derived meshes/lighting/uploads from authoritative state and **references** the host-owned world; mesh publish validates against `sim::ChunkIdentity` (no duplicate pair). **Removed** the `ChunkManager` render adapter and the `ChunkCollisionView` light wrapper; the bridge attaches to a live world and the world runs headless without it. | Render output unchanged (visual goldens identical); bridge consumes the event stream + resident snapshots, never owns sim; `RenderBridge` boundary suite + full suite + goldens hold — **landed (PR #215)** |
| **D** | **#208** | `sim::GameSimulation` facade (CPU-only, in `ft_vox_core`) owns `TerrainGenerator` + `VoxelWorld` + `PlayerController` (120 Hz) + `MobSystem` (60 Hz); explicit lifecycle/commands, plain-value observer, read-only bounded snapshots. **Removed** the `ChunkMobWorld` mob adapter (CPU seam: `WorldMobView`; entity light stays bridge-derived, sampled by the host after the snapshot). | Fixed-step determinism preserved; no render includes; headless `GameSimulation` suite (scripted determinism, physics/mob edges, reload/restore, rates) + full suite + goldens hold — **landed** |
| **E** | **#209 (this)** | Graphical `Engine` host drives `GameSimulation` via commands + snapshots: documented 16-step `run()` orchestration, single `simulationSuspended()` policy, player-aware placement query (`canPlaceVoxel`, hosts gate edits with it), world-epoch barrier (`resetWorldProjection` + bridge epoch stamp). No authoritative gameplay state in the host; all temporary adapters removed. | Game behaves identically; input/UI unchanged; epoch monotonicity + derived-state reset covered by `EngineReload` (real derived-projection step before reload, so the reset checks are non-vacuous), placement query by `GameSimulation`, epoch stamp by `RenderBridge`; full suite + goldens hold — **landed** |
| **F** | **#210** | `ft_vox_headless` executable links `ft_vox_core` only — no SDL/Vulkan/ImGui, no camera/renderer/GPU pool. `FT_VOX_GRAPHICS=OFF` configure path: the graphics stack is **not discovered at all**; CPU library + runner + CPU tests build on a machine with no Vulkan SDK/SDL3/shader toolchain. Explicit headless interest source drives residency; deterministic sync + async generation; canonical state digest; save/reload round-trip. | Headless run + save round-trip + scripted determinism (`HeadlessSmoke`, `HeadlessRunner`); dependency guard (`HeadlessDeps`); clean CPU-only configure/build gate (`CpuOnlyBuild`); top-level GPU configure requirement removed — **landed** |
| **G** | **#211** | Closure gate: dependency-direction enforcement hardened (configure-time transitive link-closure assertion on `ft_vox_core` + `ft_vox_headless` + every CPU test; `HeadlessDeps` scans core **and** runner link artifacts); reproducible interleaved performance-parity protocol (`tests/bench_parity_211.py`); cleanup audit (adapters removed, no duplicated sim path); docs finalized (executable/module graph + enforcement table). | In-game benchmark Grade **S** parity (5 interleaved trials, frame tail/GPU/memory parity-or-better); CPU sim profiles bit-identical; the explained event-boundary streaming-scope cost tracked in #219; full suite + visual goldens hold; CPU-only checks run without a GPU — **landed** |

C (#207) and D (#208) may proceed in parallel after B; E (#209) follows both;
F (#210) follows D; G (#211) is last.

---

## 8. Build-system dependency — RESOLVED in Phase F (#210)

The `ft_vox_core` library itself needs only GLM, FastNoise2 and
`Threads::Threads`. Before Phase F, the **top-level configure** hard-required
the graphical stack for *every* configuration — including one that only wants
the CPU library/tests. Phase F (#210) removed that requirement: the
`FT_VOX_GRAPHICS` option (default ON) now scopes every graphical discovery in
`cmake/Dependencies.cmake`, and with `FT_VOX_GRAPHICS=OFF` **none of the
calls below runs at all**:

| Dependency | Call | Failure mode (pre-#210) | Needed by `ft_vox_core`? | Phase F status |
|---|---|---|---|---|
| Vulkan loader/headers | `find_package(Vulkan REQUIRED)` | **fatal** at configure | No | guarded by `FT_VOX_GRAPHICS` |
| volk | CONFIG / FetchContent, `FATAL_ERROR` | **fatal** at configure | No | guarded by `FT_VOX_GRAPHICS` |
| SDL3 | CONFIG / pkg-config / FetchContent, `FATAL_ERROR` | **fatal** at configure | No | guarded by `FT_VOX_GRAPHICS` |
| Shader compiler (`glslc` / `glslangValidator`) | probe, `FATAL_ERROR` | **fatal** at configure | No | guarded by `FT_VOX_GRAPHICS` |
| Vulkan Memory Allocator | CONFIG / header / FetchContent | non-fatal | No | guarded by `FT_VOX_GRAPHICS` |
| miniz | CONFIG / FetchContent | non-fatal | No | guarded by `FT_VOX_GRAPHICS` |

The shader targets, the game executable and every graphical test target are
likewise wrapped in `if(FT_VOX_GRAPHICS)`. The CPU-only configuration is
verified by the `CpuOnlyBuild` CTest (a separate clean configure/build/test
run) and the `HeadlessDeps` guard (include closure + link artifacts + cache
discovery check). See [`headless-runner.md`](headless-runner.md) §2.

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

`takeEvents()` has two forms: the return-by-value drain, and a buffer-reuse
overload `takeEvents(std::vector<WorldEvent>&)` that appends into a caller-owned
vector (issue #219). The bridge keeps one persistent drain buffer and uses the
overload so the several drains per frame stop allocating a fresh vector each
time; the events and their order are identical either way. The per-call drain
points are kept (they are ordering-critical, issue #114) — the cost reduction
came from the buffer reuse and from hoisting the per-voxel handle indirection
out of the mesh-dispatch halo fill, not from removing drains.

### Publication ordering (preserved exactly)

`RenderWorldBridge::processFinishedJobs()` stages the pump so the issue #114
ordering holds: **(1)** world generation publication → **(2)** render
mesh/light publication (the superseded-by-edit checks still see the pending
queue) → **(3)** `applyPendingEdits()` → render edit invalidation. The
persistence durability barrier quiesces render and world jobs together before
the world captures/flushes/shuts down; a failed flush or stranded edits keep
persistence OPEN and retryable (issue #180 contracts unchanged). Ownership of
the close itself is split (issue #207 review): the bridge destructor and
`closeWorld()` only **quiesce derived work** (publish jobs, clear in-transit,
apply deferred edits); the **host** closes authoritative persistence
(`Engine::~Engine` calls `VoxelWorld::forceCloseWorldForShutdown`, and
`~VoxelWorld` is the final safety net). The bridge never closes a world it only
references. On attach, `bindRecord` also **re-arms a stale `MESHED` claim** to
`GENERATED`: a projected record's geometry lived on the previous bridge's
handles, so a fresh handle must rebuild it rather than stay resident-but-
invisible.

### Locks

The world `shared_mutex` guards authoritative state; the bridge render
`shared_mutex` guards the handle map and light caches. `WorldCollisionView`
holds the world lock for its lifetime (scoped-view contract: no edits/unloads
during a view). Phase D **removed the nested world → render acquisition**:
the former `ChunkMobWorld` adapter held both locks; its replacement
`WorldMobView` holds only the world lock, and the graphical host samples
entity light from the bridge (`sampleSmoothedLight`, render lock only) AFTER
the mob snapshot returns. Mesh dispatch still acquires render → world (via
`ensureShellPopulated`). Every acquisition remains main-thread-only; workers
take only the completion-queue and pool mutexes, so the two map locks never
contend across threads. Phase E (#209) kept both paths main-thread-only;
revisit the lock order before threading either path.

### Temporary adapters — removal status

Per the issue's architectural constraints, these facades were **documented and
must be removed before Epic #204 completes**:

1. **`ChunkManager` render adapter** — **REMOVED in Phase C (#207).** Mesh/light
   dispatch, GPU uploads, commit groups, visibility and the debug trace moved
   into the renderer-owned `RenderWorldBridge`, which references (never owns)
   the world. Phase D (#208) lifted `VoxelWorld` ownership into
   `GameSimulation`; the bridge's `world()` accessor is the read seam.
2. **`ChunkCollisionView` light wrapper** — **REMOVED in Phase C (#207).**
   Physics consumes `WorldCollisionView` (CPU-only) directly; the derived
   light-sampling seam lives on the bridge (`sampleSmoothedLight`).
3. **`ChunkMobWorld` mob adapter** — **REMOVED in Phase D (#208).** Mobs run
   behind `GameSimulation` over the CPU-only `WorldMobView` (collision +
   biome/grass surface scan, exact historical semantics). The derived entity
   light sampling it used to wrap stays on the bridge: the graphical host
   samples `sampleSmoothedLight` per mob AFTER taking the mob snapshots, and
   headless snapshots leave `localSkylight` unassigned (< 0). The
   `entities::MobWorld` interface no longer declares a light seam.
4. **`Chunk` render handle** — Phase C re-homed the mesh/light payloads under
   the bridge; the handle is now the bridge's per-chunk render record. The
   authoritative `WorldChunk` is unaffected.

### The Phase D simulation contract (issue #208)

`sim::GameSimulation` (`src/Sim/GameSimulation.hpp/.cpp`, in `ft_vox_core`)
is the single owner of authoritative gameplay state: `TerrainGenerator` +
`VoxelWorld` + `PlayerController` (120 Hz) + `MobSystem` (60 Hz) +
`items::Inventory` (Epic #223 Phase B, issue #225), flight mode,
block selection, spawn/restore and the session reload counter. Its contract:

- **Commands in, snapshots out.** The host supplies plain values — elapsed
  time, `sim::PlayerCommand`, an observer/interest point, an available radius,
  suspension flags — and consumes bounded read-only values
  (`sim::PlayerSnapshot`, `sim::MobSnapshot` = `entities::MobRenderState`,
  `sim::WorldSnapshot` with seed/generator version/reload counter/edit
  revision/residency/persistence identity). No Camera, RenderSettings, SDL,
  Vulkan or ImGui type appears in the API (the include guard proves it).
- **Policy stays with the host.** Pause/focus/benchmark decisions are the
  graphical host's; the simulation implements suspension semantics only
  (accumulators cleared, previous/current synced, no catch-up burst). SDL
  key/mouse → normalized command mapping stays in the Engine. Flight is
  host-driven: the observer moves the camera, `setPlayerPosition` follows
  with the authoritative body (headless hosts script the same call).
- **Lifecycle is explicit and refusable.** `initialize` (seed resolution +
  persistence open + saved-player read) → terrain bootstrap (bridge
  `generateInitialArea` graphically, `bootstrapTerrain` headlessly) →
  `finalizePlayerSpawn` (validated restore or surface spawn). `reload`
  returns false and keeps the OLD world authoritative when the persistent
  close is refused (stranded edits); the graphical host quiesces/closes
  through the bridge first because render jobs hold record leases.
  `savePlayerState(yaw, pitch)` captures the authoritative player plus the
  host-supplied presentation orientation; `shutdown` is the unconditional
  destructor-only close.
- **Publication ordering is preserved.** The simulation does not pump the
  world inside `advancePlayer`/`advanceMobs`: the bridge's
  `processFinishedJobs()` keeps the issue #114 sequence (world generation →
  render mesh/light → deferred edits). `pumpWorld()` is the headless-only
  publication seam. Graphical voxel edits go through the bridge (event drain
  + commit groups); `GameSimulation::placeVoxel/deleteVoxel` are the
  render-free path with the same world semantics.
- **Day/night is presentation.** The day cycle (`ShaderParameters.dayTime`)
  is a shader/presentation parameter advanced by the graphical host exactly
  as before (pause gates it host-side); it is NOT authoritative world time.
  The simulation owns no wall clock beyond the fixed-step accumulators, so
  headless runs never advance it and the visuals are unchanged.

### The Epic #223 Phase B inventory contract (issue #225)

Phase B of the gameplay-loop epic made the facade the **single owner of the
player inventory** (`items::Inventory`, the issue #224 item domain) and added
the first explicit gameplay commands + snapshot. No entities, no world
interaction yet — the loop closes in C/D (#226/#227).

- **Commands in, snapshots out — unchanged.** `selectHotbarSlot(index)`,
  `moveStack(from, to)`, `swapStacks(a, b)`, `splitStack(from, to)` answer
  with `items::ItemResult`; `grantItems(ItemId, count)` is the **test/admin
  seam** (epic §3): every call is explicitly logged (`[items] grant …`) and
  answers `items::AddResult` with the partial-pickup remainder semantics.
  Plain-value arguments only — no Camera/RenderSettings/SDL/ImGui type
  enters the API (`CoreIncludeGuard` walks the `Items/` closure).
- **Instantaneous application, no fixed-step work.** Commands apply at the
  call itself (the host frame boundary), BEFORE the next
  `advancePlayer`/`advanceMobs` step. There is no command buffer, so
  suspension never replays or reorders anything — a command issued while
  suspended applies exactly once, at call time. Inventory commands are
  world-independent (they work before `initialize`).
- **Bounded snapshot.** `sim::InventorySnapshot` = the fixed 36-slot array +
  selected hotbar index, filled IN PLACE like `mobSnapshots` (no per-frame
  allocation). Display names resolve through the registry on the UI side
  (#229); the snapshot carries stable `ItemId`s, never strings.
  `inventory()` is a read-only seam — mutations go through commands only.
- **Lifecycle.** `initialize` and a SUCCESSFUL `reload` reset the inventory
  to empty (selection 0); a REFUSED reload keeps the old world AND the old
  inventory authoritative. The `inventory.state` restore hook landed in #228
  on the `player.state` seams: `initialize` reads it, `finalizePlayerSpawn`
  restores it (missing file = fresh/quiet, corrupt = empty + loud save-error
  log; old saves load unchanged).
- **Selection duality (deliberate, documented).** The hotbar selection is
  the GAMEPLAY selection; `selectedBlock()` stays the CREATIVE/EDITOR seam
  (flight mode, benchmarks, the `T` cycle, headless place/delete scripts;
  host-wired in #227). The two never synchronize; `player.state`
  `selectedBlock` persistence is unchanged — the hotbar selection persists
  in `inventory.state` since #228 (no `player.state` version bump).
- **No behavior change.** The graphical host still uses `selectedBlock()`
  and issues no inventory commands yet; the full suite + visual goldens
  hold. Acceptance: `tests/test_gameplay.cpp` → CTest `GameplayLoop`
  (links only `ft_vox_core`).

### The Epic #223 Phase C item-entity contract (issue #226)

Phase C closed the **survival-lite loop inside the simulation**:
break → drop → auto-pickup → inventory. Dropped items are the epic's first
non-mob entity family, built on the `MobSystem` pattern (no ECS, epic §7)
and designed for replication (§2/§6).

- **`items::ItemEntitySystem`** (`src/Items/ItemEntitySystem.hpp/.cpp`, in
  `ft_vox_core`, owned by `GameSimulation`): plain-value entities (id,
  `ItemStack`, `physics::Body`, previous position, age/pickup-delay ticks),
  fixed **60 Hz** step with its own accumulator + `maxSteps` catch-up cap +
  dropped-step counter (the mob discipline), suspension freeze +
  previous/current sync. Movement reuses `physics::move` through
  `WorldCollisionView` — **no new solver**; unknown terrain holds the body.
  Gravity + ground-friction settle; v1 has no fluid interaction rule (items
  sink through fluids and settle on the bottom — documented simplification).
- **Ids**: simulation-owned monotonic allocator (starts at 1, never reused
  within a session; `reset()` is the session boundary called by
  `initialize`/successful `reload`). `itemIdHighWater()` exposes the mark;
  #228 persists it (items.state `nextEntityId`) and restores it, so ids are
  never reused across sessions. Spawn order = command/tick order.
  **Per-family namespace**: item ids and mob ids are separate allocators
  (both start at 1, the `MobSystem` pattern) — consumers pair an id with its
  entity family; the persistence format keeps the namespaces distinct
  (mob ids are not persisted in v1).
- **Deterministic rules**: pickup-delay countdown, then proximity pickup
  (1.5 blocks to the authoritative feet) merging **hotbar-first** through
  the Phase A stack math — partial pickup leaves the remainder in the world,
  a full inventory leaves the entity untouched; age despawn (18000 ticks =
  5 min); population cap 1024 with **oldest-first** overflow despawn (age
  ties break on the smallest id). Spawn jitter derives only from the entity
  id + edit revision (the splitmix64 finalizer) — no RNG state, no wall clock.
- **Commands**: `breakBlock(cell)` validates loaded/breakable per the
  registry (`BreakResult::Ok / Unbreakable / UnknownTerrain / WorldRejected`),
  deletes through the existing world edit semantics (issue #114 ordering
  preserved — the collision view's shared lock is released before the edit
  lock) and spawns the registry drop at the cell center.
  `dropFromSlot(slot, count, dir)` removes via `removeCount` semantics and
  spawns one entity at the player eye with the host-supplied plain direction
  (normalized; degenerate → +Y) and deterministic speed; manual drops carry
  the longer 40-tick pickup delay.
- **Derived outputs**: id-sorted bounded `sim::ItemEntitySnapshot`
  (= `items::ItemEntitySnapshot`, the `MobSnapshot` alias pattern) with
  previous/current + item/count/age, filled in place; mix with
  `itemInterpolationAlpha()`. Bounded spawn/remove event stream
  (`items::ItemEntityEvent`, the `WorldEvent` drain pattern with observable
  cap drops): `Spawned` / `Removed{PickedUp, Aged, Overflow}`; partial
  pickups are snapshot count changes, not events.
- **Frame sequencing**: `advanceItems(dt, suspended)` is a separate facade
  step the host calls next to `advanceMobs` (the call site landed in #227,
  `Engine::tickItems`); the world is NOT pumped inside it.
- **Out of scope**: place-from-hotbar/raycast/Engine rewiring (#227),
  entity/allocator persistence (#228), rendering/UI (#229), runner
  grammar/digest (#230).

### The Epic #223 Phase D interaction contract (issue #227)

Phase D moved the **last interaction rules into the simulation** and made
the Engine a pure command issuer for interactions, completing the #209 host
contract ("no gameplay logic in the host") and the in-game loop
break → drop → pickup → **place (consumes)**.

- **Sim-owned targeting**: `GameSimulation::raycastVoxel(origin, dir,
  maxDist)` is the historical `Engine::raycastVoxel` march moved VERBATIM
  (0.05 float step, first active cell — at #227 via
  `VoxelWorld::isVoxelActive`, the exact seam the bridge forwarded to —
  adjacent-cell fallback when the ray starts inside a block, unknown cells
  never hit). Returns the plain `sim::RaycastHit{hit, block, adjacent}`.
  `Engine::raycastVoxel` is a thin forward passing camera origin/front +
  `renderSettings.raycastDistance` as plain values; the highlight keeps
  consuming the result (presentation). CPU parity fixtures compare the
  query against the verbatim historical march bit for bit.
  **Stabilization PR1/PR2 update:** the cell query now samples the
  EFFECTIVE authoritative state through a scoped `WorldCollisionView`
  (stored backing + pending logical edits) — the same state the gameplay
  commands and physics observe — instead of raw-backing `isVoxelActive`,
  so deferred deletions/placements are targeting-consistent before
  publication. Inputs are fully validated (non-finite origin/dir/distance,
  degenerate directions, out-of-envelope origins answer no-hit) and the
  march is clamped to `kMaxRaycastDistance` (64) + iteration-bounded. Valid
  rays without pending edits keep bit-exact historical parity (the fixtures
  still pass unchanged); see `docs/gameplay-loop.md` §4.
- **`placeSelectedBlock(cell)`**: selected hotbar stack must hold a
  placeable item (registry `placeableBlock`); gates with the existing
  `canPlaceVoxel` player-aware rule (#209); edits through the existing world
  semantics; **consumes exactly 1 item on success only**.
  `PlaceResult::Ok / EmptySlot / NotPlaceable / PlayerOverlap /
  UnknownTerrain / WorldRejected` (NotPlaceable is future vocabulary — every
  v1 catalog item is placeable; gameplay refuses unknown targets, unlike the
  creative seam which may defer into registered-but-unloaded records).
- **One edit publication path**: `breakBlock`/`placeSelectedBlock` take an
  optional `WorldEditResult *outEdit`; on acceptance the result (edit id +
  geometric members) moves out and the host calls
  `RenderWorldBridge::publishEditResult` — the SAME event drain +
  commit-group registration the creative `placeVoxel`/`deleteVoxel` use
  (they now share that helper). No second edit path exists.
- **Engine input mapping (host policy stays host-side)**: LMB →
  `breakBlock` walking / creative `deleteVoxel` flight; RMB →
  `placeSelectedBlock` walking / creative `canPlaceVoxel` + bridge place
  flight; `1..9` → `selectHotbarSlot`; wheel → hotbar cycle (captured, not
  isometric-zoom); `Q` → `dropFromSlot(selected, 1, cameraFront)`; `E` →
  inventory-panel UI flag (the panel renders in #229); `T` legacy block
  cycle is FLIGHT/creative only. Capture/pause/benchmark gating unchanged;
  the new keys classify as `GameplayShortcut` (inert while ImGui owns the
  keyboard — `test_input_routing`/`test_ui_shell` pin the policy/metadata).
  `Engine::tickItems` runs `advanceItems` next to `tickMobs` under the same
  suspension policy (frame step 9b). Item snapshots get their renderer in
  #229, which also drains the lifecycle stream: until then the bounded
  event stream is undrained in real play — long walking sessions reach the
  4096 cap and drop the OLDEST events, observable via
  `itemEntities().droppedEventCount()` (the `WorldEvent` contract; no
  functional loss once #229 drains at frame cadence).
- **Host-contract checklist (no gameplay rule left in the host)**:
  targeting → `sim::raycastVoxel`; break/drop rules → `sim::breakBlock` +
  registry; consumption → `sim::placeSelectedBlock`; pickup →
  `ItemEntitySystem`; placement validation → `sim::canPlaceVoxel`. The
  Engine/bridge keep ONLY input mapping, presentation (highlight, camera),
  publication (event drain + commit groups) and host policy
  (capture/pause/benchmark, `raycastDistance`).
- **Behavior change is the point**: WALKING mode now plays the survival-lite
  loop; FLIGHT behaves exactly as before (creative infinite blocks).
  Visual goldens hold — reference scenes use the creative/bridge path.
  Persistence (#228), UI/HUD + item rendering (#229) and the runner grammar
  (#230) stay out of scope.

### The Epic #223 Phase E gameplay-persistence contract (issue #228)

Phase E made the loop **survive save / close / reopen**. Issue #180's
contracts (atomic writes, magic/version guardrails, refusable close,
identity rules) are the baseline and are UNCHANGED; `player.state` v1 stays
byte-compatible; the new state lands in new sibling files, so old saves load
unchanged (missing file = fresh, quiet; no migration machinery).

- **Formats** (spec in `WorldSave.hpp`, codecs in `WorldSave.cpp`):
  `inventory.state` — magic `FTVI`, version 1, exactly **153 bytes**
  (`selectedSlot u8` + 36 × `itemId u16 | count u16`); empty inventory is a
  valid all-zero payload. `items.state` — magic `FTVJ`, version 1,
  **20 + 68×entityCount bytes** (`nextEntityId u64` = allocator high-water
  + 1, `entityCount u32`, id-sorted records: id, stack, position/velocity
  3×f64 each, ageTicks, pickupDelayTicks). Tightly packed little-endian via
  `ByteWriter`/`ByteReader`; strict decode = loud `SaveStatus` (exact size,
  magic/version, known registry items, count ≤ maxStack, selection range,
  ascending unique ids < nextEntityId, finite transforms, population cap) —
  **never silently clamped**. The codecs validate against the item registry
  (CPU-only include; the save layer stays sim/render-free).
- **`WorldPersistence`**: `writeInventoryState`/`readInventoryState`,
  `writeItemsState`/`readItemsState` — the exact `player.state` policy:
  atomic tmp+rename (`.tmp` never authoritative), missing = fresh/quiet,
  malformed = loud `status().lastError` entry. Plain-value payloads
  (`worldsave::InventoryPersistState`/`ItemsPersistState`, raw uint16 item
  ordinals — the `PlayerPersistState` pattern, no sim types).
- **Lifecycle**: `savePlayerState(yaw, pitch)` is now the FULL gameplay
  capture (name kept for host compatibility): player.state +
  inventory.state + items.state, all written BEFORE the flush durability
  barrier; a failed write is reported and stays retryable. `initialize`
  reads the new files alongside player.state; `finalizePlayerSpawn` restores
  inventory + entities + allocator (validated; corrupt degrades to empty
  with a loud log, never refuses the world open). `reload` (transient)
  resets everything. `shutdown`'s unconditional safety net also captures
  inventory + items (fully authoritative — no host input needed, unlike
  player.state's camera orientation).
- **Id durability**: the allocator resumes from the persisted
  `nextEntityId` → **no id reuse across sessions** (replication requirement,
  epic §2/§6). Per-family namespaces stay distinct (mob ids are not
  persisted in v1).
- **Restore seams**: `items::Inventory::restore` and
  `items::ItemEntitySystem::restore` are LIFECYCLE-only seams (not gameplay
  commands); the facade calls them with save-layer-validated state.
  Restored entities get previous = position (no interpolation jump).
- **Out of scope**: UI (#229), runner grammar / multi-process round-trips
  (#230 — this phase proves round-trips at the facade level), no
  `world.meta`/`player.state` version bump.
- **Stabilization PR5 update (Phase 3 checkpoints)**: the per-file atomic
  writes above remain, but the COLLECTION is now committed transactionally
  as a generation-based checkpoint with an atomic manifest
  (`worldsave::CheckpointStore`, `docs/adr/gameplay-persistence-transactions.md`).
  `savePlayerState` runs the flush barrier FIRST and reports success only
  when the manifest committed; `shutdown`/close commit carry-forward
  checkpoints ONLY after a complete drain+flush barrier (PR #243 review:
  a failed drain/flush skips the commit, distinct diagnostics, previous
  generation stays authoritative); `flushWorld` keeps its
  intermediate-barrier meaning (no gameplay commit). Transactional
  guarantees begin at the first successful commit — a failed first
  migration recovers via the legacy per-file policy (ADR §2.7, tested). Recovery selects the most recent fully valid generation,
  re-seeds the live working set from it and routes gameplay reads to it;
  corrupt generations fall back whole, never mixed; unrecoverable corruption
  refuses the open explicitly. The root state files + live `chunks/` remain
  as legacy compatibility shims (additive, non-destructive migration;
  rollback-safe). `PlayerPersistState` + its codec moved into `worldsave`
  (global alias kept) so the checkpoint validator shares the exact rules.

### The Epic #223 Phase F gameplay-presentation contract (issue #229)

Phase F added the **derived presentation** of the loop: hotbar HUD, inventory
panel and dropped-item rendering. Strict boundary (epic §5): these surfaces
consume bounded snapshots and issue explicit commands through callbacks —
**no gameplay rule in `GameUI`, `Engine` or the renderer**.

- **`GameUIFrame` extensions**: a `sim::InventorySnapshot` value (filled by
  `Engine::drawUi`), `std::function` command callbacks (`selectHotbarSlot`,
  `moveStack`, `swapStacks`, `splitStack`, `dropFromSlot`), the host-owned
  inventory-panel flag (`E`, from #227) and an `atlasVersion` counter for icon
  revalidation. Display names resolve through the registry on the UI side.
- **Hotbar HUD** (`GameUIInventory.cpp`): always visible in the gameplay view
  (benchmarks keep the stock UI closed); 9 slots, selection highlight, stack
  counts, icons = registry `ItemId → placeableBlock → block-atlas layer` via
  per-layer 2D views registered with ImGui (no new texture assets). Read-only.
- **Inventory panel** (`E`): 27 main + 9 hotbar grid; click-to-move/swap and
  right-click-to-split **through command callbacks only**. The drag cursor is
  UI-local presentation (a VIEW of the source slot via `InventoryUi.hpp`),
  discarded on close — authoritative moves happen only through commands, so
  closing mid-drag never duplicates items. Pure layout/cursor/click logic is
  headless-testable (`test_ui_shell`).
- **`ItemEntityRenderer`** (`src/Renderer/`, derived): fed by the host from the
  id-sorted `sim::ItemEntitySnapshot`s after the sim advance (the mob snapshot
  pattern): interpolated position, registry atlas layer, presentation spin/bob
  from the authoritative age, bridge-sampled local light. One shared unit cube,
  ONE instanced indexed draw for the visible population, frustum-culled,
  bounded by the population cap; receives directional shadows, **casts none**
  (no shadow-pass pipeline). Integrated in the opaque pass after mobs (shares
  the Mobs GPU profiler scope; the pass layout stays stable). Samples the SAME
  set-1 descriptors as terrain, so pack reloads / shadow resizes rebind it.
- **Epoch barrier**: `Engine::resetWorldProjection` drops the item snapshots +
  render states and pushes the empty set to the renderer (the mobStates
  discipline); the renderer holds no authoritative state and survives reload
  by re-reading snapshots. `Engine::tickItems` also drains the item lifecycle
  stream at frame cadence (v1 discards drained events — presentation is
  snapshot-driven — keeping `droppedEventCount` at 0 in real play).
- **Visual regression**: one new offscreen scene `item_entities` (flattened
  grass pad + 8 block-item mini-cubes, entity-free base frame); reference
  generated explicitly via `--update-references --scene item_entities` and
  committed. Existing scenes/goldens unchanged (the harness draws no ImGui;
  reference scenes contain no items).
- **Out of scope**: gameplay rules (already in the sim), command/snapshot
  semantics, persistence (#228), runner grammar (#230), new item textures.

### The Epic #223 Phase G headless gameplay + closure contract (issue #230)

Phase G made the loop **scriptable and digest-covered**, then closed the epic
with the full verification gate (the #211 pattern). No new gameplay behavior
— everything landed in A–F; this phase is runner integration + closure.

- **Script grammar** (`src/Headless/SimScript`, same line-based discipline +
  exit codes): `give <item> <count>` (the logged `grantItems` seam),
  `select <slot>` (0..8), `drop <count>` (selected slot, fixed **+Z**
  direction — the headless host has no camera), `break <x> <y> <z>`
  (gameplay `breakBlock`, distinct from creative `delete`),
  `placeSelected <x> <y> <z>` (gameplay place-from-hotbar, distinct from
  creative `place`), `expect inventory <slot> <item|empty> <count>` (slot
  0..35, `empty` pins count 0) and `expect items <n>` (0..capacity). Item
  tokens use `items::parseItemId` (the registry token discipline); counts
  are bounded to the `uint16_t` range and slots/cells to their domains **at
  parse time** (exit 3). Auto-pickup is a tick rule exercised through `wait`.
  Accepted gameplay edits count in `edits`; `PlayerOverlap` in `refused`;
  every other gameplay refusal in `rejected` with a stderr diagnostic
  carrying the `BreakResult`/`PlaceResult` code.
- **Frame orchestration**: the runner advances `sim.advanceItems(kFrameDt,
  false)` after `advanceMobs` (step 8 of 9) — the same tick rule the
  graphical host runs; zero items early-out at zero cost.
- **Canonical digest format bump**: `FTVHDIG1` → `FTVHDIG2` (deliberate, no
  silent hash drift). Appended canonical fields: the selected hotbar slot +
  the 36 inventory slots in fixed order (item u16 + count u16), the
  **id-sorted** item population (id u64, item u16, count u16, quantized
  position/velocity 3×i64 at a 1/1024-block grid, ageTicks u32) and the
  entity-id allocator high-water u64. No expected digest is hardcoded in
  tests/fixtures (computed at runtime), so the bump needed no fixture
  rewrite. Reference: [`headless-runner.md`](headless-runner.md) §5.
- **SUMMARY fields**: `items=<n>` (live entities), `inventory=<total count>`
  and `item_high_water=<n>` (allocator), machine-readable in both key=value
  and `--json` forms (the JSON object is the first `{` line — `grantItems`
  logs may precede it).
- **Runner tests** (`tests/test_headless_runner.py`, 72 checks): the
  canonical fixture's gameplay leg end-to-end (break → wait → pickup →
  select → placeSelected → drop, ending with a live item **in flight** at
  the digest boundary); **sync vs async digest parity with item physics**;
  a **3-process save/reload round-trip** restoring inventory + entities +
  allocator (the second session's drop gets id 2 — no reuse); gameplay
  grammar error exits and `expect inventory`/`expect items` mismatches
  (exit 5).
- **Guards**: `CoreIncludeGuard` + `HeadlessDeps` cover the final module set
  (the `Items/` headers are core, already in both closures); the
  configure-time link-closure assertion covers every CPU test target added
  by the epic; the opt-in `CpuOnlyBuild` gate re-verified.
- **Closure record**: `benchmarks/issue-230-closure.md` — full CTest suite, CPU-only
  configure/build/test gate, visual goldens (existing + the F scene),
  in-game benchmark parity vs the pre-epic baseline (the item step
  early-outs at zero items), and the determinism report. The epic #223
  definition-of-done checklist is fully ticked in
  [`gameplay-loop.md`](gameplay-loop.md) §10.

### The Phase E graphical host contract (issue #209)

Phase E completed the integration without changing any behavior: the Engine
already drove the facade since Phase D kept the game buildable; #209 made the
host contract explicit and closed the remaining gaps.

- **`Engine::run` is the documented frame orchestrator.** Sixteen numbered
  steps (events → viewport → acquire → frame clock → benchmark → player
  simulation → day cycle → streaming → mobs → highlight → frame slots →
  ImGui → UBO → record → present → telemetry), with the cohesive blocks
  extracted into named steps (`tickMobs`, `updateFrameUniforms`,
  `recordFrame`, `updateWindowTitle`). Profiler scope names and nesting are
  unchanged, so benchmark metrics reference the same frames. Acquire stays
  BEFORE the simulation steps: the paced dt is sampled acquire-to-acquire so
  WSI stalls never feed gameplay, and failed-acquire/minimize paths resync
  the frame clock instead of replaying accumulated time.
- **One suspension policy.** `Engine::simulationSuspended()` = paused ||
  unfocused || benchmark-active. The player suspends on it (and on focus
  transitions via `suspendPlayer`); mobs suspend on it plus the authoritative
  `mobsEnabled` toggle. Minimized windows skip whole frames and resync the
  clock after the sleep; inspection view skips `processInput` entirely. The
  simulation implements the freeze semantics only (accumulators cleared,
  previous/current synced, no catch-up burst) — it never decides WHEN.
- **No gameplay logic in the host.** The player-aware placement rule (a
  solid block may not overlap the authoritative body unless flying or
  non-solid) moved from the Engine's mouse handler into
  `GameSimulation::canPlaceVoxel` — a **pure query, not enforcement**:
  `placeVoxel` does not call it, so a host that wants player-aware placement
  must gate its edit with `canPlaceVoxel` first (the graphical host does;
  headless hosts/tests may place inside the body deliberately). The rule is
  defined once in the facade instead of copied into the host. The graphical
  host still publishes the accepted edit through the bridge (event drain +
  commit groups). **Epic #223 Phase D (#227) completed this for interactions**:
  the targeting march (`sim::raycastVoxel`), break/drop rules, hotbar
  consumption and pickup all live in the simulation; the Engine mouse/keys
  only map input to commands (walking) or keep the creative path (flight).
  Mouse capture, flight toggle and jump semantics are unchanged.
- **World-epoch barrier.** Every authoritative world transition
  (`initializeNoiseGenerator`, `reloadWorld`) runs
  `Engine::resetWorldProjection()` BEFORE detaching the old bridge: it drops
  the draw/shadow lists (they reference the old bridge's handles), the mob
  presentation states (and pushes the empty set to `WorldRenderer::setMobs`),
  the block highlight, the upload budget, the UI biome cache and the biome
  map. The simulation bumps `worldGenerationId()` across the transition and
  the replacement bridge is stamped with that epoch at attach
  (`RenderWorldBridge::worldGenerationId()`), so the projection's world
  identity is explicit and testable. Async safety is unchanged: the bridge
  destructor quiesces every in-flight job against the still-live old world
  before any handle is released, and the world's persistence close stays
  refusable (a refused close aborts the reload with the world intact).
  GPU lifetime stays graphical: `reloadWorld` waits device-idle and flushes
  the retire queue around the transition; uploads are recorded only inside
  `recordFrame` (no new GPU waits).
- **UI/debug panels keep consuming snapshots.** `GameUIFrame` is unchanged;
  biome-map requests/results already validate against the world generation
  + seed, and the barrier's `invalidateBiomeMap()` resets the pan/preview
  state with the epoch. No new design work.

### The Phase F headless runner contract (issue #210)

Phase F added the standalone consumer that proves the boundary — no
simulation behavior changed:

- **`ft_vox_headless`** (`src/Headless/`) links **only `ft_vox_core`**. It
  hosts `sim::GameSimulation` exactly like the graphical Engine — commands
  in, snapshots out — and owns nothing authoritative itself. No SDL window,
  Vulkan context, ImGui, Camera, RenderWorldBridge, render handle or GPU
  pool is ever constructed (include guard: `tests/check_headless_deps.py`;
  configure-time transitive link-closure assertion in `CMakeLists.txt`).
- **Explicit headless interest source.** Residency/streaming is driven by a
  plain point + radius (`--interest`, `--interest-radius`), converted to the
  render-free `WorldStreamSettings` — never a Camera or GPU render distance.
  Per-frame budgets are fixed counts (8 loads / 8 generations), never
  wall-clock derived, so the workload is reproducible.
- **Deterministic generation, both paths.** `--workers 0` uses the new
  `VoxelWorld::generatePendingVoxelsSync` (the synchronous counterpart of
  `generatePendingVoxels`: same nearest-first selection, saved-override
  re-application and GenQueued/Generated events, main-thread only).
  `--workers N` dispatches through the real ThreadPool and **quiesces every
  frame** (bounded by `--timeout`), so worker scheduling never leaks into
  authoritative state: both modes produce the same canonical digest.
- **Canonical digest.** `src/Headless/SimDigest.cpp` hashes documented
  canonical fields only (world identity, observed-area occupancy, player
  body, id-sorted mob population, edit counters — extended by Epic #223
  Phase G/#230 with the inventory, id-sorted item population and id
  allocator, format `FTVHDIG2`) — no pointers, unordered iteration, padding
  or GPU state. Format and determinism scope:
  [`headless-runner.md`](headless-runner.md) §5.
- **Persistence contracts unchanged.** Open/create, sparse edits, flush,
  refusable close, reload and player restore all go through the same
  `GameSimulation`/`WorldPersistence` seams the game uses (issue #180). A
  requested world that fails to open is an error exit (the runner never
  silently falls back to a transient session). Save-on-exit captures the
  player with pinned yaw/pitch 0 (orientation is presentation).
- **CPU-only build slice.** `FT_VOX_GRAPHICS=OFF` skips every graphical
  discovery and target (§8); `FT_VOX_PORTABLE_FP=ON` selects the canonical
  shared FP flag set deferred from Phase A. The `CpuOnlyBuild` CTest runs a
  separate clean configure/build/test as the acceptance gate.

### Verification

`benchmarks/issue-206-phase-b.md`, `benchmarks/issue-207-phase-c.md`,
`benchmarks/issue-208-phase-d.md` and `benchmarks/issue-209-phase-e.md` record
the post-refactor comparisons against the Phase A baselines: full CTest
suite, the `VoxelWorld`, `RenderBridge` and `GameSimulation` CPU-only suites,
per-seed byte identity, persistence/streaming/mob/physics benchmarks, the
in-game benchmark smoke run, and the visual golden comparison (only the
pre-existing `cave_emissive` driver mismatch, #213, with identical metrics).
Phase E additionally **author-verified the Linux build on WSL2 Debian (GCC 14,
llvmpipe, userspace `glslc`)** — not a default-reproducible CI gate. One
behavior-neutral portability fix was required (a missing `<memory>` include in
`ChunkBorders.hpp`); the `glslangValidator` fallback rejects the shaders'
`#include` directives without `GL_GOOGLE_include_directive`, and making that
fallback (or `install_dep.sh`) work is a follow-up outside #209's wiring scope.
`EngineReload` passes in that environment but needs a Vulkan device (on a
headless box it skips, 77). The two Linux test failures (`WorldPersistence`
sparse-diff terrain determinism under GCC, `VisualRegression` on the llvmpipe
software rasterizer) were **verified identical on the base commit** —
pre-existing platform issues for #210/#211, not Phase E regressions. See
`benchmarks/issue-209-phase-e.md` for the exact reproduction steps.
Phase F verification (`benchmarks/issue-210-phase-f.md`): the headless
acceptance driver (sync + async determinism, 3-process save/reload/edit
round-trip, every failure exit code), the dependency guard, and the clean
`FT_VOX_GRAPHICS=OFF` configure/build/test gate — on Windows/MSVC and on
WSL2 Debian (GCC, no Vulkan SDK / SDL3 / shader toolchain discovered).
Epic #223 Phase G closure (`benchmarks/issue-230-closure.md`): the extended headless
acceptance driver (gameplay loop, sync/async parity with items in flight,
3-process gameplay round-trip, grammar/assertion exits), the full graphical
CTest suite, the CPU-only gate, the visual goldens (existing + the
`item_entities` scene), in-game benchmark parity vs the pre-epic baseline
and the determinism report.

## 11. Multi-player simulation ownership (issue #246, Epic #245 Phase A)

Design note for the **A1** ownership split: one shared authoritative world
hosting N independently simulated players, with **no networking** and **no
change to the solo experience**. This is the foundation the later Phase A
issues build on (actor-aware commands #247, multi-observer residency #248,
two-player headless proof #249). Names are deliberately lightweight; the
dependency direction and the single-writer rule are the invariants.

### Two ownership layers

`GameSimulation` remains the single CPU owner of all authoritative gameplay
state, but that state now falls into two layers with different lifetimes:

| Shared world authority (one instance, world-epoch scoped) | Per-player session (one `PlayerState` per `PlayerId`) |
|---|---|
| `TerrainGenerator` (seed + `kGeneratorVersion`) | `physics::PlayerController` (fixed 120 Hz) |
| `VoxelWorld` (residency/streaming, logical edits, publication, `WorldPersistence`) | flight/walk mode + transition status |
| `entities::MobSystem` (fixed 60 Hz, shared population) | spawn/restore body state |
| `items::ItemEntitySystem` (fixed 60 Hz, shared population + id allocator) | creative `selectedBlock` |
| world generation / reload counter, seed, open-world error | `items::Inventory` (36 slots) + hotbar selection |

The shared systems' public contracts are **unchanged** by A1: the world edit
semantics, the mob/item fixed-step discipline, the entity-id allocator and its
high-water, the `RenderWorldBridge` attach seam and the persistence passthrough
all behave exactly as before. Only the *player* half of the facade was split.

### `PlayerId` (src/Sim/PlayerId.hpp)

A strong 4-byte plain value (`uint32_t` ordinal), distinct from the item/mob
`EntityId` (`uint64_t`, a separate per-family allocator) and from `ItemId`
registry ordinals. Semantics mirror the established item-id discipline:

- **Invalid:** `0` (`kInvalidPlayerId`) is never assigned; default-constructed
  handles are invalid and every per-player API answers a safe refusal/no-op.
- **Allocation:** monotonic from 1, assigned at `createPlayer()` call order.
  **Never reused** for the lifetime of the `GameSimulation` instance —
  `removePlayer()` retires an id permanently.
- **Epoch/reload:** `initialize()`/`reload()` are world-epoch boundaries. They
  reset the shared populations and **every session's inventory**, but player
  **sessions survive** (bodies carry over until the host respawns them — the
  historical solo contract) and the id allocator **never rewinds**. A handle
  therefore addresses the same session across epochs, and a retired or
  never-allocated handle can never alias another player's state. Cross-session
  identity persistence (reconnect) is Phase E (#259), not this contract.
- **Exhaustion:** largest assignable id is `kMaxPlayerId`; the allocator rests
  at `kMaxNextPlayerId` and `createPlayer()` refuses with `kInvalidPlayerId`
  instead of wrapping. Population is bounded by `kMaxPlayers` (256) — a
  generous ceiling, **not** a two-player architecture.
- **Iteration:** sessions are kept **id-sorted** (monotonic allocation +
  order-preserving removal), so `playerIds()` and every bulk step/snapshot
  traverse a deterministic ascending order — never a hash order.
- **Storage / reference lifetime (PR #261 review):** sessions are heap-backed
  entries (`std::vector<std::unique_ptr<PlayerEntry>>`): vector growth/erase
  moves only the pointers, so a session's address — and every accessor
  reference/pointer into it (`inventory(id)`, `inventory()`, `selectedBlock()`,
  `playerSettings()`) — is **stable across creation/removal of other sessions
  and across world epochs**. References are invalidated only when the
  referenced session itself is removed (or the simulation is destroyed).
  Entries are allocated at `createPlayer()` only; the per-frame physics paths
  never allocate.

### The local player (solo compatibility, one source of truth)

The **local player** (`localPlayerId()`) is the solo-compatibility anchor:
created at construction (so legacy APIs are total before `initialize()`),
always the first session, and **not removable**. Every legacy single-player
API is a *thin forward* to it — there is no parallel solo state and no second
source of truth. The graphical Engine and the headless runner keep calling the
unchanged legacy surface; the per-player overloads (a `PlayerId` first
argument) are additive.

Per-player forms answer a safe refusal for invalid/unknown ids: queries return
`false` and leave out-params untouched (or a zero/default value), void commands
are no-ops, and inventory commands return the new append-only
`items::ItemResult::InvalidPlayer` without touching any inventory.

### API shape (additive)

- Sessions: `createPlayer`, `removePlayer`, `hasPlayer`, `playerCount`,
  `localPlayerId`, `playerIds(out)`.
- Advance: `advancePlayer(id, dt, cmd, suspended)` (one),
  `advancePlayers(dt, suspended)` (all, neutral command, one scoped collision
  view for the batch). Legacy `advancePlayer(dt, cmd, suspended)` forwards.
- Per-player physics/mode/spawn/restore/capture and inventory commands mirror
  the legacy set with a leading `PlayerId`.
- Snapshots: `playerSnapshot(id, out)`, `inventorySnapshot(id, out)` (bounded,
  in-place). Legacy by-value/local forms forward.

All mutation stays on the **single simulation thread** at the host frame
boundary — A1 introduces no concurrency and no command buffer. Suspension
semantics are per-player and unchanged (accumulators cleared, previous/current
synced, no catch-up burst).

### A1 scope boundary (what is deliberately NOT migrated yet)

To keep the PR reviewable and solo bit-identical, these still act for the
**local player only** and are pinned by tests as the baseline for follow-ups:

- World interaction commands (`breakBlock`, `placeSelectedBlock`,
  `dropFromSlot`), the player-aware `canPlaceVoxel` rule, and item **auto
  pickup** (`advanceItems`) used the local body/inventory in A1 → **migrated
  to actor-aware by #247 (A2), see §12**.
- Chunk residency follows a single observer in A1. A3 adds multi-observer
  interest in §13.
- The solo save format (`player.state` / `inventory.state`) captures the local
  player only; the shared item population + allocator high-water persist as
  before → multi-player checkpoints/reconnect are **#259 (E1)**.

No sockets, no ECS, no renderer refactor and no save-format migration are part
of A1. `ft_vox_core` pulls in no network types.

### Verification

`tests/test_player_sessions.cpp` (CTest `PlayerSessions`, CPU-only, links only
`ft_vox_core`) proves the acceptance list: two players with distinct
positions/velocities/flight/inventories coexist and cannot overwrite one
another; removal leaves the shared world/mobs/items and the other player
untouched; invalid/stale handles never reach another player's state;
deterministic id-sorted iteration; world-epoch continuity; and bit-identical
legacy-vs-local solo behavior. Solo CPU digests and the headless canonical
digest are unchanged (the local forwards reproduce the historical code path
exactly).

## 12. Actor-aware interactions (issue #247, Epic #245 Phase A2)

Design note for **A2**: every gameplay command/query executes in a well-defined
player context so a future server can validate and arbitrate shared-world
actions. A2 builds directly on the A1 ownership split (§11) and changes **no
shared-system contract** (world edits, mob/item fixed step, id allocator,
bridge attach, persistence). Solo behavior stays **bit-identical**.

### Actor-aware command surface (additive)

Each gameplay command keeps its legacy single-player form (a **thin forward to
the local player**) and gains a per-player form taking a `PlayerId` first:

| Command | Legacy form | Actor-aware form |
|---|---|---|
| break | `breakBlock(cell, outEdit)` | `breakBlock(id, cell, outEdit)` |
| place-from-hotbar | `placeSelectedBlock(cell, outEdit)` | `placeSelectedBlock(id, cell, outEdit)` |
| drop | `dropFromSlot(slot, count, dir)` | `dropFromSlot(id, slot, count, dir)` |
| placement query | `canPlaceVoxel(pos, type)` | `canPlaceVoxel(id, pos, type)` |
| targeting | `raycastVoxel(origin, dir, maxDist)` *(privileged host seam)* | `raycastVoxel(id, dir, maxDist)` |

Hotbar selection / move / swap / split / grant were already per-player in A1.
An unknown / retired / invalid handle answers a safe refusal **without side
effects**: `BreakResult::InvalidPlayer`, `PlaceResult::InvalidPlayer`,
`items::ItemResult::InvalidPlayer` (append-only vocabulary), `false` for the
placement query, a no-hit for targeting. The break *effect* is world-shared
(the drop belongs to the shared item population); the actor handle is
validated for authority. Place/drop derive the inventory and eye from the
actor session.

### Server-owned interaction context

The actor-aware forms derive the interaction context from the **server-owned
actor**, never from client-supplied values:

- **Targeting origin + reach.** `raycastVoxel(id, dir, maxDist)` uses the
  actor's authoritative eye (`playerEye(id)`) as the ray origin and clamps
  `maxDist` to `GameSimulation::kInteractionReach` (8 — the historical default
  interaction range). A client may request a *shorter* distance, never a longer
  one, and can never supply the origin. The legacy raw-origin
  `raycastVoxel(origin, dir, maxDist)` remains the **privileged creative/editor
  host seam** (the graphical host passes its camera; clamped only by
  `kMaxRaycastDistance`). Both forms share **one march implementation**
  (`raycastMarch`); for the local actor with `origin == eye` and
  `maxDist <= kInteractionReach` they are bit-identical.
- **Collision occupancy.** The placement rule protects **every** walking
  player body, not just the actor's (below).
- **Inventory.** Place consumes from the actor's inventory; drop removes from
  the actor's inventory and spawns at the actor's eye.

### All-body placement rule

`canPlaceVoxel` generalizes the issue #209 rule: a **solid** block is refused
when it overlaps **any** authoritative **walking** player body; flying players
are exempt (the creative rule) and non-solid blocks never collide. Only
**positioned** sessions participate (see below). Bodies are validated in
deterministic **ascending-`PlayerId`** order, so overlapping players are
considered reproducibly and no player can be entombed by another's edit.
`placeSelectedBlock` gates with this rule internally; the creative `placeVoxel`
seam still does not (hosts gate explicitly). Solo (one positioned session)
reproduces the historical local-body rule bit for bit. The actor-aware
`canPlaceVoxel(id, …)` validates the handle (an unknown session may not even
probe placement) then applies the same actor-independent all-body rule.

**Coordinate validation (review hardening).** Gameplay placement/break validate
the target cell against the supported world envelope — X/Z in
`[SHRT_MIN, SHRT_MAX)`, Y in `[0, WORLD_HEIGHT)` — using **integer** comparisons
(`cellInWorldEnvelope`) **before** any float position or AABB is constructed, so
extreme cells (near the `INT32` bounds) never reach a float→int `floor`
conversion (which would be UB). `placeSelectedBlock`/`breakBlock` answer
`WorldRejected` for out-of-envelope cells; `canPlaceVoxel` additionally rejects
**non-finite** float inputs (and out-of-envelope positions) before `glm::floor`.
Valid gameplay coordinates are unaffected; rejected requests mutate nothing.

### Deterministic shared pickup

`items::ItemEntitySystem::update` gains a multi-candidate overload taking
`std::span<const items::PickupCandidate>` (`{feet, Inventory*}` — a plain
non-owning value; the Items module knows nothing about `PlayerId`). The facade
(`advanceItems`) supplies **every positioned live session in ascending-`PlayerId`
order** (the deterministic priority) via a reserved, refilled-in-place buffer (no
per-frame allocation). Per entity, after the pickup-delay countdown:

- candidates are evaluated **in the supplied order** (the system never
  reorders); the first **in-range** candidate (3D distance to feet ≤
  `pickupRadius`) merges **hotbar-first**;
- a **partial** remainder is offered to the **next eligible** candidate in the
  **same tick**; a **full** inventory skips to the next candidate;
- the **aggregate count is conserved** — an item is never duplicated nor
  silently lost. With one candidate this is bit-identical to the historical
  single-player rule (the legacy `update(dt, world, feet, inventory, suspended)`
  is a one-candidate forward).

### Session positioning gate (review hardening)

A session participates in spatial gameplay (shared pickup **and** player-body
placement checks) only once it is **positioned**. `PlayerState::positioned`
starts **false** for newly created non-local sessions — a default-origin body
never vacuum-collects shared drops or blocks another player's placement before
the host spawns it. The **local anchor is positioned from construction** (the
historical solo lifecycle, bit-identical). A session becomes positioned on the
first successful `setPlayerPosition` / `setPlayerFlight` / `spawnPlayerOnSurface`
/ `restorePlayerState` / `restorePlayerFlight`. The flag persists across world
epochs like the body itself (`initialize`/`reload` carry bodies over); it is not
part of the solo save format. `removePlayer` drops the session entirely (no
stale candidate). `playerPositioned(id)` exposes the gate read-only. Activation
is independent of networking / session authentication.

### Ordering of simultaneous attempts

All mutation stays on the **single simulation thread** at the host frame
boundary, so contention resolves in **command call order**:

- two breaks of one block → first `Ok` (effective state immediately AIR), second
  `WorldRejected`, **exactly one drop**;
- two places of one cell → first `Ok` (consumes 1), second `WorldRejected`
  (consumes nothing);
- shared-drop claims → ascending-`PlayerId` pickup order (above), reproducible
  winner / tie-break;
- per-player inventories never contend across sessions; within a session,
  command order applies.

### A2 scope boundary (deliberately NOT migrated yet)

- **Reach/rate gating of break/place *cells*** (rejecting a far cell at the
  command itself) is server validation owned by **#255 (C3)**; A2 enforces
  reach at the actor-aware *targeting* query and validates actor identity,
  effective voxel state, all-body occupancy, breakability/placeability, id
  exhaustion and inventory. Out-of-bounds cells answer `WorldRejected`,
  out-of-range slots `InvalidSlot`.
- A3 adds multi-observer chunk residency in §13.
- The solo save format captures the local player only → **#259 (E1)**.

No sockets, no ECS, no renderer refactor and no save-format migration are part
of A2. `ft_vox_core` pulls in no network types.

### Verification

`tests/test_actor_interactions.cpp` (CTest `ActorInteractions`, CPU-only, links
only `ft_vox_core`) proves the acceptance list: break/place contention (one
accepted edit, one drop, no double consume); shared pickup priority / partial
merge / full skip / conservation (system-level explicit candidates **and**
facade-level real sessions, reproducible across runs); invalid-actor /
overlapping-player / unknown-terrain / out-of-range rejections without side
effects; actor-aware targeting (eye origin + reach clamp + legacy parity);
per-player id-exhaustion conservation; actor-aware deferred edits; and
bit-identical legacy-vs-actor-aware(local) solo behavior. Review-hardening
regression: extreme `INT32`/boundary cells and NaN/±Inf placement queries are
rejected without UB or side effects, and unpositioned sessions are excluded
from pickup/placement until activated (activation persists across epochs,
removal leaves no stale candidate). `PlayerSessions` pickup boundary updated to
the actor-aware rule. The solo CPU digests and the headless canonical digest are
unchanged; ASan/UBSan and TSan clean.

## 13. Player-driven chunk residency (Epic #245 A3, issue #248)

A CPU host opts into authoritative residency with
`GameSimulation::advancePlayerStreaming` once per streaming tick. Use
`setPlayerInterestRadius(PlayerId, radiusChunks)` to configure a live
session. Zero disables it. The accepted range is 0–8 chunks. Each center
comes from the positioned player's current controller body. Invalid positions
are ignored until valid again. XZ follows the collision world's finite
envelope and Y only needs to be finite. The footprint is a disk of chunk
offsets around the player's current chunk, clipped to the world envelope.
Session removal takes effect on the next tick. Radii default to disabled and
survive world reload with the sessions. A fresh world starts with empty
residency and scheduling state.

`VoxelWorld` caches the union when the roster, center chunks, or radii
change. A coordinate has one record regardless of how many footprints
cover it. Registration and generation each keep a separate round-robin
cursor across ticks. The cursor resumes after the last served `PlayerId`
even if that player left. Within each turn the nearest eligible chunk
wins, with X then Z as the distance tie-break. Overlap can satisfy several
interests with one admission. Dispatch is deterministic given the same
readiness at each tick. Hosts that need reproducible async replay can
quiesce workers between ticks. Worker completion order is asynchronous.

| Work | Default per tick | Hard ceiling |
| --- | ---: | ---: |
| Record registration | 32 | 128 |
| Terrain starts with sync or async generation | 8 | 64 |
| Evictions and their save captures | 16 | 64 |
| Outstanding async terrain jobs, including unpublished completions | | 64 |

Budget values can be lowered to zero. The pool grows incrementally and its
global capacity still applies. Unregistered demand waits when slots run
out, and selection checks the latest union before each admission. A tick
publishes completed generation and applies pending edits before eviction.
Terrain stays unknown to gameplay queries until the owner publishes the
completed backing and its overrides. Publication is scoped to the record
incarnation. Mesh and light worker leases preserve read access to published
terrain while deferring writes and retirement. Queries see pending logical
edits on published chunks even while backing writes are deferred.
Records outside the union retire in registration order when their worker
leases and canonical deferred edits have cleared. A round-robin turn for
stranded canonical edits shares the same generation budget with players so
those edits drain even after the last observer leaves. Deferred canonical
edits resolve their mirrors against the neighbors present when they apply,
including neighbors reloaded while the edit waited. Shell rebuilds sample
the current canonical neighbors. Dirty evictions use the existing bounded
async save queue and retry path.
Streaming never invokes a durability barrier. A transient multiplayer
world keeps sparse applied overrides for its epoch and reapplies them before
publishing regenerated voxels. The map retains one value per edited voxel,
including values in evicted chunks, so memory can grow with the number of
distinct edited cells until the epoch ends. The owner-thread streaming
snapshot exposes `transientOverrideChunks` and `transientOverrideVoxels`
as live counts, including overrides held outside residency.

`VoxelWorld::playerStreamingStats()` reports combined demand, registered
records, load and generation backlogs, out-of-interest records, records
pinned during eviction, outstanding jobs, and work started in the last tick.
The authority's residency is independent of future per-client subscription
windows. Render visibility remains owned by `RenderWorldBridge`, and world
events retain their existing incarnation and revision checks.

The first player-streaming tick selects this mode for the current world.
Call it before editing a transient world. Activation requires pending solo
jobs and edits to clear and refuses a transient solo world with prior logical edits
because that path cannot recover previously unloaded edits. Existing solo
hosts use their legacy streaming entry points. Persistent saves and checkpoints use the existing
format, so rollback requires no save conversion. The CPU-only
`PlayerStreaming` CTest drives the facade with distant, overlapping, crossing,
teleporting, and disconnected players. It checks dispatch replay, bounded
work, retained worker leases, border edits, and checkpoint reopen.

The CPU-only `HeadlessMultiplayer` CTest (issue #249) exercises this union
through `ft_vox_headless` with scripted actor commands and fixed frame
boundaries. It checks command and digest parity across worker counts and
recovers a checkpoint captured with a deferred edit.
[Headless runner guide](headless-runner.md#multiplayer-scripts-issue-249)
