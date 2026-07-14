# ft_vox

High-performance voxel sandbox engine built from scratch with **C++20**, **SDL3**, and **Vulkan 1.2+** (MoltenVK on macOS).

Features procedural infinite terrain, biomes, shadows, water, HDR post-processing (bloom, god rays, ACES), overlays, and multiplayer networking (Boost.Asio).

## Build requirements

- CMake 3.16+
- C++20 compiler (clang++ / g++ / MSVC)
- [vcpkg](https://vcpkg.io/)
- Vulkan loader + ICD:
  - **macOS:** MoltenVK (`brew install molten-vk vulkan-loader`)
  - **Linux:** GPU vendor Vulkan driver + `vulkan-loader`
  - **Windows:** GPU driver with Vulkan support
- `glslc` or `glslangValidator` (via vcpkg `glslang` or LunarG SDK)

### Optional (Debug validation)

```bash
# macOS (Homebrew)
brew install vulkan-validationlayers
export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d
```

## Dependencies (vcpkg)

| Package | Role |
|---------|------|
| `sdl3[vulkan]` | Window + input + Vulkan surface |
| `vulkan-headers` / `vulkan-loader` | Vulkan API + loader |
| `volk` | Dynamic Vulkan loader (`VK_NO_PROTOTYPES`) |
| `vulkan-memory-allocator` | GPU allocations (VMA) |
| `glslang` | GLSL → SPIR-V offline compile |
| `boost-asio` / `boost-system` | UDP networking |
| GLM, FastNoise2 | Fetched via CMake `FetchContent` |

ImGui is vendored under `src/imgui/` (SDL3 + Vulkan backends only).

## Build

```bash
# Toolchain path — adjust to your machine
export VCPKG_ROOT=${VCPKG_ROOT:-$HOME/vcpkg}
TOOLCHAIN=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

cmake -B build-vk -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DCMAKE_BUILD_TYPE=Release
cmake --build build-vk -j

# macOS: MoltenVK ICD
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json

./build-vk/ft_vox
./build-vk/ft_vox --seed 42
```

### Windows (Visual Studio)

```powershell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
.\build\Release\ft_vox.exe
```

## Tests

```bash
cd build-vk
ctest --output-on-failure
```

## Environment variables

| Variable | Purpose |
|----------|---------|
| `VK_ICD_FILENAMES` | Path to MoltenVK (or other) ICD JSON |
| `VK_LAYER_PATH` | Path to validation explicit_layer.d |
| `FT_VOX_VALIDATION` | `1` force validation layers on; `0` disable (default: on in Debug builds) |

## Controls (in-game)

| Input | Action |
|-------|--------|
| WASD / mouse | Move / look |
| Space / Ctrl | Up / down (fly) |
| LMB / RMB | Break / place block |
| T | Cycle selected block |
| B | Toggle chunk borders |
| Esc | Release mouse (ImGui) |

## Project layout

```
src/
  Vulkan/          # Instance, device, swapchain, VMA, frames, shaders
  Renderer/        # Terrain, shadows, water, post stack, overlays, textures
  Engine/          # Main loop, ImGui layer, input
  Chunk/           # Voxels, meshing, terrain generation
  Network/         # UDP client/server
  Camera/
ressources/
  shaders/vulkan/  # GLSL sources (compiled to SPIR-V at build time)
  textures/ fonts/ skybox/
```

## License

See repository root for license terms.
