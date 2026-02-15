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

- Linux example: `<path-to-vcpkg-folder>/scripts/buildsystems/vcpkg.cmake`
- Windows example: `C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake`

You can also export `VCPKG_ROOT` and use:

- Linux: `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`
- Windows: `%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`

---

## Linux Build

From the project root:

```bash
mkdir -p build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg-toolchain> -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) -C build
./build/ft_vox
```

Notes:

- Use `-DCMAKE_BUILD_TYPE=Release` for optimized performance.
- The executable is generated in `build/`.

---

## Windows Build

### Visual Studio generator

From a Developer PowerShell in the project root:

```powershell
mkdir build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg-toolchain>
cmake --build build --config Release
.\build\Release\ft_vox.exe
```
