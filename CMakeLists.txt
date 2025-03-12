cmake_minimum_required(VERSION 3.16)

project(ft_vox)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# add source files
file(GLOB_RECURSE SOURCES src/*.cpp src/*.c)

# add include directories
include_directories(src/)

find_package(OpenGL REQUIRED)
find_package(SDL3 REQUIRED)
find_package(Boost REQUIRED)

# FetchContent for GLM
include(FetchContent)
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG bf71a834948186f4097caa076cd2663c69a10e1e
)
FetchContent_MakeAvailable(glm)

# FetchContent for FreeType2
FetchContent_Declare(
  freetype2
  GIT_REPOSITORY https://github.com/aseprite/freetype2
  GIT_TAG VER-2-10-0
)
FetchContent_MakeAvailable(freetype2)


#set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2 /DNDEBUG /GL")
#set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /Ob2 /DNDEBUG /GL")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Ofast -march=native")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_RELEASE} -O3 -Ofast -march=native")

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${SDL3_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} OpenGL::GL glm freetype ${Boost_LIBRARIES} ${SDL3_LIBRARIES})

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/ressources $<TARGET_FILE_DIR:${PROJECT_NAME}>/ressources
)