cmake_minimum_required(VERSION 3.16)

project(ft_vox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: correct __cplusplus + avoid Windows min/max macros
if(MSVC)
    add_compile_options(/Zc:__cplusplus /permissive- /DNOMINMAX)
endif()

# Export compile_commands for IDEs/clangd (Unix generators)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependencies (system packages / vcpkg / FetchContent)
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)

# ---------------------------------------------------------------------------
# SPIR-V compile (+ auto-generated FrameUBO GLSL from C++ contract)
# ---------------------------------------------------------------------------
set(FT_VOX_SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/ressources/shaders/vulkan)
set(FT_VOX_SPV_OUT_DIR ${CMAKE_BINARY_DIR}/ressources/shaders/spv)
set(FT_VOX_GENERATED_SHADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)
file(MAKE_DIRECTORY ${FT_VOX_SPV_OUT_DIR})
file(MAKE_DIRECTORY ${FT_VOX_GENERATED_SHADER_DIR})
set(FT_VOX_SPV_FILES "")

set(FT_VOX_FRAME_UBO_HPP ${CMAKE_SOURCE_DIR}/src/Renderer/FrameUBO.hpp)
set(FT_VOX_FRAME_UBO_GLSL ${FT_VOX_GENERATED_SHADER_DIR}/frame_ubo.inc.glsl)
# Also mirror into source tree for tooling/IDEs that don't use the build include path
set(FT_VOX_FRAME_UBO_GLSL_SRC ${FT_VOX_SHADER_SRC_DIR}/frame_ubo.inc.glsl)

add_custom_command(
    OUTPUT ${FT_VOX_FRAME_UBO_GLSL} ${FT_VOX_FRAME_UBO_GLSL_SRC}
    COMMAND ${CMAKE_COMMAND}
        -DFT_VOX_FRAME_UBO_HPP=${FT_VOX_FRAME_UBO_HPP}
        -DFT_VOX_FRAME_UBO_GLSL_OUT=${FT_VOX_FRAME_UBO_GLSL}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateFrameUboGlsl.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FT_VOX_FRAME_UBO_GLSL} ${FT_VOX_FRAME_UBO_GLSL_SRC}
    DEPENDS ${FT_VOX_FRAME_UBO_HPP} ${CMAKE_SOURCE_DIR}/cmake/GenerateFrameUboGlsl.cmake
    COMMENT "Generate frame_ubo.inc.glsl from FrameUBO.hpp"
    VERBATIM
)
add_custom_target(ft_vox_frame_ubo_glsl DEPENDS ${FT_VOX_FRAME_UBO_GLSL})

function(ft_vox_compile_shader STAGE GLSL_NAME SPV_NAME)
    set(GLSL_PATH ${FT_VOX_SHADER_SRC_DIR}/${GLSL_NAME})
    set(SPV_PATH ${FT_VOX_SPV_OUT_DIR}/${SPV_NAME})
    if(GLSLC)
        add_custom_command(
            OUTPUT ${SPV_PATH}
            COMMAND ${GLSLC} -fshader-stage=${STAGE}
                -I${FT_VOX_GENERATED_SHADER_DIR}
                -I${FT_VOX_SHADER_SRC_DIR}
                ${GLSL_PATH} -o ${SPV_PATH}
            DEPENDS ${GLSL_PATH} ${FT_VOX_FRAME_UBO_GLSL}
            COMMENT "SPIR-V: ${GLSL_NAME} -> ${SPV_NAME}"
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT ${SPV_PATH}
            COMMAND ${GLSLANG_VALIDATOR} -V -S ${STAGE}
                -I${FT_VOX_GENERATED_SHADER_DIR}
                -I${FT_VOX_SHADER_SRC_DIR}
                ${GLSL_PATH} -o ${SPV_PATH}
            DEPENDS ${GLSL_PATH} ${FT_VOX_FRAME_UBO_GLSL}
            COMMENT "SPIR-V: ${GLSL_NAME} -> ${SPV_NAME}"
            VERBATIM
        )
    endif()
    list(APPEND FT_VOX_SPV_FILES ${SPV_PATH})
    set(FT_VOX_SPV_FILES ${FT_VOX_SPV_FILES} PARENT_SCOPE)
endfunction()

