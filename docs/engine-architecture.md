# Engine and world architecture

Authoritative description of the **C++ game engine side** of ft_vox: main loop, settings/UI, camera, chunk streaming, terrain generation, and how those feed the Vulkan renderer.

For pipelines, shaders, and post, see [`vulkan-graphics.md`](vulkan-graphics.md).

---

## 1. High-level layout

```text
main.cpp
  └── Engine::run()
        ├── SDL window (Vulkan)
        ├── VkContext / VkSwapchain / VkFrameContext
        ├── WorldRenderer (+ passes, post, overlays, mobs)
        ├── ThreadPool + ChunkPool
        ├── sim::GameSimulation (authoritative CPU simulation)  [issue #208]
        │     ├── TerrainGenerator + VoxelWorld (WorldChunk records,
        │     │     edits, generation, WorldPersistence)         [issue #206]
        │     ├── physics::PlayerController (120 Hz)
        │     └── entities::MobSystem (60 Hz, passive mobs)
        ├── RenderWorldBridge (derived render projection: mesh/light
        │     dispatch, GPU upload, visibility; references sim->world()) [issue #207]
        ├── StagingRing + GpuResourceRetire
        ├── Camera
        └── ImGuiLayer + GameUI
```

| Area | Directory | Primary types |
|------|-----------|---------------|
| App / loop | `src/Engine/` | `Engine`, `EngineDefs`, `GameUI`, `ImGuiLayer`, `Profiler`, `Benchmark` |
| Simulation facade (CPU-only) | `src/Sim/` | `GameSimulation` (owns world + player + mobs; commands, lifecycle, snapshots) |
| Authoritative world (CPU-only) | `src/World/`, `src/Core/` | `VoxelWorld`, `WorldChunk`, `WorldCollisionView`, `WorldMobView`, `WorldPersistence`, `WorldSave`, `SimContracts`, `ThreadPool`, `WorkloadTelemetry` |
| Render chunk side | `src/Chunk/` | `Chunk` (render handle), `ChunkPool`, `TerrainGenerator`, `StreamHelpers` |
| Entities | `src/Entities/` | `MobSystem`, `MobModel` (passive mobs) |
| Camera | `src/Camera/` | `Camera` |
| Rendering | `src/Renderer/` | `WorldRenderer`, `RenderWorldBridge` (derived world projection), passes, `PostStack`, … |
| Vulkan glue | `src/Vulkan/` | Context, frames, staging, retire |
| Shared constants | `src/utils.hpp` | Chunk size, voxels, biomes, textures, states |
| Network (optional) | `src/Network/` | Client/Server — **not re-wired into Engine UI** |

