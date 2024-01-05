cmake_minimum_required(VERSION 3.10)
project(ft_vox)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Detect the operating system
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    find_package(Freetype REQUIRED)
    include_directories(${FREETYPE_INCLUDE_DIRS})
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    find_package(Freetype REQUIRED)
    include_directories(/opt/homebrew/include/ /opt/homebrew/include/freetype2)
endif()

# Add your source files
file(GLOB_RECURSE SOURCES "src/*.cpp")
include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(ft_vox ${SOURCES} src/glad/glad.c)

target_link_libraries(ft_vox ${OPENGL_LIBRARIES} glfw ${FREETYPE_LIBRARIES})
