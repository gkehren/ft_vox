cmake_minimum_required(VERSION 3.16)

project(ft_vox)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(MSVC)
    add_compile_options(/Zc:__cplusplus)
    add_compile_options(/DNOMINMAX)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Vulkan REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

include(FetchContent)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "Build GLM library" FORCE)
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.0
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
  FastNoise2
  GIT_REPOSITORY https://github.com/Auburn/FastNoise2.git
  GIT_TAG v0.10.0-alpha
)
FetchContent_MakeAvailable(FastNoise2)

# ---------------------------------------------------------------------------
# SPIR-V compile
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR glslangValidator HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE})
find_program(GLSLC glslc HINTS ${Vulkan_GLSLC_EXECUTABLE})

set(FT_VOX_SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/ressources/shaders/vulkan)
set(FT_VOX_SPV_OUT_DIR ${CMAKE_BINARY_DIR}/ressources/shaders/spv)
file(MAKE_DIRECTORY ${FT_VOX_SPV_OUT_DIR})
set(FT_VOX_SPV_FILES "")

function(ft_vox_compile_shader STAGE GLSL_NAME SPV_NAME)
    set(GLSL_PATH ${FT_VOX_SHADER_SRC_DIR}/${GLSL_NAME})
    set(SPV_PATH ${FT_VOX_SPV_OUT_DIR}/${SPV_NAME})
    if(GLSLC)
        add_custom_command(
            OUTPUT ${SPV_PATH}
            COMMAND ${GLSLC} -fshader-stage=${STAGE} ${GLSL_PATH} -o ${SPV_PATH}
            DEPENDS ${GLSL_PATH}
            COMMENT "SPIR-V: ${GLSL_NAME} -> ${SPV_NAME}"
            VERBATIM
        )
    elseif(GLSLANG_VALIDATOR)
        add_custom_command(
            OUTPUT ${SPV_PATH}
            COMMAND ${GLSLANG_VALIDATOR} -V -S ${STAGE} ${GLSL_PATH} -o ${SPV_PATH}
            DEPENDS ${GLSL_PATH}
            COMMENT "SPIR-V: ${GLSL_NAME} -> ${SPV_NAME}"
            VERBATIM
        )
    else()
        message(FATAL_ERROR "Neither glslc nor glslangValidator found")
    endif()
    list(APPEND FT_VOX_SPV_FILES ${SPV_PATH})
    set(FT_VOX_SPV_FILES ${FT_VOX_SPV_FILES} PARENT_SCOPE)
endfunction()

ft_vox_compile_shader(vert smoke.vert.glsl smoke.vert.spv)
ft_vox_compile_shader(frag smoke.frag.glsl smoke.frag.spv)
ft_vox_compile_shader(vert terrain.vert.glsl terrain.vert.spv)
ft_vox_compile_shader(frag terrain.frag.glsl terrain.frag.spv)
ft_vox_compile_shader(vert shadow.vert.glsl shadow.vert.spv)
ft_vox_compile_shader(frag shadow.frag.glsl shadow.frag.spv)
ft_vox_compile_shader(vert skybox.vert.glsl skybox.vert.spv)
ft_vox_compile_shader(frag skybox.frag.glsl skybox.frag.spv)
ft_vox_compile_shader(vert fullscreen.vert.glsl fullscreen.vert.spv)
ft_vox_compile_shader(frag bloomExtract.frag.glsl bloomExtract.frag.spv)
ft_vox_compile_shader(frag bloomBlur.frag.glsl bloomBlur.frag.spv)
ft_vox_compile_shader(frag godRays.frag.glsl godRays.frag.spv)
ft_vox_compile_shader(frag composite.frag.glsl composite.frag.spv)
ft_vox_compile_shader(vert overlay.vert.glsl overlay.vert.spv)
ft_vox_compile_shader(frag overlay.frag.glsl overlay.frag.spv)
add_custom_target(ft_vox_shaders ALL DEPENDS ${FT_VOX_SPV_FILES})

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
)

set(FT_VOX_IMGUI_SOURCES
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/src/imgui/imgui_impl_vulkan.cpp
)

set(FT_VOX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/Engine.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/ImGuiLayer.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/GameUI.cpp
    ${CMAKE_SOURCE_DIR}/src/Camera/Camera.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkPool.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/ChunkManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Chunk/TerrainGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TextureManager.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/TerrainRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/PostStack.cpp
    ${CMAKE_SOURCE_DIR}/src/Renderer/OverlayRenderer.cpp
    ${FT_VOX_IMGUI_SOURCES}
    ${FT_VOX_VULKAN_SOURCES}
)

add_executable(${PROJECT_NAME} ${FT_VOX_SOURCES})
add_dependencies(${PROJECT_NAME} ft_vox_shaders)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/imgui
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

if(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VK_USE_PLATFORM_METAL_EXT)
endif()

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    SDL3::SDL3
    glm
    FastNoise2
    Boost::boost
    Boost::system
    volk::volk
    GPUOpen::VulkanMemoryAllocator
)

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework IOKit"
        "-framework IOSurface"
    )
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native>
)

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
