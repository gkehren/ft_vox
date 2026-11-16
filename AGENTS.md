# ft_vox Project Context

`ft_vox` is a high-performance voxel sandbox engine and game built from scratch using C++20. It features a procedurally generated world with infinite terrain, biomes, and modern rendering techniques.

**Graphics:** **Vulkan 1.2+** (MoltenVK on macOS). OpenGL/GLAD path removed (PR7).

**Architecture docs (authoritative):**

- [`docs/vulkan-graphics.md`](docs/vulkan-graphics.md) — Vulkan device path, pass graph, shaders, post
- [`docs/engine-architecture.md`](docs/engine-architecture.md) — Engine loop, chunks, streaming, terrain gen
- [`docs/simulation-render-contracts.md`](docs/simulation-render-contracts.md) — authoritative CPU world vs derived render boundary (Epic #204), ownership, migration checkpoints
- [`docs/headless-runner.md`](docs/headless-runner.md) — `ft_vox_headless` standalone CPU simulation runner: CLI, scripts, canonical digest, CPU-only build (issue #210)
- [`docs/visual-regression.md`](docs/visual-regression.md) — Visual-regression harness: scenes, references, tolerances

## Architecture Overview

### Core Engine
- **Engine (`src/Engine/`)**: Main loop, SDL3 window (`SDL_WINDOW_VULKAN`), input, high-level systems. Graphical host of `sim::GameSimulation` (authoritative) + `RenderWorldBridge` (derived): maps SDL input to sim commands, owns the single suspension policy, the day-cycle presentation parameter and the world-epoch barrier (`resetWorldProjection`); `run()` is the documented 16-step frame orchestration (issue #209). Interaction mapping (issue #227 / Epic #223 Phase D): walking LMB/RMB → `sim->breakBlock`/`placeSelectedBlock` (accepted edits publish via `RenderWorldBridge::publishEditResult` — the same drain + commit groups as creative), flight keeps the creative path; `1..9`/wheel → hotbar selection, `Q` → `dropFromSlot`, `E` → inventory-panel flag (panel in #229), `T` creative cycle is flight-only; `raycastVoxel` is a thin forward to the sim query; `tickItems` runs `advanceItems` next to `tickMobs`
  - **`GameUI`**: multi-panel ImGui (Status Overlay, Player/Gameplay, Graphics, Streaming, World/biome map, Help) + F-key shortcuts (issue #184: the old catch-all HUD was split into the read-only Status Overlay and the Player/Gameplay panel; VSync lives in Graphics, physics counters in the Player Diagnostics console)
- **Vulkan backend (`src/Vulkan/`)**:
  - `VkContext`: instance, surface, physical/logical device, queues, feature detection, **VMA**
  - `VkAllocator` / `VkBuffer` / `VkImage` / `VkUpload` / `VkCommands`: GPU resources + staging
  - `VkShader`: SPIR-V file load → `VkShaderModule`
  - `VkSwapchain`: surface formats, present modes, resize
  - `VkFrameContext`: frames-in-flight, command buffers, WSI sync (sole acquire/submit/present owner)
  - `ImageBarrier` / `GraphicsPipelineBuilder`: shared helpers
- **Simulation facade (`src/Sim/`, CPU-only `ft_vox_core`)** — issue #208 / Epic #204 Phase D:
  - **`GameSimulation`**: single owner of the authoritative gameplay state — `TerrainGenerator`, `VoxelWorld`, `PlayerController` (120 Hz), `MobSystem` (60 Hz), `items::Inventory` (issue #225: hotbar/inventory commands `selectHotbarSlot`/`moveStack`/`swapStacks`/`splitStack` + the logged `grantItems` admin seam, bounded in-place `InventorySnapshot`; commands apply instantly at the frame boundary, no buffer, suspension never replays), `items::ItemEntitySystem` (issue #226: dropped items at 60 Hz, `breakBlock`/`dropFromSlot` commands, deterministic auto-pickup into the inventory, monotonic entity ids with exposed high-water, id-sorted `ItemEntitySnapshot` + bounded spawn/remove event stream, `advanceItems` facade step), interaction targeting + place-from-hotbar (issue #227: `raycastVoxel` — the historical Engine march moved verbatim, plain `RaycastHit`; `placeSelectedBlock` — registry placeable + `canPlaceVoxel` gate + consume-exactly-1-on-success, `PlaceResult` codes; accepted edits move out a `WorldEditResult` for the bridge publication path), flight mode, block selection, spawn/restore, reload counter, player-aware placement query (`canPlaceVoxel`, hosts gate edits with it). Explicit lifecycle (initialize/bootstrap/finalize/reload/save/shutdown — initialize/reload reset the inventory AND the item population/id allocator; the restore hooks are #228), per-frame advance with plain values (dt, commands, observer point), read-only bounded snapshots (`PlayerSnapshot`, `MobSnapshot`, `WorldSnapshot`, `InventorySnapshot`, `ItemEntitySnapshot`). No Camera/RenderSettings/SDL/Vulkan/ImGui types; runs headless (`test_game_simulation`, `test_gameplay`). Selection duality: the hotbar selection is gameplay (walking mode, host-wired since #227); `selectedBlock()` stays the creative/editor seam (flight mode, benchmarks, headless scripts)
- **Headless runner (`src/Headless/`, CPU-only)** — issue #210 / Epic #204 Phase F:
  - **`ft_vox_headless`**: standalone deterministic simulation runner hosting the SAME `GameSimulation` facade with **no SDL3/Vulkan/ImGui/camera/renderer/GPU pool**; links only `ft_vox_core`. Explicit headless interest source (plain point + radius) drives residency; fixed per-frame streaming budgets; synchronous (`--workers 0`, `VoxelWorld::generatePendingVoxelsSync`) or async (`--workers N`, per-frame quiesce) generation — both yield the same canonical state digest. Scripted actions (`--script`), save/reload round-trips (`--world`/`--saves-root`), machine-readable SUMMARY + distinct error exit codes. `-DFT_VOX_GRAPHICS=OFF` builds the CPU slice with the graphics stack **not discovered at all**; `FT_VOX_PORTABLE_FP` selects canonical FP codegen. See `docs/headless-runner.md`
- **Authoritative world (`src/World/` + `src/Core/`, CPU-only `ft_vox_core`)** — issue #206 / Epic #204 Phase B:
  - **`VoxelWorld`**: single owner of chunk residency/streaming, serialized logical edits (coalescing + neighbor-border mirrors), terrain worker publication, and `WorldPersistence`. Render-free public API; emits the `WorldEvent` change stream consumed by the `RenderWorldBridge`. Runs headless with no bridge registered; owned by `GameSimulation` in game
  - **`WorldChunk`**: authoritative chunk record — voxel backing, occupancy, border shell, persistent edits, `sim::ChunkIdentity` (generation + revision, the single source of the pair), lifecycle state
  - **`WorldCollisionView`**: CPU-only collision/query seam with the exact historical collision semantics (physics/mobs consume it directly)
  - **`WorldMobView`**: CPU-only mob seam (`entities::MobWorld`) — collision + biome/grass surface scan; replaced the removed `ChunkMobWorld` adapter (issue #208). Derived entity light sampling stays on the bridge, sampled by the host after the mob snapshot
  - `WorldSave` / `WorldPersistence`: seed+sparse-override saves (`saves/<name>/`), async crash-safe I/O (issue #180 contracts)
  - `Core/SimContracts.hpp`, `Core/ThreadPool.hpp`, `Core/WorkloadTelemetry.hpp`: render-independent contracts + CPU utilities
- **Item domain (`src/Items/`, CPU-only `ft_vox_core`)** — issue #224 / Epic #223 Phase A; owned by `GameSimulation` since Phase B (issue #225):
  - **`ItemTypes.hpp`**: `ItemId` (stable append-only `uint16_t` registry ordinal, independent of the render-atlas `TextureType`), `ItemStack` (4-byte plain value, canonical empty `{None,0}`), `ItemResult` explicit operation codes, `kHotbarSize = 9` / `kInventorySize = 36`
  - **`ItemRegistry`**: one static append-only `ItemDefinition` table (one block item per breakable/placeable block), explicit block→drop table (`STONE`→`COBBLESTONE`, grass block→`DIRT`, leaves→themselves, `BEDROCK` unbreakable, fluids not breakable by hand), `itemForBlock`/`dropForBlock`/`parseItemId` lookups; `static_assert`s pin catalog alignment
  - **`ItemStack` math**: pure merge/split/transfer functions with explicit results (odd half-split takes the larger half), shared by the future pickup/drop paths
  - **`Inventory`**: fixed 36-slot container (hotbar = slots 0..8), selected hotbar index, deterministic hotbar-first `addStack`/`removeCount`/`moveStack`/`swapStacks`/`splitStack`; trivially copyable, no heap churn
  - **`ItemEntitySystem`** (issue #226 / Epic #223 Phase C): dropped item entities on the `MobSystem` pattern — fixed 60 Hz step (own accumulator, `maxSteps` cap, suspension freeze), gravity + `physics::move` through `WorldCollisionView` (no new solver), pickup-delay + proximity auto-pickup (hotbar-first, partial remainder stays in the world), age despawn (18000 ticks) + population cap (1024, oldest-first overflow, id tie-break), monotonic never-reused entity ids (high-water exposed for #228), id-sorted bounded snapshots + bounded spawn/remove event stream (`WorldEvent` drain pattern); jitter derives only from entity id / edit revision
- **Chunk Management (`src/Chunk/`)**: 16×256×16 voxels, greedy meshing, VMA GPU upload, terrain gen
  - **`Chunk`**: render handle bound to a `WorldChunk` (forwards voxel reads/writes; keeps GPU ranges, draw caches, mesh payloads, light caches) — the bridge's per-chunk render record
  - **`ChunkPool`** + **`ThreadPool`**: preallocated render handles and work-stealing workers
  - **`TerrainGenerator`**: FastNoise2 biomes / height / caves
- **Entities (`src/Entities/`)**: `MobSystem` — CPU fixed-step passive mobs (cow/pig/sheep/chicken): deterministic seed-based group spawning, ambient wander AI on the shared voxel collision solver; owned and advanced by `GameSimulation` (issue #208). `MobModel` bakes the Minecraft box-UV articulated meshes
- **Rendering (`src/Renderer/`)**: `WorldRenderer` orchestrator — **ShadowPass → OpaquePass → WaterPass → SkyPass → PostStack** (+ `MobRenderer`, `OverlayRenderer`, `TextureManager`, `FrameUBO`, `MaterialTable`, `Lighting`, `ShadowCascades`)
  - **`RenderWorldBridge`** (issue #207 / Epic #204 Phase C): renderer-owned **derived projection** of `VoxelWorld` — mesh/light dispatch, GPU upload, visibility, commit groups, deferred GPU retirement, light-cache sampling; references (never owns) the host-owned world. `publishEditResult` (issue #227) publishes edits accepted through the sim gameplay commands with the same event drain + commit-group registration as the creative `placeVoxel`/`deleteVoxel` (one publication path). Replaced the removed `ChunkManager` adapter / `ChunkCollisionView` wrapper (see `docs/simulation-render-contracts.md` §10)

### Networking (`src/Network/`) — experimental, test-only
- **Client/Server**: Boost.Asio UDP for player position and world state. Not a shipped feature: the module is compiled into `test_network` only and is **not linked into the game binary**. Do not add new runtime dependencies on it without re-arming the build wiring first.

## Technologies & Dependencies
- **Language**: C++20
- **Graphics API**: Vulkan 1.2+ (loader via **volk**; memory via **VMA**)
- **Windowing/Input**: SDL3 with **`vulkan` feature**
- **Shaders**: GLSL → SPIR-V offline (`glslc` / `glslangValidator`) under `ressources/shaders/vulkan/`
- **Mathematics**: GLM (`GLM_FORCE_DEPTH_ZERO_TO_ONE`)
- **Terrain Generation**: FastNoise2
- **Networking**: Boost.Asio (test-only prototype; see `src/Network/` note above)
- **UI**: ImGui (vendored SDL3 + Vulkan backends; dynamic rendering)
- **Build System**: CMake (3.16+) with platform-aware deps (`cmake/Dependencies.cmake`)
  - **Linux:** distro packages (apt/dnf/pacman) by default; vcpkg optional (`USE_VCPKG=1`)
  - **Windows:** vcpkg almost everything (`vcpkg.json`, `build.ps1`)
  - **macOS:** Homebrew Vulkan stack; vcpkg if `VCPKG_ROOT` present, else system
  - **FetchContent:** GLM, FastNoise2, and (if needed) zeux/volk + VMA; optional SDL3 via `FT_VOX_FETCH_SDL3`

### Packages

| Source | Packages |
|--------|----------|
| vcpkg (`vcpkg.json`) | `sdl3[vulkan]`, `boost-asio`, `boost-system`, `vulkan-headers`, `vulkan-loader`, `volk`, `vulkan-memory-allocator`, `glslang` |
| Linux apt (example) | `libsdl3-dev`, `libboost-system-dev`, `libvulkan-dev`, `glslang-tools` |
| macOS brew | `sdl3`, `boost`, `molten-vk`, `vulkan-loader`, `glslang` |

**Removed (cleanup):** OpenGL stack (GLAD, legacy GLSL, `Renderer`/`Shader`/`UIManager`/`TextRenderer`/`PostProcessing`), unused ImGui extras (OpenGL/SDLGPU backends, demo, stdlib helper), unused `InputSystem`/`EventBus`, FreeType vcpkg dep. Note: **`ImGuiFileDialog` is NOT removed** — it is vendored under `src/ImGuiFileDialog/` and used by the resource-pack browser. The Network module stays for `test_network` only (compiled into the test; **Boost is not linked into the game binary**).

### macOS (MoltenVK)
```bash
./install_dep.sh   # or: brew install molten-vk vulkan-loader
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
```

**Validation layers** (Debug default; override with `FT_VOX_VALIDATION=0|1`):

```bash
brew install vulkan-validationlayers
export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
```

Layers are **not** linked into the game binary; the Vulkan loader discovers them at runtime via `VK_LAYER_PATH`.

## Development Guide

### Building the Project

#### Linux (prefer system packages)
```bash
./install_dep.sh
make                          # FT_VOX_DEP_MODE=system
# fallback if SDL3 missing:
make USE_VCPKG=1 VCPKG_ROOT=$HOME/vcpkg
```

#### macOS
```bash
./install_dep.sh
make                          # uses ~/vcpkg toolchain if present
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
./build-vk/ft_vox
```

#### Windows (vcpkg)
```powershell
.\build.ps1
# or:
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

### Testing

```bash
make test
# or
cd build-vk && ctest --output-on-failure

# Terrain calibration / profiling
./build-vk/tests/test_terrain --profile 32 1337
./build-vk/tests/test_terrain --world-stats 16 3

# Automated Vulkan streaming benchmark (saves a report to the gitignored
# benchmark-results/ dir, then exits)
./build-vk/ft_vox --seed 42 --benchmark 30

# Visual regression (offscreen golden-image comparison)
ctest -R VisualRegression --output-on-failure   # from the build dir
# References are committed in tests/visual-references/ and regenerated ONLY
# explicitly via `ft_vox_visual_tests --update-references`. Details:
# docs/visual-regression.md

# Headless simulation runner (issue #210) — CPU-only, no GPU/window needed
./build-vk/ft_vox_headless --seed 42 --ticks 600 --script tests/fixtures/sim-actions.txt
ctest -R Headless --output-on-failure   # from the build dir (always-on guards)
# Heavier clean CPU-only configure/build gate is OPT-IN (label cpu-only):
#   cmake -B build-cpu -DFT_VOX_GRAPHICS=OFF -DFT_VOX_CPU_ONLY_BUILD_CHECK=ON
#   ctest --test-dir build-cpu -L cpu-only --output-on-failure
# CPU-only configure (no SDL3/Vulkan SDK/shader toolchain discovered at all):
cmake -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu -j && ctest --test-dir build-cpu --output-on-failure
# Details: docs/headless-runner.md
```

### Key Conventions
- **Voxel Data**: Flat `uint8_t` array per authoritative `WorldChunk` record (`src/World/`); the render `Chunk` handle forwards to it
- **Sim/render boundary (Epic #204)**: authoritative state (voxels, edits, residency, persistence, player, mobs) lives in the CPU-only `ft_vox_core` (`GameSimulation` owns `VoxelWorld`/`WorldChunk` + `PlayerController` + `MobSystem`); render state (meshes, light caches, GPU, visibility) is derived. Core headers must never include `Renderer/`, `Vulkan/`, `Camera/`, `Engine/`, SDL3 or ImGui — enforced by `tests/check_core_includes.py` (CTest `CoreIncludeGuard`). `sim::ChunkIdentity` is the single source of the (generation, revision) pair
- **Headless boundary (issue #210, hardened #211)**: `ft_vox_headless` (`src/Headless/`) links only `ft_vox_core` and must stay graphics-free. The CPU/GPU boundary is enforced by repeatable automated checks: the configure-time transitive link-closure assertion (`ft_vox_assert_cpu_link_closure`) covers `ft_vox_core`, `ft_vox_headless` and every CPU-only test (the authoritative link gate, incl. MSVC where a static lib has no link step); `CoreIncludeGuard` guards the **core** include closure; `tests/check_headless_deps.py` (CTest `HeadlessDeps`) guards the **runner** include closure plus the core/runner link artifacts (on MSVC the core static-lib scan reads `ProjectReference` entries) and CPU-only cache discovery. `FT_VOX_GRAPHICS=OFF` is the CPU-only configure path (graphics stack not discovered); keep new CPU tests outside the `if(FT_VOX_GRAPHICS)` guards in `tests/CMakeLists.txt`
- **World Constants**: `src/utils.hpp` (`CHUNK_SIZE`, `WORLD_HEIGHT`)
- **Resources**: `ressources/`; runtime expects `RES_PATH` next to the binary
- **Biome/block enums**: `BiomeType` and `TextureType` are append-only; keep
  `biomeTypeString`, `GameUI::kBiomeColors`, `textureTypeString`, and
  `kBlockLayers` aligned with their enum ordinals
- **Item registry**: `items::ItemId` ordinals are append-only and save/wire
  stable (saves reference `ItemId`, never `TextureType`); keep the
  `ItemDefinition` table, the block→item/block→drop tables and their
  `static_assert`s aligned when adding blocks (issue #224, epic #223).
  A new per-face atlas variant must update **all three pinned mappings
  together**: `canonicalBlock` (`Items/ItemRegistry.cpp`), the render face
  remap (`blockTopFace`/`blockBottomFace`, `Renderer/MinecraftTextures.hpp`),
  and the `kFaceVariants` pin table in `tests/test_items.cpp`
- **Resource-pack blocks**: bundled terrain textures live in
  `ressources/textures/` and may be sourced from the compliant
  `docs/default-resource-pack/assets/minecraft/textures/block/` fixture
  (optional **untracked** local fixture — gitignored, not part of the repository)
- **Vulkan**: `VK_NO_PROTOTYPES` + volk; no global OpenGL-style state
- **Depth / Y**: `GLM_FORCE_DEPTH_ZERO_TO_ONE`; negative viewport height for OpenGL-style Y without winding flip (CCW)
- **Descriptors**: Never `vkUpdateDescriptorSets` mid-command-buffer; use fixed sets or push constants
- **Threading**: `ThreadPool` for terrain gen + meshing; GPU upload / VMA destroy on main thread after `waitIdle` (or deferred unload)
- **Streaming**: `RenderSettings::{min,max}RenderDistance` (blocks) + per-sec budgets; near range = full mesh, far = LOD mesh; `streamFrontBias` loads farther/ahead-first in view direction
- **Terrain reference**: `docs/terrain-generation.md` is authoritative for
  noise graphs, the biome/block catalog, extension procedures, and calibration

## Performance Considerations
- **Greedy Meshing**: Face culling + greedy meshing to reduce vertex count
- **Texture Arrays**: `sampler2DArray` — one bind for all block types
- **Frustum Culling**: Chunks culled before draw
- **Shadow PCF**: Directional cascade-style orthographic shadow map
- **Post**: Bloom + god rays + ACES + optional FXAA on HDR targets
