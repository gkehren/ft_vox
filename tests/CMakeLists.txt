add_executable(test_network
    test_network.cpp
    ${CMAKE_SOURCE_DIR}/src/Network/Server.cpp
    ${CMAKE_SOURCE_DIR}/src/Network/Client.cpp
)

# CPU-only physics: no SDL, Vulkan device, meshes or terrain generation.
add_executable(test_physics test_physics.cpp)
target_include_directories(test_physics PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_physics PRIVATE glm)
add_test(NAME PlayerPhysics COMMAND test_physics)

target_link_libraries(test_network PRIVATE
    Boost::boost
    Boost::system
    Threads::Threads
)
target_include_directories(test_network PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_network PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME NetworkIntegrationTest COMMAND test_network)

# ---------------------------------------------------------------------------
# Vulkan resource smoke (PR2) — real device path via shipped helpers
# ---------------------------------------------------------------------------
add_executable(test_vulkan_resources
    test_vulkan_resources.cpp
    ${FT_VOX_VULKAN_SOURCES}
)

add_dependencies(test_vulkan_resources ft_vox_shaders)

target_include_directories(test_vulkan_resources PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_vulkan_resources PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(APPLE)
    target_compile_definitions(test_vulkan_resources PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_vulkan_resources PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_vulkan_resources PRIVATE
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_vulkan_resources PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(test_vulkan_resources PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

# Place SPIR-V where the test can find them (cwd is usually build/tests)
add_custom_command(TARGET test_vulkan_resources POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:test_vulkan_resources>/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FT_VOX_SPV_FILES}
        $<TARGET_FILE_DIR:test_vulkan_resources>/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_BINARY_DIR}/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FT_VOX_SPV_FILES}
        ${CMAKE_BINARY_DIR}/ressources/shaders/spv
)

add_test(NAME VulkanResourceSmoke COMMAND test_vulkan_resources)
add_test(NAME VulkanValidationErrorReporting COMMAND test_vulkan_resources --validation-error-probe)
set_tests_properties(VulkanValidationErrorReporting PROPERTIES
    WILL_FAIL TRUE
    SKIP_RETURN_CODE 77
)
set_property(TEST VulkanValidationErrorReporting APPEND PROPERTY ENVIRONMENT "FT_VOX_VALIDATION=1")
if(APPLE)
    set_property(TEST VulkanResourceSmoke VulkanValidationErrorReporting APPEND PROPERTY
        ENVIRONMENT "VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json")
endif()
set_tests_properties(VulkanResourceSmoke VulkanValidationErrorReporting PROPERTIES
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_vulkan_resources>
)

# ---------------------------------------------------------------------------
# Visual look: FrameUBO packing of colorBoost/sat + lively biome defaults
# ---------------------------------------------------------------------------
add_executable(test_visual_look
    test_visual_look.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
)

target_include_directories(test_visual_look PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_visual_look PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_visual_look PRIVATE
    glm
    FastNoise2
    Threads::Threads
)

add_test(NAME VisualLookDefaults COMMAND test_visual_look)

# ---------------------------------------------------------------------------
# Auto exposure CPU reference math (issue #140): EV semantics, clamps,
# frame-rate-independent adaptation, GLSL SSBO layout. Header-only.
# ---------------------------------------------------------------------------
add_executable(test_auto_exposure
    test_auto_exposure.cpp
)

target_include_directories(test_auto_exposure PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_auto_exposure PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_auto_exposure PRIVATE
    glm
)

add_test(NAME AutoExposure COMMAND test_auto_exposure)

# ---------------------------------------------------------------------------
# Render helpers (cascades / fog / lighting / post clamps)
# ---------------------------------------------------------------------------
add_executable(test_render_helpers
    test_render_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ResourcePackReader.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureMips.cpp
)

target_include_directories(test_render_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_render_helpers PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    VK_NO_PROTOTYPES
)

target_link_libraries(test_render_helpers PRIVATE
    glm
    miniz::miniz
    volk::volk
)

