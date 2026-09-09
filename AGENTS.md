# ft_vox Project Context

`ft_vox` is a high-performance voxel sandbox engine and game built from scratch using C++20. It features a procedurally generated world with infinite terrain, biomes, and modern rendering techniques.

**Graphics:** **Vulkan 1.2+** (MoltenVK on macOS). OpenGL/GLAD path removed (PR7).

**Architecture docs (authoritative):**

- [`docs/vulkan-graphics.md`](docs/vulkan-graphics.md) — Vulkan device path, pass graph, shaders, post
- [`docs/engine-architecture.md`](docs/engine-architecture.md) — Engine loop, chunks, streaming, terrain gen
- [`docs/visual-regression.md`](docs/visual-regression.md) — Visual-regression harness: scenes, references, tolerances

## Architecture Overview

### Core Engine
- **Engine (`src/Engine/`)**: Main loop, SDL3 window (`SDL_WINDOW_VULKAN`), input, high-level systems.
  - **`GameUI`**: multi-panel ImGui (HUD, Graphics, Streaming, World/biome map, Help) + F-key shortcuts
- **Vulkan backend (`src/Vulkan/`)**:
  - `VkContext`: instance, surface, physical/logical device, queues, feature detection, **VMA**
  - `VkAllocator` / `VkBuffer` / `VkImage` / `VkUpload` / `VkCommands`: GPU resources + staging
  - `VkShader`: SPIR-V file load → `VkShaderModule`
  - `VkSwapchain`: surface formats, present modes, resize
  - `VkFrameContext`: frames-in-flight, command buffers, WSI sync (sole acquire/submit/present owner)
  - `ImageBarrier` / `GraphicsPipelineBuilder`: shared helpers
- **Chunk Management (`src/Chunk/`)**: 16×256×16 voxels, greedy meshing, VMA GPU upload, terrain gen
  - **`ChunkManager`**: distance streaming (load → async gen → async mesh → main-thread upload), unload + pool recycle
  - **`ChunkPool`** + **`ThreadPool`**: preallocated chunks and work-stealing workers
  - **`TerrainGenerator`**: FastNoise2 biomes / height / caves
- **Entities (`src/Entities/`)**: `MobSystem` — CPU fixed-step passive mobs (cow/pig/sheep/chicken): deterministic seed-based group spawning, ambient wander AI on the shared voxel collision solver; `MobModel` bakes the Minecraft box-UV articulated meshes
- **Rendering (`src/Renderer/`)**: `WorldRenderer` orchestrator — **ShadowPass → OpaquePass → WaterPass → SkyPass → PostStack** (+ `MobRenderer`, `OverlayRenderer`, `TextureManager`, `FrameUBO`, `MaterialTable`, `Lighting`, `ShadowCascades`)

### Networking (`src/Network/`) — experimental, test-only
- **Client/Server**: Boost.Asio UDP for player position and world state. Not a shipped feature: the module is compiled into `test_network` only — the game binary builds none of its sources. Do not add new runtime dependencies on it without re-arming the build wiring first.

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

**Removed (cleanup):** OpenGL stack (GLAD, legacy GLSL, `Renderer`/`Shader`/`UIManager`/`TextRenderer`/`PostProcessing`), unused ImGui extras (OpenGL/SDLGPU backends, demo, stdlib helper), unused `InputSystem`/`EventBus`, FreeType vcpkg dep. Note: **`ImGuiFileDialog` is NOT removed** — it is vendored under `src/ImGuiFileDialog/` and used by the resource-pack browser. The Network module stays for `test_network` only (compiled into the test; the game binary builds none of its sources).

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

# Automated Vulkan streaming benchmark (saves a report, then exits)
./build-vk/ft_vox --seed 42 --benchmark 30

# Visual regression (offscreen golden-image comparison)
ctest -R VisualRegression --output-on-failure   # from the build dir
# References are committed in tests/visual-references/ and regenerated ONLY
# explicitly via `ft_vox_visual_tests --update-references`. Details:
# docs/visual-regression.md
```

### Key Conventions
- **Voxel Data**: Flat `uint8_t` array per `Chunk`
- **World Constants**: `src/utils.hpp` (`CHUNK_SIZE`, `WORLD_HEIGHT`)
- **Resources**: `ressources/`; runtime expects `RES_PATH` next to the binary
- **Biome/block enums**: `BiomeType` and `TextureType` are append-only; keep
  `biomeTypeString`, `GameUI::kBiomeColors`, `textureTypeString`, and
  `kBlockLayers` aligned with their enum ordinals
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
