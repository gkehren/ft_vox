# ft_vox

Voxel sandbox project built with C++20, SDL3, OpenGL, ImGui, and CMake.

## Build Requirements

- `cmake` (3.16+)
- C++20 compiler
  - Linux: `g++` or `clang++`
  - Windows: MSVC (Visual Studio) or MinGW
- `git`
- `vcpkg` (recommended)

## Dependencies

The project uses:

- SDL3
- OpenGL
- Boost
- FreeType
- GLM
- FastNoise2

Using **vcpkg** is the easiest way to manage dependencies across Linux and Windows.

## Build With vcpkg (Recommended)

Set your vcpkg toolchain path:

- Linux example: `/home/<user>/Documents/vcpkg/scripts/buildsystems/vcpkg.cmake`
- Windows example: `C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake`

You can also export `VCPKG_ROOT` and use:

- Linux: `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`
- Windows: `%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`

---

## Linux Build

From the project root:

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/home/<user>/Documents/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./ft_vox
```

Notes:

- Use `-DCMAKE_BUILD_TYPE=Release` for optimized performance.
- The executable is generated in `build/`.

---

## Windows Build

### Option A: Visual Studio generator

From a Developer PowerShell in the project root:

```powershell
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
.\Release\ft_vox.exe
```

### Option B: Ninja generator

```powershell
mkdir build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build .
.\ft_vox.exe
```

## Rebuild From Scratch

If you want a clean rebuild:

```bash
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg-toolchain> -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```
