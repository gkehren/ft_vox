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
if(APPLE)
    set_tests_properties(VulkanResourceSmoke PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"
    )
endif()
set_tests_properties(VulkanResourceSmoke PROPERTIES
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_vulkan_resources>
)

# ---------------------------------------------------------------------------
# Visual look: FrameUBO packing of colorBoost/sat + lively biome defaults
# ---------------------------------------------------------------------------
add_executable(test_visual_look
    test_visual_look.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
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
# Phase D stream CPU helpers (load priority, occupancy, cave Y bounds)
# ---------------------------------------------------------------------------
add_executable(test_stream_opt
    test_stream_opt.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
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