add_test(NAME RenderHelpers COMMAND test_render_helpers)
set_tests_properties(RenderHelpers PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# Indirect draw emit: page-pair bucket grouping (issue #122 review)
# ---------------------------------------------------------------------------
add_executable(test_indirect_draw_emit
    test_indirect_draw_emit.cpp
)

target_include_directories(test_indirect_draw_emit PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

# CPU/header-only test: no Vulkan surface/window APIs are used, therefore
# no VK_USE_PLATFORM_* definition is required.
target_compile_definitions(test_indirect_draw_emit PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_indirect_draw_emit PRIVATE
    glm
    FastNoise2
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

add_test(NAME IndirectDrawEmit COMMAND test_indirect_draw_emit)

# ---------------------------------------------------------------------------
# Phase D stream CPU helpers (load priority, occupancy, cave Y bounds)
# ---------------------------------------------------------------------------
add_executable(test_stream_opt
    test_stream_opt.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
)

target_include_directories(test_stream_opt PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_stream_opt PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_stream_opt PRIVATE
    glm
    FastNoise2
    Threads::Threads
)

add_test(NAME StreamOptHelpers COMMAND test_stream_opt)

# ---------------------------------------------------------------------------
# Terrain generation: determinism, border consistency, world invariants
# (also hosts the opt-in biome histogram tool: test_terrain --histogram)
# ---------------------------------------------------------------------------
add_executable(test_terrain
    test_terrain.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
)

target_include_directories(test_terrain PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_terrain PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_terrain PRIVATE
    glm
    FastNoise2
    Threads::Threads
)

add_test(NAME TerrainGeneration COMMAND test_terrain)

# ---------------------------------------------------------------------------
# Biome map generation: lifetime safety, cancellation, stale result rejection
# ---------------------------------------------------------------------------
add_executable(test_biome_map
    test_biome_map.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUIBiomeMap.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
)

target_include_directories(test_biome_map PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_biome_map PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_biome_map PRIVATE
    glm
    FastNoise2
    Threads::Threads
)

add_test(NAME BiomeMapGeneration COMMAND test_biome_map)

# ---------------------------------------------------------------------------
# Input routing policy (issue #76): global vs gameplay shortcuts, Escape
# ---------------------------------------------------------------------------
add_executable(test_input_routing
    test_input_routing.cpp
)

target_include_directories(test_input_routing PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_input_routing PRIVATE
    SDL3::SDL3
)

add_test(NAME InputRouting COMMAND test_input_routing)

# ---------------------------------------------------------------------------
# Chunk lifecycle (issue #78 review): move semantics + direct pooled
# generation via Chunk::generateTerrain. Links the Vulkan buffer helpers
# Chunk.cpp depends on; never touches a real device.
# ---------------------------------------------------------------------------
add_executable(test_chunk_lifecycle
    test_chunk_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${FT_VOX_VULKAN_SOURCES}
)

target_include_directories(test_chunk_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_chunk_lifecycle PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(test_chunk_lifecycle PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_chunk_lifecycle PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_chunk_lifecycle PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_chunk_lifecycle PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(test_chunk_lifecycle PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

add_test(NAME ChunkLifecycle COMMAND test_chunk_lifecycle)

# ---------------------------------------------------------------------------
# Greedy mesher face-key contract (issue #106): merge/non-merge boundaries
# (block types, transparency pairs, water, biome tint, chunk borders), key
# path vs forced full-range byte equality, multi-worker determinism. Also
# hosts the opt-in corpus tools:
#   test_mesh_facekey --hash-corpus [chunks] [stride]
#   test_mesh_facekey --bench-corpus [chunks] [stride]
# ---------------------------------------------------------------------------
add_executable(test_mesh_facekey
    test_mesh_facekey.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${FT_VOX_VULKAN_SOURCES}
)

target_include_directories(test_mesh_facekey PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_mesh_facekey PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(test_mesh_facekey PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_mesh_facekey PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_mesh_facekey PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_mesh_facekey PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(test_mesh_facekey PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

add_test(NAME MeshFaceKey COMMAND test_mesh_facekey)

# ---------------------------------------------------------------------------
# Mesh upload lifecycle (issue #104, review #114 items 25-28): staging-full
# deferral keeping the exact result, retry without rebuild, partial
# opaque/water staging rollback, old GPU mesh untouched. Uses a headless
# Vulkan device; skips quietly when no loader/device is available.
# ---------------------------------------------------------------------------
add_executable(test_mesh_upload
    test_mesh_upload.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${FT_VOX_VULKAN_SOURCES}
)

target_include_directories(test_mesh_upload PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_mesh_upload PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(test_mesh_upload PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_mesh_upload PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_mesh_upload PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_mesh_upload PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(test_mesh_upload PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

add_test(NAME MeshUploadLifecycle COMMAND test_mesh_upload)

# ---------------------------------------------------------------------------
# VoxelPool contract (issue #112 review): ownership, high-water reuse,
# foreign/double release refusal, concurrency, growth events.
# ---------------------------------------------------------------------------
add_executable(test_voxel_pool
    test_voxel_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
)

target_include_directories(test_voxel_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_voxel_pool PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_voxel_pool PRIVATE
    glm
    Threads::Threads
)

add_test(NAME VoxelPool COMMAND test_voxel_pool)

# ---------------------------------------------------------------------------
# BorderPool contract (issue #113 review): compact transient border blocks,
# ownership refusal, concurrency, growth events.
# ---------------------------------------------------------------------------
add_executable(test_border_pool
    test_border_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
)

target_include_directories(test_border_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_border_pool PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_border_pool PRIVATE
    glm
    Threads::Threads
)

add_test(NAME BorderPool COMMAND test_border_pool)

# ---------------------------------------------------------------------------
# MeshResultPool contract (issue #104): reusable mesh build results detached
# from Chunk lifetime - identity bookkeeping, capacity retention, growth
# events, concurrency, release refusal.
# ---------------------------------------------------------------------------
add_executable(test_mesh_result_pool
    test_mesh_result_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
)

target_include_directories(test_mesh_result_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_mesh_result_pool PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_mesh_result_pool PRIVATE
    glm
    Threads::Threads
)

add_test(NAME MeshResultPool COMMAND test_mesh_result_pool)

# ---------------------------------------------------------------------------
# Profiler worker consistency and stress test (Issue #79)
# ---------------------------------------------------------------------------
add_executable(test_profiler
    test_profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
)

target_include_directories(test_profiler PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_profiler PRIVATE
    Threads::Threads
)

add_test(NAME ProfilerWorkerConsistency COMMAND test_profiler)

add_executable(test_gpu_profile
    test_gpu_profile.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Benchmark.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/BuildInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
)
add_dependencies(test_gpu_profile ft_vox_build_info)
target_include_directories(test_gpu_profile PRIVATE ${CMAKE_SOURCE_DIR}/src ${FT_VOX_GENERATED_DIR})
target_link_libraries(test_gpu_profile PRIVATE glm SDL3::SDL3)
add_test(NAME GpuProfile COMMAND test_gpu_profile)

add_executable(test_workload_telemetry test_workload_telemetry.cpp)
target_include_directories(test_workload_telemetry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_workload_telemetry PRIVATE glm Threads::Threads)
add_test(NAME WorkloadTelemetry COMMAND test_workload_telemetry)
set_tests_properties(WorkloadTelemetry PROPERTIES ENVIRONMENT "FT_VOX_TELEMETRY=1")
add_test(NAME WorkloadTelemetryDisabled COMMAND test_workload_telemetry)
set_tests_properties(WorkloadTelemetryDisabled PROPERTIES ENVIRONMENT "FT_VOX_TELEMETRY=0")

# ---------------------------------------------------------------------------
# MeshArena allocator unit tests (issue #109): aligned suballocation,
# first-fit reuse, frame-aware retirement, page growth and exhaustion.
# Headless Vulkan device; skips quietly when no loader/device is available.
# ---------------------------------------------------------------------------
add_executable(test_mesh_arena
    test_mesh_arena.cpp
    ${FT_VOX_VULKAN_SOURCES}
)
target_include_directories(test_mesh_arena PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(test_mesh_arena PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
if(APPLE)
    target_compile_definitions(test_mesh_arena PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_mesh_arena PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
target_link_libraries(test_mesh_arena PRIVATE
    glm
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)
add_test(NAME MeshArena COMMAND test_mesh_arena)

# Passive entities: CPU simulation/model contracts, no graphics device.
add_executable(test_mobs test_mobs.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobModel.cpp)
target_include_directories(test_mobs PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mobs PRIVATE glm)
add_test(NAME PassiveMobs COMMAND test_mobs)

add_executable(test_mob_textures test_mob_textures.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/MobTextures.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ResourcePackReader.cpp)
target_include_directories(test_mob_textures PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mob_textures PRIVATE miniz::miniz)
add_test(NAME MobTextures COMMAND test_mob_textures ${CMAKE_SOURCE_DIR}/ressources/default-resource-pack.zip)

# Offscreen Vulkan evidence: actual mob pipelines, texture rollback and frame slots.
add_executable(test_mob_render test_mob_render.cpp
    ${FT_VOX_VULKAN_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Entities/MobModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/MobRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/MobTextures.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ResourcePackReader.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureMips.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp)
target_include_directories(test_mob_render PRIVATE ${CMAKE_SOURCE_DIR}/src ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(test_mob_render PRIVATE VK_NO_PROTOTYPES GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_link_libraries(test_mob_render PRIVATE SDL3::SDL3 volk::volk GPUOpen::VulkanMemoryAllocator glm miniz::miniz Threads::Threads)
if(APPLE)
    target_compile_definitions(test_mob_render PRIVATE VK_USE_PLATFORM_METAL_EXT)
    target_link_libraries(test_mob_render PRIVATE "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework IOKit" "-framework IOSurface")
elseif(WIN32)
    target_compile_definitions(test_mob_render PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX)
    target_link_libraries(test_mob_render PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(test_mob_render ft_vox_shaders)
add_test(NAME MobVulkanRender COMMAND test_mob_render ${CMAKE_BINARY_DIR}/mob-qa)
set_tests_properties(MobVulkanRender PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "FT_VOX_VALIDATION=1")

add_test(NAME MobRealTerrain COMMAND test_chunk_lifecycle --mobs-profile)

# ---------------------------------------------------------------------------
# Comparison plumbing for the visual harness: metric units, hot-pixel
# counting, PNG round-trip (pure CPU — guards the tolerance-unit class of
# bug found in review).
# ---------------------------------------------------------------------------
add_executable(test_visual_image
    test_visual_image.cpp
    VisualImage.cpp
)
target_include_directories(test_visual_image PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_visual_image PRIVATE miniz::miniz)
add_test(NAME VisualImageMetrics COMMAND test_visual_image)

# ---------------------------------------------------------------------------
# Deterministic visual regression (issue #142): fixed scenes rendered offscreen
# through the production WorldRenderer pass graph, tolerant comparison against
# committed references in tests/visual-references/. References are only
# written by the explicit --update-references developer action.
#   ft_vox_visual_tests --update-references
#
# ctest runs in SMOKE mode (FT_VOX_VISUAL_SMOKE=1) so heterogeneous GPUs
# don't go false-red; the strict canonical-GPU gate is a direct run without
# --smoke (see docs/visual-regression.md).
# ---------------------------------------------------------------------------
add_executable(ft_vox_visual_tests
    test_visual_regression.cpp
    VisualHarness.cpp
    VisualImage.cpp
    ${FT_VOX_VULKAN_SOURCES}
    ${FT_VOX_RENDERER_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp)

target_include_directories(ft_vox_visual_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(ft_vox_visual_tests PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
if(APPLE)
    target_compile_definitions(ft_vox_visual_tests PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(ft_vox_visual_tests PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
target_link_libraries(ft_vox_visual_tests PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    miniz::miniz
    Threads::Threads
)
if(UNIX AND NOT APPLE)
    target_link_libraries(ft_vox_visual_tests PRIVATE ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    target_link_libraries(ft_vox_visual_tests PRIVATE
        "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework IOKit" "-framework IOSurface")
endif()
add_dependencies(ft_vox_visual_tests ft_vox_shaders)
add_custom_command(TARGET ft_vox_visual_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:ft_vox_visual_tests>/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FT_VOX_SPV_FILES}
            $<TARGET_FILE_DIR:ft_vox_visual_tests>/ressources/shaders/spv)
add_test(NAME VisualRegression COMMAND ft_vox_visual_tests
    ${CMAKE_SOURCE_DIR}/tests/visual-references ${CMAKE_BINARY_DIR}/visual-qa)
set_tests_properties(VisualRegression PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ENVIRONMENT "FT_VOX_VALIDATION=1;FT_VOX_VISUAL_SMOKE=1"
    SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# VSync frame pacing (PR #145): pure computePacedDeltaTime contract
# ---------------------------------------------------------------------------
add_executable(test_frame_pacing
    test_frame_pacing.cpp
)

target_include_directories(test_frame_pacing PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME FramePacing COMMAND test_frame_pacing)
