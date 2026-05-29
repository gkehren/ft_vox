# ft_vox Project Context

`ft_vox` is a high-performance voxel sandbox engine and game built from scratch using C++20 and OpenGL. It features a procedurally generated world with infinite terrain, biomes, networking, and modern rendering techniques.

## Architecture Overview

### Core Engine
- **Engine (`src/Engine/`)**: Manages the main game loop, window creation (SDL3), input handling, and high-level systems like the `UIManager` and `ThreadPool`.
- **Chunk Management (`src/Chunk/`)**:
  - `Chunk`: Represents a 16x256x16 grid of voxels. Handles mesh generation (greedy meshing/face culling) and GPU uploads.
  - `ChunkManager`: Handles loading/unloading chunks around the player, multithreaded generation, and LOD management.
  - `TerrainGenerator`: Uses `FastNoise2` to generate terrain height, caves, and biomes based on temperature and humidity maps.

### Rendering (`src/Renderer/`)
- **Renderer**: The main rendering pipeline. Handles opaque, transparent, and post-processing passes.
- **Post-Processing**: Implements Bloom, God Rays, and other screen-space effects using GLSL shaders.
- **TextRenderer**: Renders UI text using FreeType.
- **TextureManager**: Manages a 2D Texture Array for voxel textures to minimize draw calls and state changes.

### Networking (`src/Network/`)
- **Client/Server**: Built on `Boost.Asio` using UDP for low-latency player position and world state synchronization.

## Technologies & Dependencies
- **Language**: C++20
- **Graphics API**: OpenGL 4.1+ (using `glad`)
- **Windowing/Input**: SDL3
- **Mathematics**: GLM
- **Terrain Generation**: FastNoise2
- **Networking**: Boost.Asio
- **Font Rendering**: FreeType
- **UI**: ImGui
- **Build System**: CMake (3.16+)

## Development Guide

### Building the Project

The project uses `vcpkg` for dependency management.

vcpkg current path: `D:\Projects\vcpkg\scripts\buildsystems\vcpkg.cmake`

#### Windows (Visual Studio)
```powershell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=D:\Projects\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

#### Linux/macOS
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Key Conventions
- **Voxel Data**: Stored as a flat array of `uint8_t` types in each `Chunk`.
- **World Constants**: Defined in `src/utils.hpp` (e.g., `CHUNK_SIZE`, `WORLD_HEIGHT`).
- **Resource Management**: Shaders, textures, and fonts are located in the `ressources/` directory. The engine expects `RES_PATH` to point to this directory.
- **Threading**: Use `ThreadPool` for heavy tasks like chunk generation and meshing to keep the frame rate stable.

## Performance Considerations
- **Greedy Meshing**: Chunks should implement face culling and ideally greedy meshing to reduce vertex count.
- **Texture Arrays**: Use `sampler2DArray` in shaders to avoid re-binding textures for different block types.
- **Frustum Culling**: Chunks are culled by the `Renderer` if they are outside the camera's view.
