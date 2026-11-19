# Experimental UDP prototype: test-only target, requires Boost (optional dep).
if(TARGET Boost::system)
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
else()
    message(STATUS "test_network disabled: Boost not found")
endif()

# CPU-only physics: no SDL, Vulkan device, meshes or terrain generation.
add_executable(test_physics test_physics.cpp)
target_include_directories(test_physics PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_physics PRIVATE glm)
add_test(NAME PlayerPhysics COMMAND test_physics)

# ---------------------------------------------------------------------------
# CPU-only simulation/render contracts (issue #205, Epic #204 Phase A).
# Links ONLY ft_vox_core (transitively glm + FastNoise2 + Threads): no SDL,
# no Vulkan device, no ImGui. Exercises a symbol from every core translation
# unit and pins the 120 Hz player / 60 Hz mob tick rates + fixed-step
# determinism. ft_vox_core's PUBLIC include dir / GLM defs propagate here.
# ---------------------------------------------------------------------------
add_executable(test_core_contracts test_core_contracts.cpp)
target_link_libraries(test_core_contracts PRIVATE ft_vox_core)
add_test(NAME CoreContracts COMMAND test_core_contracts)

# ---------------------------------------------------------------------------
# CPU-only authoritative world acceptance (issue #206, Epic #204 Phase B).
# Links ONLY ft_vox_core: constructs, generates, samples, edits, unloads,
# reloads and persists a world with NO graphics runtime (no SDL / Vulkan /
# ImGui / render chunk handles). Covers the issue #206 acceptance list:
# determinism, border/corner mirrors, deferred + coalesced edits, negative
# coordinates, unknown terrain, safe recycling, out-of-order generation
# completion, stale revision rejection, and the issue #180 save contracts
# (restart, sparse normalization, identity guards, close failure/retry,
# concurrent edits during pending async I/O).
# ---------------------------------------------------------------------------
add_executable(test_voxel_world test_voxel_world.cpp)
target_link_libraries(test_voxel_world PRIVATE ft_vox_core)
add_test(NAME VoxelWorld COMMAND test_voxel_world)

# ---------------------------------------------------------------------------
# CPU-only simulation facade acceptance (issue #208, Epic #204 Phase D).
# Links ONLY ft_vox_core: creates a GameSimulation for a fixed seed and
# advances it with scripted inputs/edits with NO graphics runtime, camera or
# window. Covers the issue #208 acceptance list: scripted determinism,
# physics edge fixtures (jump/coyote/swim/flight, unknown terrain, recovery,
# dropped-step cap, suspension), mob spawn/retire/interpolation fixtures,
# reload + save/player restore (failures keep the old world alive), and the
# independent 120 Hz / 60 Hz update rates + bounded value snapshots.
# ---------------------------------------------------------------------------
add_executable(test_game_simulation test_game_simulation.cpp)
target_link_libraries(test_game_simulation PRIVATE ft_vox_core)
add_test(NAME GameSimulation COMMAND test_game_simulation)

# ---------------------------------------------------------------------------
# CPU-only gameplay-command acceptance (issue #225, Epic #223 Phase B). Links
# ONLY ft_vox_core: the facade's inventory ownership, hotbar/inventory command
# result matrix, the logged grantItems admin seam, deterministic command
# sequences, bounded in-place InventorySnapshot, lifecycle resets (incl. the
# refused-reload contract) and suspension without command replay — no GPU,
# window or camera.
# ---------------------------------------------------------------------------
add_executable(test_gameplay test_gameplay.cpp)
target_link_libraries(test_gameplay PRIVATE ft_vox_core)
add_test(NAME GameplayLoop COMMAND test_gameplay)

# ---------------------------------------------------------------------------
# CPU-only multi-player session acceptance (issue #246, Epic #245 Phase A).
# Links ONLY ft_vox_core: one shared world hosting N independent player
# sessions — id allocation/retirement semantics, pointer-stable session
# references (PR #261 review), per-player physics/inventory isolation,
# deterministic id-sorted iteration, stale-handle safety, removal isolation
# (world/mobs/items/other players untouched), world-epoch lifecycle and the
# solo-compatibility forwards (legacy APIs == local-player APIs).
# No GPU, window or camera.
# ---------------------------------------------------------------------------
add_executable(test_player_sessions test_player_sessions.cpp)
target_link_libraries(test_player_sessions PRIVATE ft_vox_core)
add_test(NAME PlayerSessions COMMAND test_player_sessions)

