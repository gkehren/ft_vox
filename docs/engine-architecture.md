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
        ├── WorldRenderer (+ passes, post, overlays)
        ├── TerrainGenerator + ThreadPool + ChunkPool + ChunkManager
        ├── StagingRing + GpuResourceRetire
        ├── Camera
        └── ImGuiLayer + GameUI
```

| Area | Directory | Primary types |
|------|-----------|---------------|
| App / loop | `src/Engine/` | `Engine`, `EngineDefs`, `GameUI`, `ImGuiLayer`, `ThreadPool`, `Profiler`, `Benchmark` |
| World data | `src/Chunk/` | `Chunk`, `ChunkManager`, `ChunkPool`, `TerrainGenerator`, `StreamHelpers` |
| Camera | `src/Camera/` | `Camera` |
| Rendering | `src/Renderer/` | `WorldRenderer`, passes, `PostStack`, … |
| Vulkan glue | `src/Vulkan/` | Context, frames, staging, retire |
| Shared constants | `src/utils.hpp` | Chunk size, voxels, biomes, textures, states |
| Network (optional) | `src/Network/` | Client/Server — **not re-wired into Engine UI** |

---

## 2. Engine loop (`Engine`)

File: `src/Engine/Engine.hpp` / `Engine.cpp`.

### Ownership

`Engine` owns (unique_ptrs / members):

- **Windowing:** `SDL_Window *`, size, mouse capture, pause  
- **Vulkan:** `VkContext`, `VkSwapchain`, `VkFrameContext`, `ImmediateCommands`  
- **Render:** `WorldRenderer`, ImGui layer, `GameUI`  
- **World:** `TerrainGenerator`, `ThreadPool`, `ChunkPool`, `ChunkManager`  
- **GPU streaming helpers:** `StagingRing`, `GpuResourceRetire`  
- **View:** `Camera`  
- **Settings:** `RenderSettings`, `ShaderParameters`, `RenderTiming`, seed  
- **Benchmark:** `Benchmark`  

### Per-frame outline (`Engine::run`)

Matches `Engine.cpp` order:

1. **Profiler begin** — `GetProfiler().beginFrame()`  
2. **Events / input / benchmark** — `handleEvents`, `tickBenchmark`, `processInput`  
3. **Day cycle** — `tickDayCycle` updates `ShaderParameters` sun/moon/day factors  
4. **`tickStreaming`** (CPU world pipeline; **before** acquire)  
   - `processFinishedJobs` / `processDeferredReleases`  
   - `updateStreaming` → budgeted `processChunkLoading` / `generatePendingVoxels` / `meshPendingChunks` (`maxStreamMs` envelope)  
   - `updateVisibility` → `collectDrawList` / `collectShadowList`  
   - Computes `uploadBudgetThisFrame` (uploads are **not** done here)  
5. **Highlight** — raycast block under cursor (`updateHighlight`)  
6. **Deferred swapchain recreation** if needed — resize, VSync, or WSI invalidation follows one Engine-owned path that refreshes frame synchronization, `WorldRenderer`, and ImGui before acquisition
7. **Acquire** — `VkFrameContext::beginFrame` → image index + command buffer  
8. **Retire/staging frame slots** — `resourceRetire.beginFrame`, `stagingRing.beginFrame` (fence already waited)  
9. **ImGui UI build** — `imgui->beginFrame` / `drawUi` / `endFrame` (CPU only; draw later)  
10. **UBO** — underwater sample + `WorldRenderer::updateFrameUBO`  
11. **Record** — `WorldRenderer::recordFrame`:  
    - **preRecord:** `uploadPendingMeshes` + transfer→vertex barrier  
    - **ShadowPass → OpaquePass** (opaque chunks + **overlays inside OpaquePass**) **→ WaterPass → SkyPass → PostStack**  
    - **imguiDraw:** `imgui->recordDraw` onto swapchain after composite  
12. **Submit / present** — `VkFrameContext::submitAndPresent`  
13. **Profiler end** + copy scopes into `RenderTiming` / benchmark sample  

Bootstrap: `generateInitialArea` fills a small radius around spawn synchronously so the first frames are not empty.

`reloadWorld(seed)` rebuilds terrain generation state (device idle) for tools/benchmarks.

---

## 3. Settings and UI

### `EngineDefs.hpp`

| Struct / enum | Purpose |
|---------------|---------|
| `ShaderParameters` | Fog, sun/moon, ambient/diffuse, day cycle, water knobs, outdoor grade |
| `RenderSettings` | Render distance (min/max blocks), `streamFrontBias`, stream rates (`load/gen/mesh/upload` per sec), shadow distance / cascade far, `maxStreamMs`, wireframe/borders/vsync |
| `PostProcessSettings` | Bloom, SSAO, god rays, exposure/tonemap, FXAA, grain, vignette, underwater, **quality preset** |
| `GraphicsQualityPreset` | Low / Medium / High / Cinematic — `applyPreset` only remaps existing post knobs |
| `RenderTiming` | Legacy flat timings filled from hierarchical profiler |

### UI

- **`GameUI`** — multi-panel ImGui: HUD, Graphics, Streaming, World/biome map, Help; F-key shortcuts  
- **`ImGuiLayer`** — SDL3 + Vulkan backends, dynamic rendering  
- **`Profiler`** — hierarchical CPU scopes (F7-style panel)  
- **`Benchmark`** — scripted runs / reports under `docs/benchmarks/`  

Controls summary lives in root `README.md` (WASD, break/place, borders, etc.).

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
- **`Vertex`** — position, packed normal/tex/AO/biome flags, UV, packed biome color  
- **`TextureType`** — block atlas indices (stone, dirt, grass, leaves, water, ores, …)  
- **`BiomeType`** — ocean, beach, plains, forests, deserts, tundra, mountains, …  
- **`ChunkState`** — lifecycle for streaming (unloaded → generated → meshed → GPU-ready, plus transit flags)

---

## 5. Chunk (`Chunk`)

File: `src/Chunk/Chunk.hpp` / `Chunk.cpp`.

### Data

- Flat voxel array + active bitset  
- Neighbor **shell** voxels for correct greedy meshing at borders  
- Biome grass/foliage colors per column  
- CPU meshes: opaque vertices/indices + water mesh  
- GPU: VMA vertex/index buffers (opaque + water), deferred release  

### Operations

| Method | Role |
|--------|------|
| `generateTerrain` | Fill voxels via `TerrainGenerator` |
| `generateMesh` / `generateLODMesh` | Greedy meshing; far chunks may use LOD mesh |
| `uploadToGPU` | Sync upload (bootstrap/tests) |
| `uploadToGPUAsync` | Staging ring + retire old buffers (hot path) |
| `draw` / `drawWater` / `drawShadow` | Bind + draw indexed |
| `deleteVoxel` / `placeVoxel` | Edit + remesh flags |
| `rebuildShellFromNeighbors` | Face correctness across chunk edges |

### Lighting on mesh

At mesh time, sky light is cast down open columns then **flooded** into caves (attenuation). Block light propagates from emissive voxels. Packed into vertex attributes for the terrain/water shaders (see graphics doc for cave fill / sun shadow mute).

---

## 6. ChunkPool and ThreadPool

### `ChunkPool` (`ChunkPool.hpp`)

- Preallocated / recycled `Chunk` objects  
- Avoids allocator thrash when streaming  
- Acquisition uses `Chunk::ResetMode::ForGeneration`: lifecycle, mesh, and
  cache state is reset, but voxel clearing is deferred to `generateTerrain()`.
  Consumers must wait for generation before reading voxel data. Release uses
  the default full reset, including AIR fill, even if generation was cancelled.
- Grows with view distance / pressure (see git history for pool growth fixes)  

### `ThreadPool` (`Engine/ThreadPool.hpp`)

- Work-stealing workers for **terrain generation** and **meshing**  
- Priorities via task priority helpers (near camera first)  

**Rule:** GPU upload, VMA destroy, and descriptor/buffer free related to live draws happen on the **main thread** after appropriate retire delay — never free mesh buffers still referenced by in-flight frames.

---

## 7. ChunkManager (streaming)

File: `src/Chunk/ChunkManager.hpp` / `ChunkManager.cpp`.

### Pipeline

CPU work in `Engine::tickStreaming` (before swapchain acquire):

```text
processFinishedJobs
processDeferredReleases      (retire GPU → pool recycle)
updateStreaming              → enqueue loads / mark unload
processChunkLoading          (pool acquire)
generatePendingVoxels        (async terrain jobs)
meshPendingChunks            (async mesh; shell filled on main)
updateVisibility + collectDrawList / collectShadowList
```

GPU mesh upload is **not** in `tickStreaming`. It runs later inside `WorldRenderer::recordFrame` **preRecord** via `uploadPendingMeshes` (after acquire, same command buffer as draws).

### Distance model

- **`minRenderDistance`** (blocks, default 192) — near band: full mesh  
- **`maxRenderDistance`** (blocks, default 512) — stream/unload radius; far band may use **LOD mesh**  
- **`streamFrontBias`** (default 0.30) — view-direction load bias: chunks ahead of the camera count as closer (load first, reach ~×1.19 ahead / ~×0.87 behind); stays inside the 1.5× unload radius (no thrash). See `biasedLoadDistSq` in `StreamHelpers.hpp`
- Load queue is **distance-prioritized** (not pure FIFO); pruned each stream tick (`StreamHelpers.hpp` helpers: `LoadCandidate`, radius math, shell indexing)

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

## 9. Camera

File: `src/Camera/Camera.hpp` / `Camera.cpp`.

- First-person fly/look used by the sandbox  
- Supplies view matrix, position, and frustum data for culling  
- Engine places camera on surface at spawn (`placeCameraOnSurface`)  
- Raycast against voxels for highlight and dig/place (`Engine::raycastVoxel`)  

---

## 10. Interaction with the renderer

| Engine data | Consumed by |
|-------------|-------------|
| `drawList` chunks | Opaque + water passes |
| `shadowList` chunks | ShadowPass |
| `ShaderParameters` + camera | FrameUBO (filled **before** `recordFrame`) |
| `uploadBudgetThisFrame` | `uploadPendingMeshes` inside **preRecord** |
| `RenderSettings::shadowCascadeFar` / shadow distance | CSM + caster radius |
| Camera in water | `PostProcessSettings::underwater` / UBO flag |
| `PostProcessSettings` | PostStack composite + effect toggles |
| Overlay highlight / demo players | **OverlayRenderer** via OpaquePass |

`WorldRenderer` does not stream chunks; it only records draws for lists the engine built in `tickStreaming`. Mesh GPU upload is recorded at the start of `recordFrame` (preRecord), not as a separate step before UBO.

---

## 11. Networking (status)

`src/Network/` — Boost.Asio UDP client/server for position/world state.

- Built for `test_network` / historical multiplayer experiments  
- **Not currently wired** into the Vulkan `Engine` UI or authoritative world sim  

Document presence only; do not assume multiplayer is live in the main binary UX.

---

## 12. Related docs

- [`vulkan-graphics.md`](vulkan-graphics.md) — Vulkan device, pass graph, shaders, post  
- [`terrain-generation.md`](terrain-generation.md) — noise graphs, biome/block catalog, extension procedures, calibration
- Root [`README.md`](../README.md) — build and controls  
- [`AGENTS.md`](../AGENTS.md) — contributor conventions and architecture map
