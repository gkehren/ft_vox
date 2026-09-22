# Building ft_vox

This page is the detailed companion to the [README Quick start](../README.md#quick-start):
requirements, dependency policy, per-platform setup, build helpers and
runtime environment variables.

## Build requirements

| | Windows | Linux | macOS |
|--|---------|-------|-------|
| CMake | 3.16+ | 3.16+ | 3.16+ |
| Compiler | MSVC 2022 / clang-cl | g++ / clang++ (C++20) | Apple Clang (C++20) |
| Deps | **vcpkg** (recommended) | **apt / dnf / pacman** | **Homebrew** and/or vcpkg |
| Vulkan | GPU driver | mesa / vendor ICD | MoltenVK |
| Shaders | `glslc` or `glslangValidator` | `glslang-tools` | `brew install glslang` |

**Policy:** Linux prefers distro packages. Windows uses vcpkg for almost everything. macOS uses Homebrew for the Vulkan stack; C++ libraries may come from Homebrew or vcpkg.

## Per-platform builds

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
| Boost.System / Asio | Experimental UDP networking prototype (compiled into `test_network` only; Boost is **not linked into the game binary**) | `libboost-system-dev` | `boost-asio`, `boost-system` |
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

The suite also includes the offscreen visual-regression harness — see
[visual-regression.md](visual-regression.md) for scenes, references and
tolerances.

## Environment variables

| Variable | Purpose |
|----------|---------|
| `VK_ICD_FILENAMES` | Path to MoltenVK (or other) ICD JSON |
| `VK_LAYER_PATH` | Path to validation `explicit_layer.d` |
| `FT_VOX_VALIDATION` | `1` force validation layers on; `0` disable (default: on in Debug) |
| `FT_VOX_RESOURCE_PACK` | Resource pack root or `.zip` override (CLI `--resource-pack` wins; default: bundled `default-resource-pack.zip`) |
| `FT_VOX_VULKAN_LIB` | Explicit path to the Vulkan loader library (else well-known loader locations) |
| `VCPKG_ROOT` | vcpkg install root (Makefile / `build.ps1`) |

## Validation layers (optional, Debug)

```bash
# macOS
brew install vulkan-validationlayers
export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d

# Linux (Debian/Ubuntu)
sudo apt install vulkan-validationlayers
```

See [vulkan-validation.md](vulkan-validation.md) for layer setup details,
the error-reporting probe and known overlay interactions (RTSS).