add_executable(test_player_streaming test_player_streaming.cpp)
target_link_libraries(test_player_streaming PRIVATE ft_vox_core)
add_test(NAME PlayerStreaming COMMAND test_player_streaming)

# ---------------------------------------------------------------------------
# CPU-only actor-aware gameplay acceptance (issue #247, Epic #245 Phase A2).
# Links ONLY ft_vox_core: actor-aware break/place/drop/targeting, the all-body
# placement rule, deterministic shared pickup (ascending-PlayerId priority,
# conserved partial merges), invalid-actor / overlap / unknown-terrain /
# out-of-range rejections without side effects, per-player id-exhaustion
# conservation, deferred edits and the solo-compatibility forwards
# (legacy == actor-aware(local) bit for bit). No GPU, window or camera.
# ---------------------------------------------------------------------------
add_executable(test_actor_interactions test_actor_interactions.cpp)
target_link_libraries(test_actor_interactions PRIVATE ft_vox_core)
add_test(NAME ActorInteractions COMMAND test_actor_interactions)

# ---------------------------------------------------------------------------
# CPU-only item domain acceptance (issue #224, Epic #223 Phase A). Links ONLY
# ft_vox_core: registry invariants (unique ids/names, append-only ordinals,
# placeable round-trips, drop-table totality, name parsing), pure stack math
# edges and the deterministic 36-slot inventory operation matrix — no GPU,
# window or simulation integration (nothing consumes the module yet).
# ---------------------------------------------------------------------------
add_executable(test_items test_items.cpp)
target_link_libraries(test_items PRIVATE ft_vox_core)
add_test(NAME Items COMMAND test_items)

# Forbidden-include-chain guard (issue #205 acceptance criterion #3): the
# CPU-only public headers must never transitively include Renderer/, Vulkan/,
# Camera/, SDL3, ImGui, or the render-coupled / not-yet-migrated Chunk headers.
# Scripted, stdlib-only. Python3 is REQUIRED whenever tests are enabled
# (BUILD_TESTING): the guard is an acceptance gate and must not silently
# disappear on a machine without an interpreter.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME CoreIncludeGuard
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_core_includes.py)

