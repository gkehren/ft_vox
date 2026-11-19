# =============================================================================
# CheckCpuOnlyBuild.cmake — clean CPU-only configure/build gate (issue #210,
# Epic #204 Phase F, acceptance criterion 1). Run via `cmake -P`:
#
#   cmake -DSOURCE_DIR=<src> -DPARENT_BINARY_DIR=<build>
#         -DGENERATOR=<generator> -DBUILD_CONFIG=<Release|Debug|...>
#         [-DPLATFORM=<x64|...>] [-DTOOLSET=<...>]
#         -P cmake/CheckCpuOnlyBuild.cmake
#
# Performs a SEPARATE fresh CMake configure in
# <parent-build>/cpu-only-check with:
#   * FT_VOX_GRAPHICS=OFF  — SDL3/Vulkan/volk/VMA/miniz/shader compilers are
#     not discovered at all (no find_package call runs for them);
#   * NO vcpkg toolchain   — proves the CPU slice needs no package manager;
#   * FT_VOX_CPU_ONLY_BUILD_CHECK=OFF — no recursion into this gate.
#
# GLM/FastNoise2 come from FetchContent; when the parent build already
# downloaded them, their sources are reused via FETCHCONTENT_SOURCE_DIR_*
# overrides so the gate works offline. Then it builds EVERY target of the
# CPU-only configuration (core + headless runner + server + CPU tests) and runs
# the nested CTest suite. Registered as CTest `CpuOnlyBuild` (label cpu-only).
# =============================================================================

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PARENT_BINARY_DIR)
    message(FATAL_ERROR "CheckCpuOnlyBuild: SOURCE_DIR and PARENT_BINARY_DIR are required")
endif()
if(NOT DEFINED BUILD_CONFIG OR BUILD_CONFIG STREQUAL "")
    set(BUILD_CONFIG "Release")
endif()

set(CHECK_DIR "${PARENT_BINARY_DIR}/cpu-only-check")
file(REMOVE_RECURSE "${CHECK_DIR}")
file(MAKE_DIRECTORY "${CHECK_DIR}")

set(CONFIGURE_ARGS
    -S "${SOURCE_DIR}"
    -B "${CHECK_DIR}"
    -DFT_VOX_GRAPHICS=OFF
    -DFT_VOX_CPU_ONLY_BUILD_CHECK=OFF
    -DFT_VOX_DEP_MODE=system
    -DCMAKE_BUILD_TYPE=${BUILD_CONFIG}
)
if(DEFINED GENERATOR AND NOT GENERATOR STREQUAL "")
    list(APPEND CONFIGURE_ARGS -G "${GENERATOR}")
endif()
if(DEFINED PLATFORM AND NOT PLATFORM STREQUAL "")
    list(APPEND CONFIGURE_ARGS -A "${PLATFORM}")
endif()
if(DEFINED TOOLSET AND NOT TOOLSET STREQUAL "")
    list(APPEND CONFIGURE_ARGS -T "${TOOLSET}")
endif()

# Offline-safe FetchContent reuse: point at the parent build's downloaded
# sources when they exist (names upper-cased per FetchContent convention).
foreach(dep glm fastnoise2)
    string(TOUPPER "${dep}" DEP_UPPER)
    set(dep_src "${PARENT_BINARY_DIR}/_deps/${dep}-src")
    if(EXISTS "${dep_src}/CMakeLists.txt")
        list(APPEND CONFIGURE_ARGS "-DFETCHCONTENT_SOURCE_DIR_${DEP_UPPER}=${dep_src}")
    endif()
endforeach()

message(STATUS "CpuOnlyBuild: configuring ${CHECK_DIR} (FT_VOX_GRAPHICS=OFF, no toolchain)")
execute_process(
    COMMAND ${CMAKE_COMMAND} ${CONFIGURE_ARGS}
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "CpuOnlyBuild: CPU-only configure FAILED (exit ${configure_result})")
endif()

# Cache-level proof: no graphics discovery entries may exist.
file(STRINGS "${CHECK_DIR}/CMakeCache.txt" CACHE_LINES)
foreach(line IN LISTS CACHE_LINES)
    if(line MATCHES "^(Vulkan_|SDL3_|SDL2_|volk_|VulkanMemoryAllocator_|miniz_|GLSLC|GLSLANG_VALIDATOR|FT_VOX_VMA_INCLUDE_DIR)")
        message(FATAL_ERROR "CpuOnlyBuild: graphics discovery leaked into the CPU-only cache: ${line}")
    endif()
endforeach()

message(STATUS "CpuOnlyBuild: building all CPU-only targets")
execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${CHECK_DIR}" --config ${BUILD_CONFIG} --parallel
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "CpuOnlyBuild: CPU-only build FAILED (exit ${build_result})")
endif()

message(STATUS "CpuOnlyBuild: running the nested CPU-only CTest suite")
execute_process(
    COMMAND ${CMAKE_CTEST_COMMAND} -C ${BUILD_CONFIG} --output-on-failure --timeout 300
    WORKING_DIRECTORY "${CHECK_DIR}"
    RESULT_VARIABLE test_result
)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "CpuOnlyBuild: CPU-only tests FAILED (exit ${test_result})")
endif()

message(STATUS "CpuOnlyBuild: PASS — clean configure/build/tests without any graphics dependency")
