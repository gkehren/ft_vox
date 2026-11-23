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
# First-party warning/sanitizer hygiene (review M21): 52k lines used to build
# with ZERO warning flags. ft_vox_warnings is linked to every first-party
# target (core, headless, game, tests); vendored sources (ImGui/FileDialog)
# are silenced per-file so their noise never masks our own diagnostics.
# Warnings-as-errors and sanitizers are OPT-IN so existing builds keep their
# behavior; CI enables them explicitly.
# ---------------------------------------------------------------------------
option(FT_VOX_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)
option(FT_VOX_SANITIZERS "ASan + UBSan on first-party targets (GCC/Clang/MSVC)" OFF)
option(FT_VOX_TSAN "TSan on first-party targets (GCC/Clang only)" OFF)
option(FT_VOX_LTO "Link-time optimization for Release configurations" OFF)

if(FT_VOX_SANITIZERS AND FT_VOX_TSAN)
    message(FATAL_ERROR "FT_VOX_SANITIZERS (ASan) and FT_VOX_TSAN are mutually exclusive")
endif()

add_library(ft_vox_warnings INTERFACE)
target_compile_options(ft_vox_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)
# MSVC's secure-CRT deprecation nag (getenv etc.) is noise for this
# codebase's deliberate POSIX-style usage; the real diagnostics stay on.
target_compile_definitions(ft_vox_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
if(FT_VOX_WARNINGS_AS_ERRORS)
    target_compile_options(ft_vox_warnings INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/WX>)
endif()

add_library(ft_vox_sanitizers INTERFACE)
if(FT_VOX_SANITIZERS)
    if(MSVC)
        target_compile_options(ft_vox_sanitizers INTERFACE /fsanitize=address)
    else()
        target_compile_options(ft_vox_sanitizers INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(ft_vox_sanitizers INTERFACE -fsanitize=address,undefined)
    endif()
endif()
if(FT_VOX_TSAN)
    if(MSVC)
        message(FATAL_ERROR "FT_VOX_TSAN requires GCC or Clang")
    endif()
    target_compile_options(ft_vox_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(ft_vox_sanitizers INTERFACE -fsanitize=thread)
endif()

if(FT_VOX_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FT_VOX_IPO_OK OUTPUT FT_VOX_IPO_ERR)
    if(FT_VOX_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "FT_VOX_LTO requested but IPO is unsupported: ${FT_VOX_IPO_ERR}")
    endif()
endif()

# Apply the hygiene pair to a first-party target and silence the vendored
# translation units it may compile (source-file properties are directory
# scoped, so this runs in the CALLER's directory — root or tests/).
function(ft_vox_target_hygiene target)
    target_link_libraries(${target} PRIVATE ft_vox_warnings ft_vox_sanitizers)
    if(FT_VOX_IMGUI_SOURCES)
        set_source_files_properties(${FT_VOX_IMGUI_SOURCES} PROPERTIES
            COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/W0>;$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Build scope (issue #210, Epic #204 Phase F)
#
# FT_VOX_GRAPHICS=ON  (default): the full game + renderer + every test.
# FT_VOX_GRAPHICS=OFF: CPU-only configuration — ft_vox_core, the
#   ft_vox_headless, ft_vox_server and the CPU-only tests. The graphical
#   dependency stack (SDL3, Vulkan, volk, VMA, miniz, SPIR-V compilers) is
#   NOT discovered at all, so this configures on a machine with no Vulkan
#   SDK / SDL3 / shader toolchain. See docs/headless-runner.md.
# ---------------------------------------------------------------------------
option(FT_VOX_GRAPHICS "Build the graphical game (SDL3/Vulkan/ImGui); OFF = CPU-only core and hosts" ON)

# Canonical floating-point codegen for the CPU simulation (issue #210):
# OFF keeps the historical game codegen (-march=native on non-Apple, FMA
# contraction allowed) — digests are reproducible per binary. ON compiles
# ft_vox_core + ft_vox_headless without -march=native and with contraction
# disabled, so authoritative-state digests compare across machines of the
# same platform family (docs/headless-runner.md, "Determinism").
option(FT_VOX_PORTABLE_FP "Portable FP codegen for ft_vox_core/headless (no -march=native, no FMA contraction)" OFF)

# ---------------------------------------------------------------------------
# Dependencies (system packages / vcpkg / FetchContent)
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)

# ---------------------------------------------------------------------------
# SPIR-V compile (+ auto-generated FrameUBO GLSL from C++ contract)
# Graphical only: the CPU-only configuration has no shader targets/assets.
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
set(FT_VOX_SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/ressources/shaders/vulkan)
set(FT_VOX_SPV_OUT_DIR ${CMAKE_BINARY_DIR}/ressources/shaders/spv)
set(FT_VOX_GENERATED_SHADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)
file(MAKE_DIRECTORY ${FT_VOX_SPV_OUT_DIR})
file(MAKE_DIRECTORY ${FT_VOX_GENERATED_SHADER_DIR})
set(FT_VOX_SPV_FILES "")

set(FT_VOX_FRAME_UBO_HPP ${CMAKE_SOURCE_DIR}/src/Renderer/FrameUBO.hpp)
set(FT_VOX_FRAME_UBO_GLSL ${FT_VOX_GENERATED_SHADER_DIR}/frame_ubo.inc.glsl)
# Generated into the build tree only; shaders pick it up via the
# -I${FT_VOX_GENERATED_SHADER_DIR} include path below. The source tree is
# never written by a build.

add_custom_command(
    OUTPUT ${FT_VOX_FRAME_UBO_GLSL}
    COMMAND ${CMAKE_COMMAND}
        -DFT_VOX_FRAME_UBO_HPP=${FT_VOX_FRAME_UBO_HPP}
        -DFT_VOX_FRAME_UBO_GLSL_OUT=${FT_VOX_FRAME_UBO_GLSL}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateFrameUboGlsl.cmake
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
                ${FT_VOX_SHADER_SRC_DIR}/foliage_wind.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/colorspace.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/csm.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/sky_radiance.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/water_optics.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/cutout.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/atmosphere_fog.inc.glsl
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
                ${FT_VOX_SHADER_SRC_DIR}/foliage_wind.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/colorspace.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/csm.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/sky_radiance.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/water_optics.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/cutout.inc.glsl
                ${FT_VOX_SHADER_SRC_DIR}/atmosphere_fog.inc.glsl
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
ft_vox_compile_shader(frag ssaoUpsample.frag.glsl ssaoUpsample.frag.spv)
ft_vox_compile_shader(frag composite.frag.glsl composite.frag.spv)
ft_vox_compile_shader(frag fxaa.frag.glsl fxaa.frag.spv)
ft_vox_compile_shader(frag luminance_downsample.frag.glsl luminance_downsample.frag.spv)
ft_vox_compile_shader(frag exposure_adapt.frag.glsl exposure_adapt.frag.spv)
ft_vox_compile_shader(vert mob.vert.glsl mob.vert.spv)
ft_vox_compile_shader(frag mob.frag.glsl mob.frag.spv)
ft_vox_compile_shader(frag mob_shadow.frag.glsl mob_shadow.frag.spv)
ft_vox_compile_shader(vert item.vert.glsl item.vert.spv)
ft_vox_compile_shader(frag item.frag.glsl item.frag.spv)
ft_vox_compile_shader(vert overlay.vert.glsl overlay.vert.spv)
ft_vox_compile_shader(frag overlay.frag.glsl overlay.frag.spv)
add_custom_target(ft_vox_shaders ALL DEPENDS ${FT_VOX_SPV_FILES} ft_vox_frame_ubo_glsl)
add_dependencies(ft_vox_shaders ft_vox_frame_ubo_glsl)

# ---------------------------------------------------------------------------
# Build-time git revision (regenerated every build) — graphical targets only
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
endif() # FT_VOX_GRAPHICS (shaders + build info)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# The Chunk implementation split (issue #181): these five translation units
# implement ONE class. Every target that compiles any of them must link all
# five (missing ones fail at link time). ChunkBorders.cpp is NOT part of this
# list: it is the standalone BorderPool implementation (see test_border_pool).
set(FT_VOX_CHUNK_SOURCES
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkEditing.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkLighting.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshing.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkGpu.cpp
)

set(FT_VOX_VULKAN_SOURCES
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkContext.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkSwapchain.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkFrame.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkGpuProfiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkAllocator.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkImage.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkCommands.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkUpload.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/VkShader.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/StagingRing.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/MeshArena.cpp
    ${CMAKE_SOURCE_DIR}/src/Vulkan/GpuResourceRetire.cpp
)

set(FT_VOX_RENDERER_SOURCES
    # MobSystem.cpp / MobModel.cpp ALSO appear in FT_VOX_CORE_SOURCES below.
    # That is intentional: ft_vox_visual_tests and test_post_resize compile
    # this list directly (they do not link ft_vox_core), so the mob units must
    # stay here. The GAME subtracts the core units via list(REMOVE_ITEM) and
    # links ft_vox_core instead. Do not "de-duplicate" by deleting them here.
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/MobTextures.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/MobRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ItemEntityRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/ResourcePackReader.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureMips.cpp
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

set(FT_VOX_WORLD_SOURCES
    ${CMAKE_SOURCE_DIR}/src/World/VoxelWorldPlayerStreaming.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldChunk.cpp
    ${CMAKE_SOURCE_DIR}/src/World/VoxelWorld.cpp
)

# Item domain contracts (issue #224, Epic #223 Phase A): CPU-only module,
# part of ft_vox_core. Expanded into BOTH FT_VOX_SOURCES and
# FT_VOX_CORE_SOURCES (identical spellings) so the game subtracts these units
# below and links them from the core library instead of dual-compiling them.
set(FT_VOX_ITEM_SOURCES
    ${CMAKE_SOURCE_DIR}/src/Items/ItemRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemStack.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/Inventory.cpp
    ${CMAKE_SOURCE_DIR}/src/Items/ItemEntitySystem.cpp
)

set(FT_VOX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Engine.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/EngineMultiplayer.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/EngineJoin.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/ClientProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/MultiplayerJoin.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/ImGuiLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUI.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUIInventory.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUIMultiplayer.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUIResourcePack.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/UiTheme.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/UiShell.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUIBiomeMap.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugUiCore.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugUiState.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugOverview.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugPerformance.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugPlayer.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugRendering.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/GraphicsCategories.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugStreaming.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugChunkInspector.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugMemory.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DebugUI/DebugBenchmark.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Benchmark.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/BuildInfo.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${FT_VOX_CHUNK_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkMeshResult.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/RenderWorldBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Sim/GameSimulation.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
    ${FT_VOX_WORLD_SOURCES}
    ${FT_VOX_ITEM_SOURCES}
    ${FT_VOX_RENDERER_SOURCES}
    ${FT_VOX_IMGUI_SOURCES}
    ${FT_VOX_VULKAN_SOURCES}
)

# ---------------------------------------------------------------------------
# ft_vox_core — CPU-only simulation/persistence static library (issue #205,
# Epic #204 Phase A; issue #206 Phase B adds the authoritative world; issue
# #208 Phase D adds the GameSimulation facade owning player + mobs + world;
# issue #224 / Epic #223 Phase A adds the item domain contracts).
# Render-independent: links ONLY glm + FastNoise2 + Threads, never SDL3 /
# Vulkan / ImGui. Phase A extracted the six already-independent translation
# units; Phase B adds the authoritative voxel world (WorldChunk + VoxelWorld)
# and the CPU backing pools they own (VoxelPool, BorderPool). See
# docs/simulation-render-contracts.md.
#
# The public headers of these units (and Core/SimContracts.hpp) are guarded
# against forbidden include chains by tests/check_core_includes.py.
# ---------------------------------------------------------------------------
set(FT_VOX_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/World/WorldSave.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldCheckpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/World/WorldPersistence.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainVegetation.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobSystem.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities/MobModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Sim/GameSimulation.cpp
    ${FT_VOX_WORLD_SOURCES}
    ${FT_VOX_ITEM_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Chunk/VoxelPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkBorders.cpp
)

add_library(ft_vox_core STATIC ${FT_VOX_CORE_SOURCES})
ft_vox_target_hygiene(ft_vox_core)
target_include_directories(ft_vox_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ft_vox_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
target_link_libraries(ft_vox_core PUBLIC glm FastNoise2 Threads::Threads)
# Same Release codegen as the GAME target (/O2 MSVC; -O3, -march=native on
# non-Apple elsewhere), so the extracted units compile exactly as they did in
# the monolithic game build. This is a same-target claim only: CPU test targets
# compile these sources with their own flags (no -march=native), so cross-target
# bit-identity is NOT guaranteed (FMA contraction can differ on Linux).
# Issue #210 (Phase F): FT_VOX_PORTABLE_FP=ON selects the canonical shared
# flag set for cross-machine digest comparison — no -march=native, FMA
# contraction disabled (GCC/Clang), precise FP (MSVC). The default stays
# byte-identical to the historical game codegen. See docs/headless-runner.md.
if(MSVC)
    target_compile_options(ft_vox_core PRIVATE $<$<CONFIG:Release>:/O2>)
    if(FT_VOX_PORTABLE_FP)
        target_compile_options(ft_vox_core PRIVATE /fp:precise)
    endif()
else()
    target_compile_options(ft_vox_core PRIVATE $<$<CONFIG:Release>:-O3>)
    if(FT_VOX_PORTABLE_FP)
        target_compile_options(ft_vox_core PRIVATE -ffp-contract=off)
    elseif(NOT APPLE)
        target_compile_options(ft_vox_core PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
endif()

# ---------------------------------------------------------------------------
# ft_vox_headless — standalone CPU simulation runner (issue #210, Epic #204
# Phase F). The graphical-free host of sim::GameSimulation: links ONLY
# ft_vox_core (transitively glm + FastNoise2 + Threads). Built in BOTH
# configurations. No SDL3/Vulkan/ImGui/shader/resource dependency is reachable
# (configure-time link-closure guard below + tests/check_headless_deps.py).
# ---------------------------------------------------------------------------
set(FT_VOX_HEADLESS_SOURCES
    ${CMAKE_SOURCE_DIR}/src/Headless/HeadlessMain.cpp
    ${CMAKE_SOURCE_DIR}/src/Headless/HeadlessRunner.cpp
    ${CMAKE_SOURCE_DIR}/src/Headless/SimScript.cpp
    ${CMAKE_SOURCE_DIR}/src/Headless/SimDigest.cpp
)

add_executable(ft_vox_headless ${FT_VOX_HEADLESS_SOURCES})
target_link_libraries(ft_vox_headless PRIVATE ft_vox_core)
ft_vox_target_hygiene(ft_vox_headless)
if(MSVC)
    target_compile_options(ft_vox_headless PRIVATE $<$<CONFIG:Release>:/O2>)
    if(FT_VOX_PORTABLE_FP)
        target_compile_options(ft_vox_headless PRIVATE /fp:precise)
    endif()
else()
    target_compile_options(ft_vox_headless PRIVATE $<$<CONFIG:Release>:-O3>)
    if(FT_VOX_PORTABLE_FP)
        target_compile_options(ft_vox_headless PRIVATE -ffp-contract=off)
    elseif(NOT APPLE)
        target_compile_options(ft_vox_headless PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
endif()

# Dedicated authority host (issue #250). The test-only UDP prototype and
# the graphical game remain independent of the server executable.
set(FT_VOX_SERVER_HOST_SOURCE
    ${CMAKE_SOURCE_DIR}/src/Server/ServerHost.cpp
    ${CMAKE_SOURCE_DIR}/src/Server/WorldIdentities.cpp
)
add_executable(ft_vox_server src/Server/ServerMain.cpp ${FT_VOX_SERVER_HOST_SOURCE})
target_link_libraries(ft_vox_server PRIVATE ft_vox_core)
ft_vox_target_hygiene(ft_vox_server)

# Authoritative wire codec (issue #251). Transport hosts opt in to this target.
add_library(ft_vox_wire STATIC src/Network/WireProtocol.cpp)
target_include_directories(ft_vox_wire PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ft_vox_wire PRIVATE ft_vox_core)
ft_vox_target_hygiene(ft_vox_wire)

add_library(ft_vox_udp STATIC
    src/Network/UdpSocket.cpp
    src/Network/ServerTransport.cpp
    src/Network/ClientMovement.cpp
    src/Network/ClientWorld.cpp
    src/Network/ClientTransport.cpp)
target_link_libraries(ft_vox_udp PUBLIC ft_vox_wire PRIVATE ft_vox_core Threads::Threads)
if(WIN32)
    target_link_libraries(ft_vox_udp PRIVATE ws2_32 bcrypt)
endif()
ft_vox_target_hygiene(ft_vox_udp)
target_link_libraries(ft_vox_server PRIVATE ft_vox_udp)

# Configure-time dependency-direction guard (issue #210 criterion 1, hardened
# in issue #211 / Epic #204 Phase G): walk the TRANSITIVE link closure of a
# CPU-only target and fail on any graphical library, in either configuration.
# Applied to ft_vox_core ITSELF (the simulation library must never link
# SDL3/Vulkan/volk/VMA/miniz/ImGui transitively) and to ft_vox_headless, plus
# the CPU-only test targets (tests/CMakeLists.txt). Generator-independent
# (declared targets, not link lines); tests/check_headless_deps.py additionally
# verifies the include closure and the actual build artifacts.
function(ft_vox_assert_cpu_link_closure target)
    set(_visited "")
    set(_pending "${target}")
    while(_pending)
        list(POP_FRONT _pending _current)
        if(_current IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_current}")
        if(_current MATCHES "(SDL3|SDL2|volk|VulkanMemoryAllocator|Vulkan::|miniz|imgui|glfw|glad)")
            message(FATAL_ERROR
                "CPU-only target '${target}' link closure contains graphical dependency "
                "'${_current}' (Epic #204: the CPU simulation must not link the render stack).")
        endif()
        if(TARGET "${_current}")
            get_target_property(_iface "${_current}" INTERFACE_LINK_LIBRARIES)
            if(_iface)
                list(APPEND _pending ${_iface})
            endif()
            get_target_property(_direct "${_current}" LINK_LIBRARIES)
            if(_direct)
                list(APPEND _pending ${_direct})
            endif()
        endif()
    endwhile()
endfunction()
ft_vox_assert_cpu_link_closure(ft_vox_core)
ft_vox_assert_cpu_link_closure(ft_vox_headless)
ft_vox_assert_cpu_link_closure(ft_vox_server)
ft_vox_assert_cpu_link_closure(ft_vox_wire)
ft_vox_assert_cpu_link_closure(ft_vox_udp)

# CPU-only configuration: the graphical stack must not even have been
# DISCOVERED (no targets created by Dependencies.cmake).
if(NOT FT_VOX_GRAPHICS)
    foreach(_forbidden IN ITEMS SDL3::SDL3 volk::volk GPUOpen::VulkanMemoryAllocator miniz::miniz Vulkan::Vulkan)
        if(TARGET ${_forbidden})
            message(FATAL_ERROR
                "FT_VOX_GRAPHICS=OFF but graphical target '${_forbidden}' exists — "
                "dependency discovery leaked into the CPU-only configuration (issue #210).")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# The graphical game (FT_VOX_GRAPHICS=ON only)
# ---------------------------------------------------------------------------
if(FT_VOX_GRAPHICS)
# The game consumes ft_vox_core instead of recompiling its sources. Existing
# tests keep compiling the sources directly (unchanged); because the game no
# longer compiles them there are no duplicate symbols.
set(FT_VOX_GAME_SOURCES ${FT_VOX_SOURCES})
# REMOVE_ITEM matches exact path spellings; a mismatch would silently leave a
# core unit in the game list and dual-compile it (duplicate symbols, LNK2005).
# Assert the full core list was actually removed.
list(LENGTH FT_VOX_GAME_SOURCES FT_VOX_GAME_COUNT_BEFORE)
list(REMOVE_ITEM FT_VOX_GAME_SOURCES ${FT_VOX_CORE_SOURCES})
list(LENGTH FT_VOX_GAME_SOURCES FT_VOX_GAME_COUNT_AFTER)
list(LENGTH FT_VOX_CORE_SOURCES FT_VOX_CORE_COUNT)
math(EXPR FT_VOX_GAME_COUNT_EXPECTED "${FT_VOX_GAME_COUNT_BEFORE} - ${FT_VOX_CORE_COUNT}")
if(NOT FT_VOX_GAME_COUNT_AFTER EQUAL FT_VOX_GAME_COUNT_EXPECTED)
    message(FATAL_ERROR
        "ft_vox_core extraction: expected ${FT_VOX_CORE_COUNT} sources removed from "
        "FT_VOX_SOURCES (${FT_VOX_GAME_COUNT_BEFORE} -> ${FT_VOX_GAME_COUNT_AFTER}, "
        "expected ${FT_VOX_GAME_COUNT_EXPECTED}). Path spellings in FT_VOX_CORE_SOURCES "
        "must match FT_VOX_SOURCES exactly; a partial removal dual-compiles core units.")
endif()

add_executable(${PROJECT_NAME} ${FT_VOX_GAME_SOURCES})
ft_vox_target_hygiene(${PROJECT_NAME})
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

# Common link set used by main binary and Vulkan tests.
# Boost is intentionally absent: only test_network uses it (see tests/).
set(FT_VOX_COMMON_LIBS
    SDL3::SDL3
    glm
    FastNoise2
    miniz::miniz
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

# The game objects reference ft_vox_core's symbols, so on order-sensitive
# linkers (GNU ld) the library must appear AFTER the objects — listing it first
# among the libraries achieves that. CMake appends its transitive
# glm/FastNoise2/Threads after it automatically.
target_link_libraries(${PROJECT_NAME} PRIVATE ft_vox_udp ft_vox_core ${FT_VOX_COMMON_LIBS})

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
endif() # FT_VOX_GRAPHICS (game target)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
