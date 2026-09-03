# =============================================================================
# ft_vox dependency resolution
#
# Strategy:
#   - Windows: prefer vcpkg CONFIG packages (toolchain almost always required)
#   - Linux:   prefer distro packages (apt/dnf); FetchContent for volk / VMA
#   - macOS:   Homebrew Vulkan stack + system/vcpkg SDL3/Boost, or full vcpkg
#
# When CMAKE_TOOLCHAIN_FILE points at vcpkg, CONFIG packages resolve as usual.
# Without a toolchain, we probe system/pkg-config and fall back to FetchContent
# for small deps that are rarely packaged correctly (zeux/volk, VMA).
# =============================================================================

include(FetchContent)
include(CMakeFindDependencyMacro OPTIONAL)

# ---------------------------------------------------------------------------
# Detect dependency mode
# ---------------------------------------------------------------------------
set(_FT_VOX_USING_VCPKG FALSE)
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
    set(_FT_VOX_USING_VCPKG TRUE)
endif()
if(DEFINED VCPKG_TARGET_TRIPLET OR DEFINED VCPKG_TOOLCHAIN)
    set(_FT_VOX_USING_VCPKG TRUE)
endif()

# FT_VOX_DEP_MODE: auto | system | vcpkg
# auto = vcpkg on Windows when toolchain present; system-first elsewhere
set(FT_VOX_DEP_MODE "auto" CACHE STRING "Dependency mode: auto, system, or vcpkg")
set_property(CACHE FT_VOX_DEP_MODE PROPERTY STRINGS auto system vcpkg)

if(FT_VOX_DEP_MODE STREQUAL "auto")
    if(WIN32 OR _FT_VOX_USING_VCPKG)
        set(_FT_VOX_MODE "vcpkg")
    else()
        set(_FT_VOX_MODE "system")
    endif()
else()
    set(_FT_VOX_MODE "${FT_VOX_DEP_MODE}")
endif()

message(STATUS "ft_vox dependency mode: ${_FT_VOX_MODE} (vcpkg toolchain=${_FT_VOX_USING_VCPKG})")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function(ft_vox_alias_if_missing alias real)
    if(TARGET ${real} AND NOT TARGET ${alias})
        add_library(${alias} ALIAS ${real})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Threads (Boost.Asio, etc.)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# SDL3
# ---------------------------------------------------------------------------
set(_FT_VOX_SDL3_OK FALSE)

if(_FT_VOX_MODE STREQUAL "vcpkg" OR _FT_VOX_USING_VCPKG)
    find_package(SDL3 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(_FT_VOX_SDL3_OK TRUE)
        message(STATUS "SDL3: vcpkg/CONFIG target SDL3::SDL3")
    endif()
endif()

if(NOT _FT_VOX_SDL3_OK)
    find_package(SDL3 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(_FT_VOX_SDL3_OK TRUE)
        message(STATUS "SDL3: CONFIG package")
    endif()
endif()

if(NOT _FT_VOX_SDL3_OK)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FT_VOX_SDL3 QUIET IMPORTED_TARGET sdl3)
        if(TARGET PkgConfig::FT_VOX_SDL3)
            if(NOT TARGET SDL3::SDL3)
                add_library(SDL3::SDL3 INTERFACE IMPORTED)
                target_link_libraries(SDL3::SDL3 INTERFACE PkgConfig::FT_VOX_SDL3)
            endif()
            set(_FT_VOX_SDL3_OK TRUE)
            message(STATUS "SDL3: pkg-config (sdl3)")
        endif()
    endif()
endif()

if(NOT _FT_VOX_SDL3_OK)
    # Optional last resort: build SDL3 from source (slow, needs network)
    option(FT_VOX_FETCH_SDL3 "Fetch and build SDL3 when not found on system" OFF)
    if(FT_VOX_FETCH_SDL3)
        message(STATUS "SDL3: FetchContent (FT_VOX_FETCH_SDL3=ON)")
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.10
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(SDL3)
        if(TARGET SDL3::SDL3)
            set(_FT_VOX_SDL3_OK TRUE)
        endif()
    endif()
endif()

if(NOT _FT_VOX_SDL3_OK)
    message(FATAL_ERROR
        "SDL3 not found.\n"
        "  Linux:   sudo apt install libsdl3-dev   OR   sudo dnf install SDL3-devel\n"
        "  macOS:   brew install sdl3   OR use vcpkg (see Makefile USE_VCPKG=1)\n"
        "  Windows: use vcpkg with vcpkg.json (sdl3[vulkan])\n"
        "  Optional: cmake -DFT_VOX_FETCH_SDL3=ON to build SDL3 from source")
endif()

# ---------------------------------------------------------------------------
# Boost (system for Asio)
# Prefer CONFIG (vcpkg / modern Boost); fall back to Module for older distros.
# ---------------------------------------------------------------------------
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW) # Don't use removed FindBoost module when possible
endif()
find_package(Boost CONFIG QUIET COMPONENTS system)
if(NOT Boost_FOUND)
    # Older CMake/distros still ship Module mode
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 OLD)
    endif()
    find_package(Boost REQUIRED COMPONENTS system)