**Ownership boundary (Epic #204, issues #206 / #207 / #208 / #209):** the CPU-only
`sim::GameSimulation` facade (in `ft_vox_core`) is the single owner of
authoritative gameplay state: the `VoxelWorld` + `WorldChunk` voxel world
(chunk residency, logical edits, terrain publication, persistence), the
120 Hz `PlayerController` and the 60 Hz `MobSystem`. The renderer-owned
`RenderWorldBridge` is a **derived projection** that references the sim's
world (never owns it) and keeps only derived render work (mesh/light
dispatch, GPU uploads, visibility); `Chunk` is a render handle bound to a
`WorldChunk`. The `Engine` is the graphical host (issue #209): SDL/Vulkan/
ImGui, camera, input→command mapping, the suspension policy and the
world-epoch barrier — no authoritative gameplay state. The temporary
`ChunkManager` adapter and `ChunkCollisionView` wrapper were removed in
Phase C; the temporary `ChunkMobWorld` mob adapter was removed in Phase D
(CPU mob seam: `WorldMobView`). See
[`simulation-render-contracts.md`](simulation-render-contracts.md) for the full
boundary, the world event stream and the adapter removal status.

---

## 2. Engine loop (`Engine`)

File: `src/Engine/Engine.hpp` / `Engine.cpp`.

### Ownership

`Engine` owns (unique_ptrs / members):

- **Windowing:** `SDL_Window *`, size, mouse capture, pause  
- **Vulkan:** `VkContext`, `VkSwapchain`, `VkFrameContext`, `ImmediateCommands`  
- **Render:** `WorldRenderer`, ImGui layer, `GameUI`  
- **Simulation:** `sim::GameSimulation` (authoritative: `TerrainGenerator`, `VoxelWorld`, `PlayerController`, `MobSystem`) + `ThreadPool`, `ChunkPool`, `RenderWorldBridge` (derived projection referencing `sim->world()`)  
- **GPU streaming helpers:** `StagingRing`, `GpuResourceRetire`  
- **View:** `Camera`  
- **Player:** owned by the simulation — fixed-step CPU body, gravity, collision and swimming; the Engine maps SDL input to `sim::PlayerCommand` and the camera follows the interpolated eyes in walking mode (issue #208)
- **Entities:** owned by the simulation — CPU-only passive mob simulation (see §9); the Engine takes mob snapshots, samples their entity light from the bridge and pushes them to `WorldRenderer::setMobs` each frame
- **Settings:** `RenderSettings`, `ShaderParameters`, `RenderTiming` (the seed lives in the simulation)  
- **Benchmark:** `Benchmark`  

### Per-frame outline (`Engine::run`)

`run()` is the documented frame orchestrator (issue #209, Epic #204 Phase E):
sixteen numbered steps, with the cohesive blocks extracted into named methods
(`tickMobs`, `updateFrameUniforms`, `recordFrame`, `updateWindowTitle`).
Matches `Engine.cpp` order:

1. **Profiler begin** — `GetProfiler().beginFrame()`  
2. **Events** — `handleEvents`: SDL poll → ImGui routing + simulation commands (placement validation is `sim->canPlaceVoxel`; the accepted edit publishes through the bridge)  
3. **Viewport** — minimized/hidden windows skip the whole frame (sleep + frame-clock resync); deferred swapchain recreation (resize / VSync / WSI invalidation) refreshes frame synchronization, `WorldRenderer` and ImGui before acquisition  
4. **Acquire** — `VkFrameContext::beginFrame` → image index + command buffer. A failed acquire resyncs the frame clock (the blocked stretch is never simulated)  
5. **Frame clock** — paced dt sampled acquire-to-acquire (`frame_pacing::computePacedDeltaTime`), so WSI stalls never feed gameplay; once-per-second window title update  
6. **Benchmark** — `tickBenchmark` (policy; may reload the world through the epoch barrier)  
7. **Simulation** — `processInput`: SDL → `sim::PlayerCommand`; walking physics steps inside `sim->advancePlayer`, flight follows the camera via `sim->setPlayerPosition`; the camera follows the interpolated render eye  
8. **Day cycle** — `tickDayCycle` updates `ShaderParameters` sun/moon/day factors (a **presentation** parameter, not authoritative world time; pause gates it here, issue #208)  
9. **`tickStreaming`** (derived world projection; CPU only, no GPU work)  
   - `processFinishedJobs` / `processDeferredReleases`  
   - `updateStreaming` → budgeted `processChunkLoading` / `generatePendingVoxels` / `meshPendingChunks` (`maxStreamMs` envelope)  
   - `updateEntityLightCaches` — dedicated async light-cache-only jobs for meshed chunks entering the entity-light radius (issue #172; never remeshes a valid mesh)  
   - `updateVisibility` → `collectDrawList` / `collectShadowList`  
   - Computes `uploadBudgetThisFrame` (uploads are **not** done here)  
10. **Mobs** — `tickMobs`: `sim->advanceMobs` (fixed-step AI/physics, spawn/retire over the CPU-only `WorldMobView`), then `sim->mobSnapshots(mobStates)` (filled in place) + per-mob entity light from `RenderWorldBridge::sampleSmoothedLight` → `WorldRenderer::setMobs`  
11. **Highlight** — raycast block under cursor (`updateHighlight`)  
12. **Frame slots** — `resourceRetire.beginFrame`, `arenas.beginFrame`, `stagingRing.beginFrame` (fence already waited)  
13. **ImGui UI build** — `imgui->beginFrame` / `drawUi` / `endFrame` (CPU only; draw later)  
14. **UBO** — `updateFrameUniforms`: underwater sample + local water-surface scan + `WorldRenderer::updateFrameUBO` (which also runs `MobRenderer::prepare`: frustum masks + per-part transforms for camera and cascades)  
15. **Record** — `recordFrame` → `WorldRenderer::recordFrame`:  
    - **preRecord:** `uploadPendingMeshes` + transfer→vertex barrier — the **only** GPU upload point, inside frame recording (no device idle)  
    - **ShadowPass → OpaquePass** (opaque chunks + **mobs then overlays inside OpaquePass** + **mobs inside every shadow cascade**) **→ WaterPass → SkyPass → PostStack**  
    - **imguiDraw:** `imgui->recordDraw` onto swapchain after composite; ImGui samples the **currently published** biome-map texture  
    - **postImGuiRecord:** pending biome-map texture upload + publication of the matching `BiomeRegionGrid` for the **next** UI frame. Biome-map uploads intentionally occur after ImGui so a frame never samples new map pixels with overlays built from the previous map grid (architectural invariant, issue #186/#191)  
16. **Submit / present + telemetry** — `VkFrameContext::submitAndPresent`, `publishFrameTelemetry`, profiler end, copy scopes into `RenderTiming`, benchmark sample  

Bootstrap: `generateInitialArea` fills a small radius around spawn synchronously so the first frames are not empty.

`reloadWorld(seed)` rebuilds terrain generation state (device idle) for tools/benchmarks.

### Suspension policy (host-owned, issue #209)

The simulation implements freeze semantics only (accumulators cleared,
previous/current synced, **no catch-up burst** on resume); the Engine decides
WHEN through the single `simulationSuspended()` predicate (paused ||
unfocused || benchmark-active):

| State | Player | Mobs | Streaming / uploads | Day cycle |
|---|---|---|---|---|
| Pause (P / UI) | suspended | suspended | visibility + draw lists stay live; no job dispatch, no uploads | frozen (atmosphere still tracks settings) |
| Focus lost / gained | suspended on BOTH transitions, buffered input dropped | suspended while unfocused | unchanged | unchanged |
| Minimized / hidden | whole frames skipped; frame clock resyncs AFTER the sleep so the skipped stretch never feeds a timestep | — | — | — |
| Benchmark | suspended (camera benchmark-owned), input locked | disabled | normal streaming (the measured workload) | normal |
| Inspection (`--view`) | `processInput` skipped (fixed daylight camera) | suspended | normal streaming | frozen at the fixed daylight value |

### World-epoch barrier (issue #209)

Every authoritative world transition (`initializeNoiseGenerator`,
`reloadWorld`) runs `Engine::resetWorldProjection()` BEFORE detaching the old
render bridge: draw/shadow lists (they reference the old bridge's handles),
mob presentation states (also pushed empty to `WorldRenderer::setMobs`), the
block highlight, the upload budget, the UI biome cache and the biome map are
dropped so no stale derived snapshot survives the transition. The simulation
bumps `worldGenerationId()` across the transition; the replacement bridge is
stamped with that epoch at attach (`RenderWorldBridge::worldGenerationId()`).
Async jobs are quiesced by the bridge destructor against the still-live old
world; a refused persistent close aborts the reload with the old world
authoritative and renderable (issue #180 contracts). GPU lifetime stays
graphical: device-idle + retire flush around the transition, uploads only
inside frame recording.

Benchmark memory ownership, mesh allocation/retirement, draw workload and worker
stage measurements are documented in [workload-telemetry.md](workload-telemetry.md).

---

## 3. Settings and UI

### `EngineDefs.hpp`

| Struct / enum | Purpose |
|---------------|---------|
| `ShaderParameters` | Fog, sun/moon, ambient/diffuse, day cycle, water knobs, outdoor grade |
| `RenderSettings` | Render distance (min/max blocks), `streamFrontBias`, stream rates (`load/gen/mesh/upload` per sec), shadow distance / cascade far, `maxStreamMs`, wireframe/borders/vsync |
| `PostProcessSettings` | Bloom, SSAO, god rays, exposure/tonemap, spatial AA (dedicated FXAA 3.11 pass — Low: off, Medium+: on, issue #143), grain, vignette, underwater + `underwaterSurfaceY` (local water-surface scan feeds the composite submersion blend), **quality preset** |
| `GraphicsQualityPreset` | Low / Medium / High / Cinematic — `applyPreset` only remaps existing post knobs |
| `RenderTiming` | Legacy flat timings filled from hierarchical profiler |

### UI

- **`UiShell`** — application dockspace with passthru central game view,
  `ft_vox / World / View / Developer / Help` menu bar, right-side status strip
  (narrow-width degradation, unit-tested planner), default developer layout +
  reset action; canonical `ui::windows` titles shared with every `Begin()`
  call (issue #183)
- **`UiTheme` / `UiScale` / `UiShortcuts` / `UiStatus`** — centralized theme
  (`ui::applyStyle(scale)`, no-drift rebuild), 100–200% UI scaling persisted
  in `imgui.ini`, shared presentation helpers, shortcut display metadata
- **`GameUI`** — ImGui shell: main menu bar, read-only Status Overlay (F1,
  density Off/Minimal/Detailed), interactive Player / Gameplay panel
  (Movement/Camera/Interaction/World toggles), World/biome map, Help,
  on-screen hints, F-key shortcut routing and biome-map plumbing (issue #184).
  The player reaches UI surfaces only as the read-only `playerui::PlayerSnapshot`
  value carried by `GameUIFrame`
- **`DebugUI/` (`src/Engine/DebugUI/`)** — developer console panels (issue #179),
  one domain per translation unit, all consuming read-only snapshots in
  `debugui::UiState` instead of reaching into engine subsystems:
  - **Overview (F8)** — frame/streaming/memory summary plus sustained health
    warnings (hysteresis-based, no one-frame-transient alarms)
  - **Graphics (F2)** — settings only, organized as category navigation
    (General / Display / Lighting / Atmosphere / Shadows / Water / Post /
    Resources, issue #185): Display owns VSync + present mode, Shadows owns
    shadow distance / cascade far; **Render Debug (F12)** — diagnostic views
    (shadow/water/SSAO debug), environment readouts (day factors, underwater),
    exposure readout, per-pass GPU cost, writing
    the same `shadowDebug`/`waterDebugView`/`ssaoDebugView` state as before
  - **Streaming (F3)** — operational streaming control (issue #186):
    Distance (view/full-quality/front-bias + estimated resident capacity),
    Pipeline (first-class CPU budget, documented Conservative/Balanced/
    Aggressive presets with truthful Custom detection, advanced stage
    rates) and compact read-only live health whose warnings follow the
    sustained DebugHealth monitors (never one-frame spikes). Deep pool,
    timing, device and history diagnostics stay in Overview, Performance,
    Memory and Help ▸ About
  - **Performance (F7)** — CPU hierarchy + flat sortable scope table
    (last/avg/peak + click-to-plot), GPU pass view, worker jobs, spikes
  - **Player Diagnostics** (Developer menu) — raw physics solver counters
    (steps, queried cells, dropped fixed steps), motion flags and camera
    readout removed from the gameplay surfaces (issue #184)
  - **Chunk inspector (F9)** — per-chunk lifecycle/mesh/light-cache/upload
    state for the chunk under the player/target or manual coordinates, plus an
    opt-in, bounded (`RenderWorldBridge::kChunkEventRingSize`) main-thread event
    trace (`load/genQueued/genDone/meshQueued/meshDone/edit/gpuCommit/unload`)
  - **Memory (F11)** — CPU pools, GPU mesh resources, mesh arena, staging /
    retire queues, workload events and mesh stage timings
  - **Benchmark** — scored-run controls (report window unchanged)
  - Pure logic (formatting, `MetricHistory` ring, `HealthMonitor`, snapshot
    structs) lives in `DebugUiCore` and is unit-tested headlessly
    (`tests/test_debug_ui.cpp`)
- **`ImGuiLayer`** — SDL3 + Vulkan backends, dynamic rendering  
- **`Profiler`** — hierarchical CPU scopes (F7-style panel)  
- **`Benchmark`** — scripted runs; reports written to gitignored `benchmark-results/`  

Telemetry: `telemetry::Registry::snapshot()` stays the destructive,
capture-terminal read for benchmark finalize. The UI never calls it; panels
read `sampleLive()`, a non-destructive bounded copy of gauges/peaks/events/
stage totals that does not touch capture epochs or benchmark samples. The
main-thread memory gauges are published every frame by
`Engine::publishFrameTelemetry()` (profiled as `TelemetryPublish`), so the
console works without running a benchmark. Console sampling is throttled to
10 Hz and only runs while a consumer panel is open.

Controls summary lives in root `README.md` (WASD, break/place, borders, etc.).

### Profiler capture reset

Profiler capture reset (`Profiler::clearHistory`, also used after world reload)
clears the displayed frame/scopes, history, averages, spikes, worker snapshot,
and pending samples from the previous capture epoch. Registered worker names
and the capture enabled/paused setting are preserved. Reset and snapshot calls
belong to the capture/main thread; worker submission may run concurrently.

Each asynchronous job captures `captureEpoch()` at submission and supplies it
to `addWorkerSample(name, ms, epoch)`. A result from an old epoch is rejected
even if its job finishes after reset. Bucket mutexes protect count/time pairs;
new-epoch samples arriving during reset are retained. The two-argument overload
uses the epoch at the time of submission and is intended for immediate samples.

When clearing inside an open frame, its instrumentation stack remains valid
until scopes close, but that entire frame is discarded. History resumes at the
next full completed frame. Benchmark sampling skips the discarded frame, so a
zero-warmup reload cannot measure its pre-reset scopes or worker samples.
Reproduce that path with `ft_vox --seed 42 --benchmark 5 --benchmark-warmup 0`.

---

## 4. World constants (`utils.hpp`)

| Constant | Value | Meaning |
|----------|------:|---------|
| `CHUNK_SIZE` | 16 | X/Z voxels per chunk |
| `CHUNK_HEIGHT` / `WORLD_HEIGHT` | 256 | Y voxels |
| `CHUNK_VOLUME` | 16×256×16 | Flat voxel storage length |
| `SEA_LEVEL` (terrain) | 64 | `TerrainGenerator::SEA_LEVEL` |

### Voxel and mesh types

- **`Voxel`** — `uint8_t type` (block id / `TextureType`)  
- **`Vertex`** — 16-byte packed voxel vertex (chunk-local position, packed normal/tex/AO/lighting, UV, packed biome color) + per-draw `VoxelDrawData` world origin (issue #110)  
- **`TextureType`** — block atlas indices (stone, dirt, grass, leaves, water, ores, …)  
- **`BiomeType`** — ocean, beach, plains, forests, deserts, tundra, mountains, …  
- **`ChunkState`** — lifecycle for streaming (unloaded → generated → meshed → GPU-ready, plus transit flags)

---

## 5. Chunk (`Chunk`) and the authoritative record (`WorldChunk`)

Files: `src/Chunk/Chunk.hpp` plus an implementation partitioned by
responsibility (issue #181): `Chunk.cpp` (construction/moves, record binding,
render reset, generation wrapper), `ChunkEditing.cpp` (render-side edit
invalidation), `ChunkLighting.cpp` (light sampling, chunk-wide light field,
light-cache packing, neighbor light halo), `ChunkMeshing.cpp` (full/LOD greedy
builds), and `ChunkGpu.cpp` (mesh result publication, arena replacement
phases, indirect draw cache, retirement). The worker-side translation units
share the thread-local scratch buffers declared in the internal
`ChunkMeshScratch.hpp`. These files implement one class — every build target
compiling one must link them all.

**Issue #206 (Epic #204 Phase B) split the authoritative voxel state out of
`Chunk` into `World/WorldChunk.hpp/.cpp`** (CPU-only, in `ft_vox_core`).
`WorldChunk` owns the voxel backing, occupancy metadata, border shell,
persistent-edit map, `sim::ChunkIdentity` (generation + revision), lifecycle
state, in-transit flag and per-column generation outputs, and implements the
authoritative operations (`setVoxel`, `generateTerrain`, storage lifecycle,
border rebuild, reset/recycle). `Chunk` is now a **render handle**: it binds to
a `WorldChunk` (world-owned in game; privately owned for standalone
tests/tools) and forwards every voxel read/write, keeping only derived render
state. See [`simulation-render-contracts.md`](simulation-render-contracts.md)
§10 for the ownership table and event stream.

### Data

Render handle (`Chunk`):
- GPU: shared-arena vertex/index ranges per section + whole-chunk LOD ranges,
  indirect draw caches, deferred release
- Pending CPU mesh payload (pooled `MeshBuildResult`), entity light cache
  (pooled `ChunkLightStorage`), dirty-section mask, visibility

Authoritative record (`WorldChunk`):
- Flat voxel array (the canonical occupancy source: `type != AIR`) + compact
  per-section non-air counters for empty-slab skipping (issue #105)
- Neighbor **shell** voxels for correct greedy meshing at borders
- Biome grass/foliage colors + biome/height per column
- Persistent-edit map, identity counters, lifecycle state

### Operations

| Method | Role |
|--------|------|
| `generateTerrain` | Fill voxels via `TerrainGenerator` (record) + render re-arm |
| `generateMesh` / `generateLODMesh` | Greedy meshing; far chunks may use LOD mesh |
| `uploadToGPU` | Sync upload (bootstrap/tests) |
| `uploadToGPUAsync` | Staging ring + fresh arena ranges, frame-aware retire of replaced ranges (hot path) |
| `collectOpaqueDraws` / `collectWaterDraws` | Append cached `VkDrawIndexedIndirectCommand` descriptors per live section (rebuilt at upload-commit, issue #109 / #122) |
| `deleteVoxel` / `placeVoxel` | Edit + remesh flags (forwarded to the record; the manager's edit API goes through `VoxelWorld`) |
| `rebuildBordersFromNeighbors` | Face correctness across chunk edges (record) |

### Lighting on mesh

At mesh time, sky light is cast down open columns then **flooded** into caves (attenuation). Block light propagates from emissive voxels. Packed into vertex attributes for the terrain/water shaders (see graphics doc for cave fill / sun shadow mute).

### Greedy meshing face keys (issue #106)

`Chunk::buildMeshRanged` materializes the face inputs of every slice cell
**once** into compact per-cell face keys (thread-local workspace, reserved
per worker): the voxel pair straddling the face plane plus the biome owner
color of each side. The greedy width/height expansion then evaluates the
merge rules on these key fields — no voxel/shell re-sampling and no biome
array lookups per probe. The key carries the raw pair (not just the cell's
classified face) because the merge rules accept a candidate whose own
visible face is the opposite face of a different transparent block, and they
merge faces fronting air with faces fronting transparent blocks; a pure
"same face" equality key cannot express those boundaries. AO and light stay
out of the key — they are sampled at the final merged quad corners. The
merge semantics are pinned byte-exact by `tests/test_mesh_facekey.cpp`
(deterministic scenes plus the `--hash-corpus` dev tool).

### Sectioned meshes (issue #107)

The logical chunk stays 16×256×16, but its full-quality render mesh is one
payload per vertical **16³ section** (the same 16-slab tiling as the
occupancy metadata). Section `s` owns every greedy face whose owning voxel
lies in `y ∈ [16s, 16s+15]`; the owner-side gating clips the greedy rect to
the section, so quads never cross a section boundary and each section can be
rebuilt and re-uploaded independently (a quad that would have merged across
a boundary is split — a small, measured quad increase).

- **Dirty granularity**: an edit marks its own section, the section above/
  below when the voxel sits on a section Y boundary, the horizontal
  neighbor's `y/16` section for x/z border writes (mirror), and a
  conservative light range: emissive edits spread ±14, other edits cover the
  skylight column down to its first blocker. Lighting itself is recomputed
  **chunk-wide** for every build, so rebuilt sections sample exactly the
  field a whole build would produce — no seams at 16-block boundaries
  (regional lighting is future work; deep horizontal flood beyond the
  conservative range is its known frontier).
- **GPU layout**: chunk geometry lives in four shared device-local arenas
  (opaque/water x vertex/index) owned by the WorldRenderer; each section
  holds an aligned range pair (`Chunk::SectionGpuSlot`). Published ranges
  are immutable: a remesh allocates fresh ranges for the dirty sections,
  atomically swaps in the replacement slot table, then retires the replaced
  ranges frame-aware. Draw submission is indirect: one command per live
  section (section-local indices rebased by the command's `vertexOffset`),
  grouped by arena page pair.
- **Builds**: `buildMesh(result, gen, rev, sectionMask)` builds exactly the
  masked sections in one worker job (batched — no tiny-task overhead for
  initial generation, which masks all 16 sections); an empty section skips
  meshing entirely and clears its slot content on upload. The dirty mask is
  captured at dispatch like the mesh identity, re-armed when a publish is
  rejected, and expanded to a whole-chunk build when no sectioned GPU state
  exists yet (LOD promotion, first build) or while an earlier full-quality
  result still awaits upload.
- **Tests**: `tests/test_chunk_lifecycle.cpp` pins the dirty-propagation
  contract and the partial-rebuild ≡ full-rebuild section equivalence;
  `tests/test_mesh_facekey.cpp --edit-bench` reports the edit scenarios
  (mid-section, Y boundary, chunk border, burst, spread) against
  whole-chunk rebuilds.

---

## 6. ChunkPool, record pool and ThreadPool

### `ChunkPool` (`ChunkPool.hpp`) — render handles

- Preallocated / recycled `Chunk` render handles
- Avoids allocator thrash when streaming
- Acquisition uses `Chunk::ResetMode::ForGeneration`: lifecycle, mesh, and
  cache state is reset, but voxel clearing is deferred to `generateTerrain()`.
  Consumers must wait for generation before reading voxel data. Release uses
  the default full reset. Pool acquire/release only ever see **unbound**
  handles (the adapter unbinds before releasing).
- Grows with view distance / pressure (see git history for pool growth fixes)

### `VoxelWorld` record pool — authoritative backing (issue #206)

- Pointer-stable `WorldChunk` records with the same back-pressure contract as
  `ChunkPool` (acquire returns null when the free list is empty; incremental
  growth via `chunkPoolGrowStep`, hard cap `kMaxChunkPoolCapacity`). The world
  grows it toward `estimateChunkPoolCapacity(maxRenderDistance)` inside
  `updateStreaming`, so a headless consumer needs no external growth.
- Owns the `VoxelPool` (64 KiB voxel blocks) and `BorderPool` (17 KiB border
  blocks) the records borrow from; both are CPU-only and live in `ft_vox_core`.

### `ThreadPool` (`Core/ThreadPool.hpp`, relocated from `Engine/` in #206)

- Work-stealing workers for **terrain generation** (world side) and
  **meshing / light caches** (render side)
- Priorities via task priority helpers (near camera first)
- Biome maps use up to eight Low-priority tile lanes, yielding back to the pool
  after each tile. High/Normal work is searched across all queues first. The
  final tile publishes via an atomic completion count; workers never wait for
  child tasks. GameUI still permits only one map build at a time.

**Rule:** GPU upload, VMA destroy, and descriptor/buffer free related to live draws happen on the **main thread** after appropriate retire delay — never free mesh buffers still referenced by in-flight frames.

---

## 7. RenderWorldBridge (derived projection) + VoxelWorld (streaming)

Files: `src/Renderer/RenderWorldBridge.hpp` / `RenderWorldBridge.cpp` (derived
render projection), `src/World/VoxelWorld.hpp` / `VoxelWorld.cpp` (authoritative
world).

**Issues #206 / #207:** streaming residency, terrain publication, logical edits
and persistence live in `VoxelWorld` (CPU-only). The renderer-owned
`RenderWorldBridge` **references** the host-owned world (never owns it) and runs
the derived render pipeline; it replaced the temporary `ChunkManager` adapter
(removal status:
[`simulation-render-contracts.md`](simulation-render-contracts.md) §10). It
converts `Camera` + `RenderSettings` into plain world inputs, drains the world
event stream to keep its render handle map in sync (and projects already-resident
records on attach). The per-frame sequence is unchanged:

### Pipeline

CPU work in `Engine::tickStreaming` (before swapchain acquire):

```text
processFinishedJobs          (world gen publish → render mesh/light publish → deferred edits)
processDeferredReleases      (retire GPU → render pool recycle)
updateStreaming              → world: enqueue loads / mark unload; bridge: bind/unbind handles
processChunkLoading          (world record acquire + render handle bind)
generatePendingVoxels        (world: async terrain jobs)
meshPendingChunks            (render: async mesh; shell filled via world on main)
updateEntityLightCaches      (render: async light-cache-only jobs; issue #172)
updateVisibility + collectDrawList / collectShadowList
```

GPU mesh upload is **not** in `tickStreaming`. It runs later inside `WorldRenderer::recordFrame` **preRecord** via `uploadPendingMeshes` (after acquire, same command buffer as draws).

### Distance model

- **`minRenderDistance`** (blocks, default 192) — near band: full mesh  
- **`maxRenderDistance`** (blocks, default 512) — stream/unload radius; far band may use **LOD mesh**  
- **`streamFrontBias`** (default 0.30, capped at `kSafeMaxStreamFrontBias` = 0.55) — view-direction load bias: chunks ahead of the camera count as closer (load first). The cap is not arbitrary: ahead reach is `maxRenderDistance / √(1−bias)`, so `bias ≤ 1 − 1/1.5² ≈ 0.556` (×1.49 ahead at the cap) is the largest value that keeps `desired footprint ⊆ unload hysteresis radius` (1.5× view distance) — see `kSafeMaxStreamFrontBias` in `StreamHelpers.hpp`
- Load queue is **distance-prioritized** (not pure FIFO) and maintained **event/increment driven** (issue #108). The desired-chunk footprint is stored as per-row X spans (`ChunkDesiredFootprint` in `StreamHelpers.hpp`): stationary frames — and movement that stays within the current 4-block streaming anchor — do **no** candidate maintenance at all; crossing an anchor or a chunk boundary triggers an O(r) incremental reconciliation (`FootprintDiff`); a camera rotation beyond ~10° (`kStreamHeadingCosThreshold`) matters only while `streamFrontBias > 0` and re-reconciles the footprint from full rows. Full rebuilds are reserved for teleports, render-distance or front-bias changes (compared via `streamFrontBiasChanged`, epsilon + clamp-normalized). Sorting happens once per streaming reconciliation, because surviving candidates also need their biased distance refreshed; additional sorts occur only after pool-acquisition retries. Consumption uses a head index instead of shifting the vector each tick (`qLoad` peak in benchmark reports is the live queue size, excluding the consumed prefix). Out-of-range unloads run on chunk-cross / settings change and at least every `kUnloadCheckIntervalFrames` (60); unload hysteresis is 1.5× view distance, unchanged. Dispatch counters (`StreamingMaintenanceStats`, reported per benchmark) cover the zero-work / incremental / heading / full split, rows visited per reconciliation and candidate churn; benchmark reports the **measurement window only** (warmup excluded).

### Draw lists

| API | Output |
|-----|--------|
| `collectDrawList` | GPU-ready + frustum-visible chunks |
| `collectShadowList` | GPU-ready casters within `shadowDistance` XZ |

### Edit API

`deleteVoxel` / `placeVoxel` / `isVoxelActive` / `getChunkAtWorldPos` for dig/build and raycast from `Engine`.

### Concurrency

- `shared_mutex` around chunk map  
- Chunks mark `inTransit` while worker jobs run  
- Main thread joins futures and clears transit flags  

---

## 8. Terrain generation (`TerrainGenerator`)

File: `src/Chunk/TerrainGenerator.hpp` / `TerrainGenerator.cpp`.

### Stack

- **FastNoise2** node graphs (domain-warped continental, peaks/valleys and
  ridge fields; temperature, humidity, caves and rivers)
- Seeded; **thread-local** generators via `getThreadLocal(seed)` for worker safety  
- `NOISE_OFFSET` avoids origin symmetry artifacts  
- A 28x28 extended window keeps thermal erosion, biome tint smoothing,
  border shells, and cross-chunk vegetation deterministic around the 16x16 core.
- Terrain shaping includes erosion-driven badlands/cold plateaus, per-biome
  3D surface perturbation, sharp mountain ridges, and flat two-block-deep
  river channels with cold-climate ice.
- Underground shaping combines the existing cheese and ravine fields with two
  ridged spaghetti fields and a low-frequency cavern field. The additional
  fields are sampled on a world-aligned half-resolution 3D grid below y=96,
  shared by the chunk core and border shell to bound cost and prevent seams.
- A low-frequency 2D aquifer field selects dry or water-filled cavities and a
  stable local water table (y=20..36). Lava owns the deep table through y=11;
  volcanic columns may contain deterministic pockets through y=24.
- Deepslate transitions gradually below y=24. Ore start heights use uniform,
  high-biased, mid-triangular, or deep-triangular distributions; emerald is
  restricted to mountain columns and badlands receive extra gold candidates.
- A deterministic post-pass adds cube-based dripstone, wet-cave moss, and
  volcanic magma without specialized geometry.

### Output (`ChunkData`)

| Field | Meaning |
|-------|---------|
| `voxels` | Full chunk block types |
| `borderVoxels` | 1-thick shell for neighbors |
| `biomes` | Per-column biome |
| `heightMap` | Surface height per column |
| `grassColors` / `foliageColors` | Packed RGBA for mesh tint |

### Biomes

`BiomeConfig` per `BiomeType`: surface/subsurface/underwater blocks, tree and
ground-cover densities, grass/foliage colors, snow/cacti flags, and 3D surface
perturbation amplitude.

Biome selection is a multi-noise pipeline:

1. Continentality selects ocean, coast, flatland, hill, or mountain terrain.
2. A temperature × humidity matrix selects the primary climate biome.
3. Weirdness, erosion, and local relief select structured variants and rare
   biomes without changing chunk-order determinism.
4. River noise overrides land where the carved channel reaches sea level.

The 31 biomes include the original climate set plus flower meadows, cherry
groves, autumn and redwood forests, mangrove swamps, bamboo jungles, moors,
glaciers, frozen rivers, volcanic terrain, oases, mushroom fields, and coral
reefs. Their map colors are indexed by `BiomeType` in `GameUI.cpp`; the World
panel displays the complete legend in a scrollable child.

Phase 3 feature placers are world-coordinate deterministic and evaluated from
the cross-chunk halo. They include cherry trees, 2×2 redwoods, mangroves,
bamboo, palms, giant mushrooms, coral heads, temperate-ocean kelp, and volcanic
surface patches. Ocean floors use deterministic four-block sand, gravel, and
clay patches. Lily pads and seagrass remain deferred until flat/cross-plane
feature geometry exists.

Notable constants: `SEA_LEVEL = 64`, `BEDROCK_LEVEL`, and
`MAX_TREE_RADIUS = 4`.

### Terrain block textures

`TextureType` is append-only. Every value before `COUNT` has a matching
`kBlockLayers` entry in `MinecraftTextures.hpp`. Phase 3 bundles the required
Minecraft-compliant textures for cherry and mangrove wood, bamboo, mushroom
blocks, basalt, blackstone, magma, and five coral blocks under
`ressources/textures/`; an external compliant resource pack may override them
through `assets/minecraft/textures/block/`.

Magma is emissive and participates in propagated block lighting. Cherry and
mangrove leaves use the shared transparent foliage/wind material policy.

### Queries

- `getBiomeAt(worldX, worldZ)`  
- `getBiomeRegion(...)` — batch grid for World map UI (SIMD-friendly uniform grid sampling)  
- `test_terrain --histogram [size] [step] [seed]` — manual biome calibration
- `test_terrain --height-histogram [chunk-grid-size]` — height/performance calibration
- `test_terrain --profile [chunk-grid-size] [seed]` — per-stage terrain timings
- `test_terrain --world-stats [sample-grid-size] [seed-count]` — combined world report
- `ft_vox --seed N --benchmark seconds` — automated wide-orbit streaming benchmark

Generation is **horizontal infinite** in practice (chunk X/Z); vertical extent is fixed chunk height.

---

## 9. Passive mobs (`Entities/`)

Files: `src/Entities/MobSystem.*` (simulation), `src/Entities/MobModel.*` (articulated box models), `src/World/WorldMobView.hpp` (CPU world seam), `src/Sim/GameSimulation.*` (owner/advance). Rendering side: `MobRenderer` — see [`vulkan-graphics.md`](vulkan-graphics.md).

Four passive species (`MobSpecies`: Cow, Pig, Sheep, Chicken) live in the world with a
simple ambient AI. There is intentionally **no** combat, health, breeding, babies,
death, or persistence: the population is deterministic from the world seed and
recreated as chunks stream in and out.

### Simulation contract

- **Fixed step 60 Hz** with up to 8 catch-up steps per frame; visual state is
  interpolated (`renderStates`) from previous/current positions and yaws. Steps
  beyond the budget are counted (`droppedSteps`), never accumulated. Suspended
  (positions frozen, accumulator cleared) while paused, unfocused, mobs toggled
  off, or benchmarking.
- **Reuse of player physics:** mobs are `physics::Body` volumes moved through the
  same `VoxelCollisionWorld` solve as the player, sampled via `WorldCollisionView`.
  No chunk pointer survives an update: the `WorldMobView` seam is scoped per frame
  and must be destroyed before streaming/edit publication.
- **`tickMob`** is a free function (one fixed-step controller) so tests drive it
  without an engine: walk/idle cycles (3–8 s / 2–6 s), progressive yaw turns,
  neighbor separation, cliff/water probes one body-width ahead, one-block steps
  via a physical hop (never a teleport), buoyancy + shore seeking in water, and
  a re-target when stuck or when terrain is unavailable (`waitingForTerrain`).

### Spawning and population (all constants in `MobSettings`)

| Knob | Value |
|------|-------|
| Capacity | 48 mobs |
| Group size | 2–4 of one species |
| Spawn band | 24–80 blocks from the observer, capped by loaded radius |
| Retire | beyond 112 blocks, or when the mob's terrain sample is unavailable |
| Spawn scan | every `spawnInterval` (0.5 s), ≤`maxGroupAttemptsPerScan` (4) group attempts per scan |

- Group eligibility, species and member layout are deterministic functions of
  the **world seed + chunk coordinates** (no shared global RNG sequence), and a
  processed group cannot duplicate while it stays in the active zone. The exact
  active population may still vary with terrain publication order and observer
  streaming state (capacity, spawn band and scan budget decide the rest).
- `WorldMobView::surface` only accepts feet positions on **grass with a fully
  air column above**, in temperate biomes (plains, flower meadow, forest, birch,
  autumn forest, cherry grove), read from **published voxels** — never inferred
  from procedural height.

### Render handoff

`MobRenderState` (species, interpolated position/yaw/gait/stride, idle look,
flap) is a plain CPU struct produced by the simulation's bounded snapshot
(`sim::GameSimulation::mobSnapshots`, filled in place like
`MobSystem::renderStates`); the Engine samples each mob's entity light from
the bridge, and `WorldRenderer::setMobs` takes the result. `MobRenderer::prepare` builds each part transform once and
scatters copies into per-pass contiguous instance slices for the camera pass
and the three shadow cascades (issue #130: one instanced draw per populated
static part batch). Each render state also carries
`localSkylight` / `localBlockRgb` sampled per frame from the chunk light caches
(trilinear over packed 16-bit voxel light, `RenderWorldBridge::sampleSmoothedLight`)
— a derived presentation step OUTSIDE the simulation (issue #208; headless
snapshots leave the light unassigned).
UI: Player/Gameplay panel "Passive mobs" toggle (`GameUI`, issue #184) with
active/visible counters in the Overview console; CPU time
under the `Mobs` profiler scope, GPU under `GpuPass::Mobs` / `MobShadow0-2`.

### Entity light caches (`ChunkLightStorage`, issues #128/#172)

Each near-camera chunk may hold a packed 128 KiB light cache (skylight + RGB
block light) acquired from the pooled `ChunkLightPool`. Residency is governed by
**acquire/release hysteresis** around the camera: caches are acquired at 128 m
(the mob spawn/wander range — mobs retire beyond 112 m) and retained until
144 m — a 16-block margin (~+12.5%) that absorbs in-chunk camera movement
(a 16 m chunk never crosses both thresholds in one step) — so camera jitter
around the acquire edge cannot oscillate allocations.

Cache acquisition is **decoupled from meshing** (issue #172): a `MESHED` chunk
entering the radius gets a dedicated async light-only job
(`RenderWorldBridge::updateEntityLightCaches` → `Chunk::buildLightCache`) that
computes the same chunk-wide light field a mesh build would and publishes only
the storage after generation/revision/intent validation. It never touches chunk
state, dirty sections, mesh payloads or GPU upload flags — a valid render mesh is
never invalidated by cache residency. Real mesh builds still populate the cache
when it is wanted, so no light work is duplicated when a remesh was due anyway.

The light-only pipeline has its **own telemetry** (issue #173 review), disjoint
from the mesh path:

```text
updateEntityLightCaches
  -> LightCacheQueue        (queue-wait worker sample)
  -> LightCache worker      (job execution worker sample)
  -> lightCache.skylight / lightCache.blocklight / lightCache.haloFill
  -> lightCache.build(sample-sum)
```

`MeshBuild` / `MeshLOD` / `MeshQueue` and the `mesh.*` workload stages /
`mesh.build(sample-sum)` totals therefore count **real mesh builds only**.
Observability: the Streaming panel and `[stream]` log report the light queue
(`load/gen/mesh/light`), and the benchmark reports `LightCache` worker jobs plus
`LightCacheQueue` background waits and a `peakPendingLight` peak
(`qLoad/Gen/Mesh/Light=`).

### Tests

- `tests/test_mobs.cpp` — AI behaviors, deterministic population, models; `--profile` prints 48-mob CPU cost (allocation-free per tick).
- `tests/test_chunk_lifecycle.cpp --mobs-profile` — spawning/movement on real generated terrain.
- `tests/test_mob_textures.cpp`, `tests/test_mob_render.cpp` — resource and Vulkan coverage (see renderer doc).

---

## 10. Camera

File: `src/Camera/Camera.hpp` / `Camera.cpp`.

- First-person physical player by default, with explicit debug flight and isometric inspection
- Supplies view matrix, position, and frustum data for culling  
- Engine finds a clear, supported player spawn (`sim::GameSimulation::spawnPlayerOnSurface`, driven by `placeCameraOnSurface`); unavailable spawn falls back to debug flight
- Raycast against voxels for highlight and dig/place (`Engine::raycastVoxel`)  

Player motion is owned by the simulation's `PlayerController`
(`sim::GameSimulation`), not the camera. The CPU-only solver queries published
voxel data through a scoped `WorldCollisionView`, independently of render
meshes and LOD. See [player-physics.md](player-physics.md)
for controls, timing, thread/publication contracts and future entity integration.

---

## 11. Interaction with the renderer

| Engine data | Consumed by |
|-------------|-------------|
| `drawList` chunks | Opaque + water passes |
| `shadowList` chunks | ShadowPass |
| `ShaderParameters` + camera | FrameUBO (filled **before** `recordFrame`) |
| `uploadBudgetThisFrame` | `uploadPendingMeshes` inside **preRecord** |
| `RenderSettings::shadowCascadeFar` / shadow distance | CSM + caster radius |
| Camera in water | `PostProcessSettings::underwater` / UBO flag + `underwaterSurfaceY` (scan up to the local water surface; drives the composite submersion blend, issue #144) |
| `PostProcessSettings` | PostStack composite + effect toggles |
| Overlay highlight / demo players | **OverlayRenderer** via OpaquePass |

`WorldRenderer` does not stream chunks; it only records draws for lists the engine built in `tickStreaming`. Mesh GPU upload is recorded at the start of `recordFrame` (preRecord), not as a separate step before UBO.

---

## 12. World persistence (`src/World/`, issue #180)

Persistent world saves: a **seed + sparse-override** model. The deterministic
terrain generator is always the source of truth; a save stores only the world
identity (seed) plus the voxels the player actually changed. A fresh install of
the engine can therefore reproduce any saved world from `seed +
TerrainGenerator::kGeneratorVersion` and re-apply the stored diff on top.

### Directory layout

```text
saves/<name>/world.meta        16-byte identity: magic 'FTVW', format version, seed, generator version
saves/<name>/chunks/<x>_<z>.chunk   one sparse override table per edited chunk ('FTVC', checksummed)
saves/<name>/player.state      45-byte player snapshot ('FTVP'): feet position, yaw/pitch, flight, selected block
```

All files are little-endian byte-level formats (no struct dumps) — exact
layouts are documented in `src/World/WorldSave.hpp`. Chunk `localIndex` is the
canonical y-major index (`y*256 + z*16 + x`) and `blockType` is the final
authoritative value for that voxel (an override table, not an operation log).

### Format versioning + generator compatibility

`world.meta` carries both a save **format version** (`kWorldSaveFormatVersion`)
and the **generator version** stored at create time. On open, a stored
generator version different from the running `TerrainGenerator::kGeneratorVersion`
is refused (no migration in v1). Consequence: **any terrain change that would
alter generated voxels for an existing seed must bump
`TerrainGenerator::kGeneratorVersion`** — otherwise saved-world overrides would
be re-diffed against a different base and silently corrupt the restored terrain.
Pure additive changes (new biomes/blocks that leave existing seeds' voxels
untouched) do not require a bump.

### Lifecycle integration (`VoxelWorld` / `RenderWorldBridge`)

- **Open** (`VoxelWorld::openWorld` → `WorldPersistence::openOrCreate`,
  called from `Engine::initializeNoiseGenerator` before any generation):
  validates/creates `world.meta`, cleans leftover `.tmp` files (an interrupted
  atomic write is never authoritative), then loads every chunk file into an
  in-memory override index.
- **Apply**: freshly generated chunks get the stored overrides re-applied after
  deterministic generation (async gen jobs + the synchronous bootstrap path),
  so procedural output and saved edits compose deterministically.
- **Capture**: edited chunks are captured **before pool release on unload** —
  the current value of every edited voxel is copied out and the chunk can be
  recycled immediately.
- **Write**: a dedicated `SaveService` `std::jthread` does all disk I/O (never
  the shared gen/mesh `ThreadPool` — disk stalls must not starve worldgen).
  Requests are superseded per chunk coordinate (the last enqueued request
  wins; doomed older requests are dropped without I/O), the true minimal diff
  is computed inside the worker by regenerating the chunk's deterministic base,
  and every file is written tmp + rename (crash leaves the previous file or the
  complete new one, never a mixture).
- **Close** (`closeWorld`): capture-all + flush + service shutdown; the
  world then behaves exactly like a never-opened one. `~VoxelWorld` runs
  the same path as a safety net (the bridge quiesces its render jobs first).

### Engine wiring (Phase 5; facade host since issues #208/#209)

- `--world <name>` (CLI) → `Engine::requestOpenWorld` → consumed by
  `sim::GameSimulation::initialize` inside `initializeNoiseGenerator` before
  generation. If the stored seed differs from an explicitly requested
  `--seed`, the open is **refused** and the session runs transient (identity
  is never overwritten, `openWorldError()` surfaces why in the World panel);
  without an explicit seed the stored seed recreates the `TerrainGenerator`.
- Player state (`player.state`) is restored on open when present (position is
  kept verbatim — no surface snap) via `sim->finalizePlayerSpawn`, and written
  in `~Engine` through `sim->savePlayerState(camera yaw, pitch)` (the
  authoritative body + the host-supplied presentation orientation) before the
  final flush; `sim->shutdown()` then force-closes the world unconditionally.
- **Transient / benchmark worlds run persistence-disabled**: `reloadWorld`
  closes any open world first (through the bridge's quiesce + the world's
  refusable close), so a benchmark can never write into a user save. Sessions
  without `--world` never create files. Combining `--world` with `--benchmark`
  is a CLI error (fail-fast) rather than a silently ignored flag.
- **World transitions never recreate Vulkan objects for CPU state**: the
  simulation swaps only CPU world/generator/player/mob state; the Engine
  keeps the device, swapchain, renderer and ImGui alive across
  initialize/reload (the epoch barrier resets the derived projection, and
  `reloadWorld` waits device-idle + flushes the retire queue around the
  bridge swap — issue #209).

### Failure policy

- Last known-good data is preserved: writes are atomic, and a failed open
  never overwrites identity.
- Corrupt chunk files are loudly reported (`status().lastError`, World UI
  panel) and **skipped** — that chunk falls back to procedural generation.
- Seed / generator-version mismatch = refuse to open; the session continues
  without persistence rather than exiting.
- Corrupt `player.state` is reported and ignored (fresh spawn) — never
  teleports the player.

Tests: `tests/test_world_save.cpp` (format layer), `tests/test_world_persistence.cpp`
(service, facade, RenderWorldBridge integration, player state).

---

## 13. Networking (status)

`src/Network/` — Boost.Asio UDP client/server for position/world state.

- Built for `test_network` / historical multiplayer experiments  
- **Not currently wired** into the Vulkan `Engine` UI or authoritative world sim  

Document presence only; do not assume multiplayer is live in the main binary UX.

---

## 14. Related docs

- [`vulkan-graphics.md`](vulkan-graphics.md) — Vulkan device, pass graph, shaders, post  
- [`terrain-generation.md`](terrain-generation.md) — noise graphs, biome/block catalog, extension procedures, calibration
- Root [`README.md`](../README.md) — build and controls  
- [`AGENTS.md`](../AGENTS.md) — contributor conventions and architecture map
