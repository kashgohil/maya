cmake_minimum_required(VERSION 3.20)
project(Maya VERSION 0.1.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies
include(FetchContent)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.8
)

FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw catch2)

# Export compile commands for IDE/tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories
# Note: target_include_directories is used below instead of global include_directories

# Source files
file(GLOB_RECURSE ENGINE_SOURCES 
    "src/maya/*.cpp"
    "src/maya/*.mm"
)

add_library(MayaEngine STATIC ${ENGINE_SOURCES})

target_include_directories(MayaEngine PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)

# Platform specific libraries
if(APPLE)
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(QUARTZ_LIBRARY QuartzCore REQUIRED)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    target_link_libraries(MayaEngine PUBLIC 
        ${METAL_LIBRARY} 
        ${QUARTZ_LIBRARY} 
        ${COCOA_LIBRARY}
    )
endif()

target_link_libraries(MayaEngine PUBLIC glfw)

# Main executable
add_executable(maya src/main.cpp)
target_link_libraries(maya PRIVATE MayaEngine)

# Tests
add_executable(maya_tests tests/math_tests.cpp tests/rhi_tests.cpp tests/camera_tests.cpp)
target_link_libraries(maya_tests PRIVATE MayaEngine Catch2::Catch2WithMain)