# GPU workflow trust-policy guard (PR #244 review): the optional GPU workflow
# must stay dispatch-only on the protected main ref, with minimal permissions,
# no persisted checkout credentials, no privileged installation and no secret
# exposure — untrusted PR code can never reach the persistent GPU runner.
# Stdlib-only structural checks; cpu-boundary.yml's per-PR gate is asserted
# intact. Registered here so every CPU CI run enforces the security contract.
add_test(NAME GpuWorkflowTrustGuard
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_gpu_workflow.py
        --repo-root ${CMAKE_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Vulkan resource smoke (PR2) — real device path via shipped helpers
# GRAPHICAL (FT_VOX_GRAPHICS): needs SDL3 + volk + VMA + SPIR-V.
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
add_executable(test_vulkan_resources
    test_vulkan_resources.cpp
    # Test-only Vulkan smoke helper: sole consumer is this target; the game
    # binary and the other device tests do not compile or ship it.
    VulkanResourceSmoke.cpp
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
endif() # FT_VOX_GRAPHICS (Vulkan resource smoke)

# ---------------------------------------------------------------------------
# Visual look: FrameUBO packing of colorBoost/sat + lively biome defaults
# CPU-only: compiles the terrain generator and scans shader SOURCE text; no
# Vulkan/SDL/shader-compiler dependency (links glm + FastNoise2 + Threads).
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
    # Absolute source-tree anchor so source-contract scans work from any
    # build directory (out-of-tree builds included), issue #165 review.
    FT_VOX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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
# Graphics settings core (issue #185): canonical presetValues, truthful
# Custom detection via matchesPreset, scoped panel resets. Header-only.
# ---------------------------------------------------------------------------
add_executable(test_graphics_settings
    test_graphics_settings.cpp
)

target_include_directories(test_graphics_settings PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_graphics_settings PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_graphics_settings PRIVATE
    glm
)

add_test(NAME GraphicsSettings COMMAND test_graphics_settings)

# ---------------------------------------------------------------------------
# Render helpers (cascades / fog / lighting / post clamps)
# GRAPHICAL (FT_VOX_GRAPHICS): links miniz + volk.
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
add_executable(test_render_helpers
    test_render_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ResourcePackReader.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureMips.cpp
)

# The FrameUBO contract scan reads the generated frame_ubo.inc.glsl; declare
# the dependency so an isolated `--target test_render_helpers` build still
# generates it (the generator target is not ALL by itself through shaders).
add_dependencies(test_render_helpers ft_vox_frame_ubo_glsl)

target_include_directories(test_render_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_render_helpers PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    VK_NO_PROTOTYPES
    # Absolute source-tree anchor so the shader source-contract scans work
    # from any build directory (out-of-tree builds included), issue #165 review.
    FT_VOX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    # Absolute build-tree anchor for the generated frame_ubo.inc.glsl contract.
    FT_VOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
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

# SDL3 is a header-only dependency here: IndirectDrawEmit.hpp -> Chunk.hpp ->
# Vulkan/VkContext.hpp includes SDL3/SDL.h. On Linux SDL3 headers are not
# globally visible (unlike vcpkg on Windows), so the target must carry them.
target_link_libraries(test_indirect_draw_emit PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

add_test(NAME IndirectDrawEmit COMMAND test_indirect_draw_emit)
endif() # FT_VOX_GRAPHICS (render helpers + indirect draw emit)

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
# GRAPHICAL (FT_VOX_GRAPHICS): this region runs through test_mesh_upload —
# every target here links SDL3 and/or the Vulkan stack.
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
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
# UI shell pure logic (issues #183/#184): scale normalization, status-strip
# degradation, shortcut metadata consistency with InputRouting, and the
# player/status-overlay UI model (motion labels, chunk math, block palette,
# overlay density). Issue #229 adds the inventory presentation logic
# (InventoryUi.hpp layout/cursor/click planning; the Items stack-math TUs
# provide splitHalf/maxStackOf).
# ---------------------------------------------------------------------------
add_executable(test_ui_shell
    test_ui_shell.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemStack.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
)

target_include_directories(test_ui_shell PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_ui_shell PRIVATE
    SDL3::SDL3
    glm
)

add_test(NAME UiShell COMMAND test_ui_shell)

# ---------------------------------------------------------------------------
# Chunk lifecycle (issue #78 review): move semantics + direct pooled
# generation via Chunk::generateTerrain. Links the Vulkan buffer helpers
# Chunk.cpp depends on; never touches a real device.
# ---------------------------------------------------------------------------
add_executable(test_chunk_lifecycle
    test_chunk_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
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
# Render bridge boundary (issue #207, Epic #204 Phase C): the CPU world runs
# with no bridge; the bridge is a derived projection referencing the host-owned
# world; the world outlives the bridge and a fresh bridge re-attaches; mesh
# publish validates sim::ChunkIdentity; border edits form atomic commit groups.
# Real Chunk/RenderWorldBridge/TerrainGenerator, no Vulkan device (same headless
# setup as test_chunk_lifecycle).
# ---------------------------------------------------------------------------
add_executable(test_render_bridge
    test_render_bridge.cpp
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
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

target_include_directories(test_render_bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_render_bridge PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(test_render_bridge PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_render_bridge PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_render_bridge PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_render_bridge PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(test_render_bridge PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

add_test(NAME RenderBridge COMMAND test_render_bridge)

# ---------------------------------------------------------------------------
# World persistence (issue #180 Phase 3+4): SaveService ordering/superseding,
# WorldPersistence open/create identity semantics, sparse-save diffing against
# regenerated terrain, recycled-chunk payload isolation, and the RenderWorldBridge
# unload/reload integration. Real Chunk/RenderWorldBridge/TerrainGenerator, no
# Vulkan device (same headless setup as test_chunk_lifecycle).
# ---------------------------------------------------------------------------
add_executable(test_world_persistence
    test_world_persistence.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
    ${FT_VOX_VULKAN_SOURCES}
)

target_include_directories(test_world_persistence PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_world_persistence PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(test_world_persistence PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_world_persistence PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_world_persistence PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_world_persistence PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(test_world_persistence PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

add_test(NAME WorldPersistence COMMAND test_world_persistence)

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
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
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
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
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
endif() # FT_VOX_GRAPHICS (input routing .. mesh upload)

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

# GRAPHICAL (FT_VOX_GRAPHICS): links SDL3 + the Engine benchmark units.
if(FT_VOX_GRAPHICS)
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
endif() # FT_VOX_GRAPHICS (GPU profile)

add_executable(test_workload_telemetry test_workload_telemetry.cpp)
target_include_directories(test_workload_telemetry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_workload_telemetry PRIVATE glm Threads::Threads)
add_test(NAME WorkloadTelemetry COMMAND test_workload_telemetry)
set_tests_properties(WorkloadTelemetry PROPERTIES ENVIRONMENT "FT_VOX_TELEMETRY=1")
add_test(NAME WorkloadTelemetryDisabled COMMAND test_workload_telemetry)
set_tests_properties(WorkloadTelemetryDisabled PROPERTIES ENVIRONMENT "FT_VOX_TELEMETRY=0")

# ---------------------------------------------------------------------------
# Developer-console core logic (issue #179): formatting, ring histories,
# health hysteresis, chunk debug snapshot extraction, bounded event trace
# and the throttled UI-state sampler. No ImGui, no window, no GPU work.
# GRAPHICAL (FT_VOX_GRAPHICS): compiles the render bridge + Vulkan units.
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
add_executable(test_debug_ui
    test_debug_ui.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugUiCore.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugUiState.cpp
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
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
target_include_directories(test_debug_ui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(test_debug_ui PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
if(APPLE)
    target_compile_definitions(test_debug_ui PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_debug_ui PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_debug_ui PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_debug_ui PRIVATE ${CMAKE_DL_LIBS})
endif()

add_test(NAME DebugUiCore COMMAND test_debug_ui)

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
endif() # FT_VOX_GRAPHICS (debug UI + mesh arena)

# Passive entities: CPU simulation/model contracts, no graphics device.
add_executable(test_mobs test_mobs.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobModel.cpp)
target_include_directories(test_mobs PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mobs PRIVATE glm)
add_test(NAME PassiveMobs COMMAND test_mobs)

# GRAPHICAL (FT_VOX_GRAPHICS): this region runs through test_post_resize —
# miniz resource packs, real Vulkan pipelines and the visual harness.
if(FT_VOX_GRAPHICS)
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
# resolveSpvPath prefers the executable directory, so fresh SPIR-V must be
# copied there — without this, stale hand-copied shaders shadow the current
# ones (the issue #128 skylight check silently failed against Sep-6 binaries).
add_custom_command(TARGET test_mob_render POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:test_mob_render>/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FT_VOX_SPV_FILES}
            $<TARGET_FILE_DIR:test_mob_render>/ressources/shaders/spv)
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
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
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
# Issue #156 regression: PostStack::resize() across a swapchain
# format/color-space change must keep every pipeline layout valid. Drives
# PostStack directly through A -> B -> A output pairs (plus an extent-only
# resize) with full post frames and a silent Vulkan validation layer.
# ---------------------------------------------------------------------------
add_executable(test_post_resize
    test_post_resize.cpp
    ${FT_VOX_VULKAN_SOURCES}
    ${FT_VOX_RENDERER_SOURCES}
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp)

target_include_directories(test_post_resize PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(test_post_resize PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
if(APPLE)
    target_compile_definitions(test_post_resize PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_post_resize PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
target_link_libraries(test_post_resize PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    miniz::miniz
    Threads::Threads
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_post_resize PRIVATE ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    target_link_libraries(test_post_resize PRIVATE
        "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework IOKit" "-framework IOSurface")
endif()
add_dependencies(test_post_resize ft_vox_shaders)
add_custom_command(TARGET test_post_resize POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:test_post_resize>/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FT_VOX_SPV_FILES}
            $<TARGET_FILE_DIR:test_post_resize>/ressources/shaders/spv)
add_test(NAME PostResizeFormats COMMAND test_post_resize)
set_tests_properties(PostResizeFormats PROPERTIES
    ENVIRONMENT "FT_VOX_VALIDATION=1"
    SKIP_RETURN_CODE 77)
endif() # FT_VOX_GRAPHICS (mob textures .. post resize)

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

# ---------------------------------------------------------------------------
# World-save format layer (issue #180 Phases 1+2): world.meta / chunk-file
# round trips, corruption taxonomy, sparse-diff semantics, atomic replace and
# ".tmp" hygiene. CPU-only: compiles the format layer directly.
# ---------------------------------------------------------------------------
add_executable(test_world_save
    test_world_save.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
)

target_include_directories(test_world_save PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_world_save PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(test_world_save PRIVATE
    glm
    Threads::Threads
)

add_test(NAME WorldSave COMMAND test_world_save)

# ---------------------------------------------------------------------------
# Gameplay persistence checkpoints (stabilization Phase 3, PR5a): manifest
# codec, generation staging/commit/recovery, the deterministic fault-injection
# matrix, retention/cleanup and legacy compatibility. CPU-only: links ONLY
# ft_vox_core (the store is a pure filesystem/format component).
# ---------------------------------------------------------------------------
add_executable(test_world_checkpoint test_world_checkpoint.cpp)
target_link_libraries(test_world_checkpoint PRIVATE ft_vox_core)
add_test(NAME WorldCheckpoint COMMAND test_world_checkpoint)

# ---------------------------------------------------------------------------
# Engine-level lifecycle: persistent world x reloadWorld (issue #180 review
# round 3, item 12). Compiles the full engine minus main.cpp; needs a
# GPU/window so it skips (77) when the engine cannot be constructed.
# GRAPHICAL (FT_VOX_GRAPHICS): full engine + bench_persistence to EOF.
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
set(FT_VOX_ENGINE_TEST_SOURCES ${FT_VOX_SOURCES})
list(REMOVE_ITEM FT_VOX_ENGINE_TEST_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(test_engine_reload
    test_engine_reload.cpp
    ${FT_VOX_ENGINE_TEST_SOURCES}
)

add_dependencies(test_engine_reload ft_vox_shaders ft_vox_build_info)

target_include_directories(test_engine_reload PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/imgui
    ${CMAKE_SOURCE_DIR}/src/ImGuiFileDialog
    ${FT_VOX_GENERATED_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(test_engine_reload PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(test_engine_reload PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(test_engine_reload PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(test_engine_reload PRIVATE
    ${FT_VOX_COMMON_LIBS}
)


add_test(NAME EngineReload COMMAND test_engine_reload)
set_tests_properties(EngineReload PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FT_VOX_VALIDATION=1"
)

# ---------------------------------------------------------------------------
# Persistence stress benchmark (issue #180 review round 6, item 8). Manual
# benchmark - deliberately NOT registered with ctest: it runs ~2x the given
# duration with real disk writes.
# ---------------------------------------------------------------------------
add_executable(bench_persistence
    bench_persistence.cpp
    ${FT_VOX_CHUNK_SOURCES}
    ${FT_VOX_WORLD_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${FT_VOX_VULKAN_SOURCES}
)

target_include_directories(bench_persistence PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(bench_persistence PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(bench_persistence PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(bench_persistence PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

target_link_libraries(bench_persistence PRIVATE
    glm
    FastNoise2
    SDL3::SDL3
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)
endif() # FT_VOX_GRAPHICS (engine reload + bench persistence)

# ---------------------------------------------------------------------------
# Dropped-item simulation benchmark (stabilization PR4, Phase 5). CPU-only:
# links ONLY ft_vox_core (no GPU/window/camera). Manual benchmark —
# deliberately NOT registered with ctest (each scenario runs reps x
# (warmup+ticks) frames with real terrain bootstrap):
#   bench_gameplay_items --reps 5 --json results.json --csv results.csv
# The same source builds against the baseline commit (public facade APIs
# only), so baseline-vs-current comparisons share the exact methodology.
# ---------------------------------------------------------------------------
add_executable(bench_gameplay_items bench_gameplay_items.cpp)
target_link_libraries(bench_gameplay_items PRIVATE ft_vox_core)

# ---------------------------------------------------------------------------
# Headless simulation runner (issue #210, Epic #204 Phase F). CPU-only: the
# runner links ONLY ft_vox_core, and every test below runs without a GPU,
# window or shader asset. All integration tests have bounded wall-clock
# timeouts and deterministic end conditions (fixed tick budgets).
# ---------------------------------------------------------------------------

# Smoke: a plain fixed-seed run must exit 0 and print the SUMMARY line.
add_test(NAME HeadlessSmoke COMMAND ft_vox_headless --seed 42 --ticks 120)
set_tests_properties(HeadlessSmoke PROPERTIES TIMEOUT 120)

# Full acceptance driver: scripted determinism (sync + async workers),
# save -> process exit -> reload -> edit verification in an isolated temp
# directory, invalid-script / assertion / world-open failure exit codes, and
# bounded teardown. Python3 stdlib only (already REQUIRED above).
add_test(NAME HeadlessRunner
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_headless_runner.py
        --runner $<TARGET_FILE:ft_vox_headless>
        --fixtures ${CMAKE_SOURCE_DIR}/tests/fixtures)
# This driver runs many headless processes sequentially. CTest's per-test
# TIMEOUT takes precedence over the workflow's default timeout.
if(FT_VOX_SANITIZERS)
    set_tests_properties(HeadlessRunner PROPERTIES TIMEOUT 1800)
else()
    set_tests_properties(HeadlessRunner PROPERTIES TIMEOUT 600)
endif()

# Dependency guard: the runner's include closure must stay render-free, its
# actual link artifacts must contain no graphical library, and a
# FT_VOX_GRAPHICS=OFF cache must show no graphics discovery (issue #210
# acceptance criterion 1).
add_test(NAME HeadlessDeps
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_headless_deps.py
        --build-dir ${CMAKE_BINARY_DIR})
set_tests_properties(HeadlessDeps PROPERTIES TIMEOUT 120)

# Clean-configure gate (issue #210 acceptance criterion 1): a SEPARATE fresh
# CMake configure + build with FT_VOX_GRAPHICS=OFF, no vcpkg toolchain and no
# graphics discovery, producing the runner + CPU tests and running them.
# Reuses the parent build's FetchContent sources when present (offline-safe).
# Opt-IN (issue #210 review): it adds a full nested configure/build (~150 s),
# so it is OFF by default to keep the everyday suite fast. The always-on
# HeadlessDeps guard is the default scripted dependency check; enable this
# heavier gate in CI or explicitly with -DFT_VOX_CPU_ONLY_BUILD_CHECK=ON
# (or run `ctest -L cpu-only` once enabled).
option(FT_VOX_CPU_ONLY_BUILD_CHECK "Register the clean CPU-only configure/build gate as a CTest (heavier; recommended for CI)" OFF)
if(FT_VOX_CPU_ONLY_BUILD_CHECK)
    add_test(NAME CpuOnlyBuild
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPARENT_BINARY_DIR=${CMAKE_BINARY_DIR}
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -DBUILD_CONFIG=$<CONFIG>
            -P ${CMAKE_SOURCE_DIR}/cmake/CheckCpuOnlyBuild.cmake)
    set_tests_properties(CpuOnlyBuild PROPERTIES TIMEOUT 1200 LABELS "cpu-only")
endif()

# ---------------------------------------------------------------------------
# Dependency-direction enforcement (issue #211, Epic #204 Phase G): every
# CPU-only test target must link the render stack nowhere in its transitive
# closure. The configure-time assertion (ft_vox_assert_cpu_link_closure, defined
# in the top-level CMakeLists) makes the boundary a repeatable build gate, not
# just a comment. Graphical test targets are intentionally NOT checked here.
#
# These CPU test targets are defined UNCONDITIONALLY above, so a listed name
# that does not exist is a stale-list error (e.g. after a rename), NOT a silent
# skip (review #220). test_network is the one exception: it is only defined when
# Boost is found, so it is checked conditionally.
# ---------------------------------------------------------------------------
foreach(_cpu_test IN ITEMS
    test_physics test_core_contracts test_voxel_world test_game_simulation
    test_gameplay test_player_sessions test_player_streaming test_actor_interactions test_items
    test_visual_look test_auto_exposure test_graphics_settings test_stream_opt
    test_terrain test_biome_map test_voxel_pool test_border_pool
    test_mesh_result_pool test_profiler test_workload_telemetry test_mobs
    test_frame_pacing test_world_save test_world_checkpoint bench_gameplay_items)
    if(NOT TARGET ${_cpu_test})
        message(FATAL_ERROR
            "CPU-only dependency guard lists '${_cpu_test}' but no such target exists "
            "(renamed/removed?). Update the guard list in tests/CMakeLists.txt — the "
            "boundary gate must not go quiet (issue #211).")
    endif()
    ft_vox_assert_cpu_link_closure(${_cpu_test})
endforeach()
# test_network is Boost-conditional (defined only when Boost::system exists).
if(TARGET test_network)
    ft_vox_assert_cpu_link_closure(test_network)
endif()

# ---------------------------------------------------------------------------
# Warning/sanitizer hygiene (review M21): every test executable defined in
# this directory gets the first-party warning flags + opt-in sanitizers.
# Iterating BUILDSYSTEM_TARGETS keeps future tests covered automatically;
# vendored sources compiled by a test (engine reload) are silenced per-file
# inside ft_vox_target_hygiene.
# ---------------------------------------------------------------------------
get_property(_ft_vox_test_targets DIRECTORY . PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _ft_vox_test_targets)
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "EXECUTABLE")
        ft_vox_target_hygiene(${_t})
    endif()
endforeach()