ft_vox_compile_shader(vert smoke.vert.glsl smoke.vert.spv)
ft_vox_compile_shader(frag smoke.frag.glsl smoke.frag.spv)
ft_vox_compile_shader(vert terrain.vert.glsl terrain.vert.spv)
ft_vox_compile_shader(frag terrain.frag.glsl terrain.frag.spv)
ft_vox_compile_shader(vert water.vert.glsl water.vert.spv)
ft_vox_compile_shader(frag water.frag.glsl water.frag.spv)
ft_vox_compile_shader(vert shadow.vert.glsl shadow.vert.spv)
ft_vox_compile_shader(frag shadow.frag.glsl shadow.frag.spv)
ft_vox_compile_shader(vert skybox.vert.glsl skybox.vert.spv)
ft_vox_compile_shader(frag skybox.frag.glsl skybox.frag.spv)
ft_vox_compile_shader(vert fullscreen.vert.glsl fullscreen.vert.spv)
ft_vox_compile_shader(frag bloomExtract.frag.glsl bloomExtract.frag.spv)
ft_vox_compile_shader(frag bloomBlur.frag.glsl bloomBlur.frag.spv)
ft_vox_compile_shader(frag godRays.frag.glsl godRays.frag.spv)
ft_vox_compile_shader(frag ssao.frag.glsl ssao.frag.spv)
ft_vox_compile_shader(frag composite.frag.glsl composite.frag.spv)
ft_vox_compile_shader(vert overlay.vert.glsl overlay.vert.spv)
ft_vox_compile_shader(frag overlay.frag.glsl overlay.frag.spv)
add_custom_target(ft_vox_shaders ALL DEPENDS ${FT_VOX_SPV_FILES} ft_vox_frame_ubo_glsl)
add_dependencies(ft_vox_shaders ft_vox_frame_ubo_glsl)

# ---------------------------------------------------------------------------
# Build-time git revision (regenerated every build)
# ---------------------------------------------------------------------------
set(FT_VOX_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(FT_VOX_BUILD_INFO_INC ${FT_VOX_GENERATED_DIR}/BuildInfo.inc)
file(MAKE_DIRECTORY ${FT_VOX_GENERATED_DIR})
# Seed so the first configure can still compile before the custom command runs.
if(NOT EXISTS ${FT_VOX_BUILD_INFO_INC})
    file(WRITE ${FT_VOX_BUILD_INFO_INC}
"// Placeholder until first build generates real values.
#pragma once
#define FT_VOX_GIT_HASH \"unknown\"
#define FT_VOX_GIT_BRANCH \"unknown\"
#define FT_VOX_GIT_DIRTY 0
#define FT_VOX_GIT_DESCRIBE \"unknown\"
#define FT_VOX_BUILD_UTC \"unknown\"
")
endif()
add_custom_target(ft_vox_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DFT_VOX_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DFT_VOX_BUILD_INFO_OUT=${FT_VOX_BUILD_INFO_INC}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake
    BYPRODUCTS ${FT_VOX_BUILD_INFO_INC}
    COMMENT "Generating BuildInfo.inc (git revision)"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(FT_VOX_VULKAN_SOURCES
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkContext.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkSwapchain.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkFrame.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkAllocator.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkImage.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkCommands.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkUpload.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkShader.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkResourceSmoke.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/StagingRing.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/GpuResourceRetire.cpp
)

set(FT_VOX_RENDERER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/WorldRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/PostStack.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/OverlayRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ShadowPass.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/OpaquePass.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/WaterPass.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/SkyPass.cpp
)

set(FT_VOX_IMGUI_SOURCES
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_impl_vulkan.cpp
    ${CMAKE_SOURCE_DIR}/src/ImGuiFileDialog/ImGuiFileDialog.cpp
)

set(FT_VOX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Engine.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/ImGuiLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUI.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUIResourcePack.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Benchmark.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/BuildInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${FT_VOX_RENDERER_SOURCES}
    ${FT_VOX_IMGUI_SOURCES}
    ${FT_VOX_VULKAN_SOURCES}
)

add_executable(${PROJECT_NAME} ${FT_VOX_SOURCES})
add_dependencies(${PROJECT_NAME} ft_vox_shaders ft_vox_build_info)
# Rebuild BuildInfo.cpp when the generated revision header changes.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/Engine/BuildInfo.cpp
    PROPERTIES OBJECT_DEPENDS ${FT_VOX_BUILD_INFO_INC}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/imgui
    ${CMAKE_SOURCE_DIR}/src/ImGuiFileDialog
    ${FT_VOX_GENERATED_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VK_USE_PLATFORM_METAL_EXT)
elseif(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX)
    # Wayland/X11 surface creation goes through SDL; no extra platform define required.
endif()

# Common link set used by main binary and Vulkan tests
set(FT_VOX_COMMON_LIBS
    SDL3::SDL3
    glm
    FastNoise2
    Boost::boost
    Boost::system
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${FT_VOX_COMMON_LIBS})

# Linux: libdl for dynamic loader helpers; Vulkan loader when not using pure volk bootstrap
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

# Release optimisations (multi-config safe; avoid -march=native on Apple for portability)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-O3>)
    if(NOT APPLE)
        target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/ressources $<TARGET_FILE_DIR:${PROJECT_NAME}>/ressources
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/ressources/shaders/spv
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FT_VOX_SPV_FILES}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/ressources/shaders/spv
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
