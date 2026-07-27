# ft_vox

High-performance voxel sandbox engine built from scratch with **C++20**, **SDL3**, and **Vulkan 1.2+** (MoltenVK on macOS).

Features procedural infinite terrain, biomes, shadows, water, HDR post-processing (bloom, god rays, ACES), overlays, and multiplayer networking (Boost.Asio).

## Build requirements

| | Windows | Linux | macOS |
|--|---------|-------|-------|
| CMake | 3.16+ | 3.16+ | 3.16+ |
| Compiler | MSVC 2022 / clang-cl | g++ / clang++ (C++20) | Apple Clang (C++20) |
| Deps | **vcpkg** (recommended) | **apt / dnf / pacman** | **Homebrew** and/or vcpkg |
| Vulkan | GPU driver | mesa / vendor ICD | MoltenVK |
| Shaders | `glslc` or `glslangValidator` | `glslang-tools` | `brew install glslang` |

**Policy:** Linux prefers distro packages. Windows uses vcpkg for almost everything. macOS uses Homebrew for the Vulkan stack; C++ libraries may come from Homebrew or vcpkg.

## Quick start

### Linux (system packages)

```bash
./install_dep.sh          # apt, dnf, or pacman
make                      # no vcpkg toolchain
./build-vk/ft_vox
```

If your distro is too old for `libsdl3-dev` / `SDL3-devel`:

```bash
# Option A — fetch SDL3 via CMake
cmake -B build-vk -DFT_VOX_DEP_MODE=system -DFT_VOX_FETCH_SDL3=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-vk -j

# Option B — full vcpkg
make USE_VCPKG=1 VCPKG_ROOT=$HOME/vcpkg
```

### macOS (Homebrew + optional vcpkg)

```bash
./install_dep.sh          # molten-vk, sdl3, boost, glslang, …
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json

make                      # uses vcpkg automatically if ~/vcpkg exists
# or force system/Homebrew only:
make USE_VCPKG=0

./build-vk/ft_vox
```

### Windows (vcpkg)

```powershell
# Once: clone + bootstrap vcpkg, install VS C++ tools
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"

.\build.ps1
.\build.ps1 -Test
.\build\Release\ft_vox.exe
```

Or plain CMake:

```powershell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Dependencies

| Component | Role | Linux package (examples) | vcpkg |
|-----------|------|--------------------------|-------|
| SDL3 | Window, input, Vulkan surface | `libsdl3-dev` / `SDL3-devel` | `sdl3[vulkan]` |
| Boost.System / Asio | Networking | `libboost-system-dev` | `boost-asio`, `boost-system` |
| Vulkan headers + loader | API | `libvulkan-dev` | `vulkan-headers`, `vulkan-loader` |
| volk (zeux) | Dynamic Vulkan load | *FetchContent* if missing | `volk` |
| VMA | GPU allocations | *system header or FetchContent* | `vulkan-memory-allocator` |
| glslang / glslc | GLSL → SPIR-V | `glslang-tools` | `glslang` |
| GLM, FastNoise2 | Math / noise | *FetchContent* | — |
| ImGui | UI | vendored `src/imgui/` | — |

CMake resolves this in `cmake/Dependencies.cmake`:

1. CONFIG packages (vcpkg) when a vcpkg toolchain is active  
2. System / pkg-config (`find_package`, `pkg-config`)  
3. FetchContent for volk, VMA, GLM, FastNoise2 (and optional SDL3)

```bash
# Force modes
cmake -B build-vk -DFT_VOX_DEP_MODE=system …
cmake -B build-vk -DFT_VOX_DEP_MODE=vcpkg -DCMAKE_TOOLCHAIN_FILE=…/vcpkg.cmake …
```

## Makefile targets

```text
make deps          # ./install_dep.sh
make configure     # cmake -B build-vk
make / make build  # compile
make test          # ctest
make run ARGS=…    # launch (sets MoltenVK ICD on macOS)
make USE_VCPKG=1   # force vcpkg on Linux/macOS
make USE_VCPKG=0   # force system packages
make print-config  # show resolved toolchain flags
```

## Tests

```bash
make test
# or
cd build-vk && ctest --output-on-failure
```

Windows: `.\build.ps1 -Test`

## Environment variables

| Variable | Purpose |
|----------|---------|
| `VK_ICD_FILENAMES` | Path to MoltenVK (or other) ICD JSON |
| `VK_LAYER_PATH` | Path to validation `explicit_layer.d` |
| `FT_VOX_VALIDATION` | `1` force validation layers on; `0` disable (default: on in Debug) |
| `VCPKG_ROOT` | vcpkg install root (Makefile / `build.ps1`) |

### Optional validation (Debug)

```bash
# macOS
brew install vulkan-validationlayers
export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d

# Linux (Debian/Ubuntu)
sudo apt install vulkan-validationlayers
```

## Controls (in-game)

| Input | Action |
|-------|--------|
| WASD / mouse | Move / look |
| Space / Ctrl | Up / down (fly) |
| LMB / RMB | Break / place block |
| T | Cycle selected block |
| B | Toggle chunk borders |
| Esc | Release mouse (ImGui) |

## Documentation

| Doc | Contents |
|-----|----------|
| [`docs/vulkan-graphics.md`](docs/vulkan-graphics.md) | Vulkan setup, frame graph, passes, shaders, lighting/post |
| [`docs/engine-architecture.md`](docs/engine-architecture.md) | Engine loop, settings/UI, chunks, streaming, terrain gen |
| [`docs/terrain-generation.md`](docs/terrain-generation.md) | Noise graphs, biome/block catalog, extension and calibration procedures |
| [`AGENTS.md`](AGENTS.md) | Contributor-oriented project map and conventions |

## Project layout

```
cmake/
  Dependencies.cmake   # multi-platform package resolution
docs/
  vulkan-graphics.md   # graphics architecture (authoritative)
  engine-architecture.md
  benchmarks/          # profiler dump artifacts
src/
  Vulkan/              # Instance, device, swapchain, VMA, frames, shaders
  Renderer/            # WorldRenderer, Shadow/Opaque/Water/Sky, PostStack, overlays
  Engine/              # Main loop, ImGui layer, input, profiler
  Chunk/               # Voxels, meshing, streaming, terrain generation
  Network/             # UDP client/server (not wired into Engine UI)
  Camera/
ressources/
  shaders/vulkan/      # GLSL sources (compiled to SPIR-V at build time)
  textures/ fonts/ skybox/
vcpkg.json             # Windows / optional Unix vcpkg manifest
install_dep.sh         # apt / dnf / pacman / brew helper
build.ps1              # Windows vcpkg build helper
Makefile               # Unix-friendly cmake wrapper
```

## License

See repository root for license terms.
