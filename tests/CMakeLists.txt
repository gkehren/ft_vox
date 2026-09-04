add_executable(test_network
    test_network.cpp
    ${CMAKE_SOURCE_DIR}/src/Network/Server.cpp
    ${CMAKE_SOURCE_DIR}/src/Network/Client.cpp
)

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
# Render helpers (cascades / fog / lighting / post clamps)
# ---------------------------------------------------------------------------
add_executable(test_render_helpers
    test_render_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ResourcePackReader.cpp
)

target_include_directories(test_render_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_render_helpers PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_render_helpers PRIVATE
    glm
    miniz::miniz
)

add_test(NAME RenderHelpers COMMAND test_render_helpers)
set_tests_properties(RenderHelpers PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

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
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
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
target_link_libraries(test_workload_telemetry PRIVATE Threads::Threads)
add_test(NAME WorkloadTelemetry COMMAND test_workload_telemetry)
set_tests_properties(WorkloadTelemetry PROPERTIES ENVIRONMENT "FT_VOX_TELEMETRY=1")
add_test(NAME WorkloadTelemetryDisabled COMMAND test_workload_telemetry)
set_tests_properties(WorkloadTelemetryDisabled PROPERTIES ENVIRONMENT "FT_VOX_TELEMETRY=0")