endif()

# Normalize targets across Boost CMake configs / FindBoost
if(TARGET Boost::system)
    # ok
elseif(Boost_SYSTEM_LIBRARY)
    add_library(Boost::system UNKNOWN IMPORTED)
    set_target_properties(Boost::system PROPERTIES
        IMPORTED_LOCATION "${Boost_SYSTEM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
endif()

if(NOT TARGET Boost::boost AND NOT TARGET Boost::headers)
    if(Boost_INCLUDE_DIRS)
        add_library(Boost::headers INTERFACE IMPORTED)
        set_target_properties(Boost::headers PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
    endif()
endif()

if(TARGET Boost::headers AND NOT TARGET Boost::boost)
    add_library(Boost::boost ALIAS Boost::headers)
elseif(NOT TARGET Boost::boost AND Boost_INCLUDE_DIRS)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
endif()

message(STATUS "Boost: found (include=${Boost_INCLUDE_DIRS})")

# ---------------------------------------------------------------------------
# Vulkan (loader + headers via CMake FindVulkan / SDK)
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_LIBRARIES} (headers ${Vulkan_INCLUDE_DIRS})")

# ---------------------------------------------------------------------------
# volk — prefer CONFIG (vcpkg); else FetchContent (not the GNU Radio "volk")
# ---------------------------------------------------------------------------
set(_FT_VOX_VOLK_OK FALSE)

find_package(volk CONFIG QUIET)
if(TARGET volk::volk)
    set(_FT_VOX_VOLK_OK TRUE)
    message(STATUS "volk: CONFIG target volk::volk")
endif()

if(NOT _FT_VOX_VOLK_OK)
    # System install of zeux/volk is rare; do not confuse with GNU Radio volk.
    message(STATUS "volk: FetchContent (zeux/volk)")
    set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
    set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG 1.3.295
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(volk)
    ft_vox_alias_if_missing(volk::volk volk)
    if(TARGET volk::volk OR TARGET volk)
        set(_FT_VOX_VOLK_OK TRUE)
    endif()
endif()

if(NOT _FT_VOX_VOLK_OK)
    message(FATAL_ERROR "volk not available (CONFIG package or FetchContent failed)")
endif()

# Ensure we can always link volk::volk
if(NOT TARGET volk::volk AND TARGET volk)
    add_library(volk::volk ALIAS volk)
endif()

# ---------------------------------------------------------------------------
# Vulkan Memory Allocator (header-only)
# ---------------------------------------------------------------------------
set(_FT_VOX_VMA_OK FALSE)

find_package(VulkanMemoryAllocator CONFIG QUIET)
if(TARGET GPUOpen::VulkanMemoryAllocator)
    set(_FT_VOX_VMA_OK TRUE)
    message(STATUS "VMA: CONFIG target GPUOpen::VulkanMemoryAllocator")
endif()

if(NOT _FT_VOX_VMA_OK)
    # Some distros ship a plain header under /usr/include
    find_path(FT_VOX_VMA_INCLUDE_DIR
        NAMES vk_mem_alloc.h
        PATH_SUFFIXES vma VulkanMemoryAllocator
    )
    if(FT_VOX_VMA_INCLUDE_DIR)
        if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
            add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
            set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${FT_VOX_VMA_INCLUDE_DIR}")
        endif()
        set(_FT_VOX_VMA_OK TRUE)
        message(STATUS "VMA: system header at ${FT_VOX_VMA_INCLUDE_DIR}")
    endif()
endif()

if(NOT _FT_VOX_VMA_OK)
    message(STATUS "VMA: FetchContent (GPUOpen VulkanMemoryAllocator)")
    FetchContent_Declare(
        VulkanMemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.3.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(VulkanMemoryAllocator)
    if(TARGET VulkanMemoryAllocator AND NOT TARGET GPUOpen::VulkanMemoryAllocator)
        add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    endif()
    if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
        # Older/layout fallback: header-only include from source tree
        FetchContent_GetProperties(VulkanMemoryAllocator SOURCE_DIR _ft_vox_vma_src)
        if(NOT _ft_vox_vma_src)
            set(_ft_vox_vma_src "${vulkanmemoryallocator_SOURCE_DIR}")
        endif()
        add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
        set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_ft_vox_vma_src}/include")
    endif()
    set(_FT_VOX_VMA_OK TRUE)
endif()

# ---------------------------------------------------------------------------
# GLM + FastNoise2 (always FetchContent — not reliable as system packages)
# ---------------------------------------------------------------------------
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "Build GLM library" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    FastNoise2
    GIT_REPOSITORY https://github.com/Auburn/FastNoise2.git
    GIT_TAG v0.10.0-alpha
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(FastNoise2)

# ---------------------------------------------------------------------------
# miniz (ZIP archive reader)
# ---------------------------------------------------------------------------
set(_FT_VOX_MINIZ_OK FALSE)
find_package(miniz CONFIG QUIET)
if(TARGET miniz::miniz OR TARGET miniz)
    set(_FT_VOX_MINIZ_OK TRUE)
    message(STATUS "miniz: CONFIG package")
endif()

if(NOT _FT_VOX_MINIZ_OK)
    message(STATUS "miniz: FetchContent (richgel999/miniz)")
    FetchContent_Declare(
        miniz
        GIT_REPOSITORY https://github.com/richgel999/miniz.git
        GIT_TAG 3.0.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(miniz)
    if(TARGET miniz OR TARGET miniz::miniz)
        set(_FT_VOX_MINIZ_OK TRUE)
    endif()
endif()

if(TARGET miniz AND NOT TARGET miniz::miniz)
    add_library(miniz::miniz ALIAS miniz)
endif()

# ---------------------------------------------------------------------------
# SPIR-V compilers
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR glslangValidator
    HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
    PATHS
        "$ENV{VULKAN_SDK}/bin"
        /usr/bin
        /opt/homebrew/bin
)
find_program(GLSLC glslc
    HINTS ${Vulkan_GLSLC_EXECUTABLE}
    PATHS
        "$ENV{VULKAN_SDK}/bin"
        /usr/bin
        /opt/homebrew/bin
)

if(NOT GLSLC AND NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
        "Neither glslc nor glslangValidator found.\n"
        "  Linux:   sudo apt install glslang-tools   OR   sudo dnf install glslang\n"
        "  macOS:   brew install glslang   OR shaderc\n"
        "  Windows: vcpkg glslang / LunarG Vulkan SDK")
endif()

if(GLSLC)
    message(STATUS "SPIR-V compiler: glslc (${GLSLC})")
else()
    message(STATUS "SPIR-V compiler: glslangValidator (${GLSLANG_VALIDATOR})")
endif()
