cmake_minimum_required(VERSION 3.20)
project(Maya VERSION 0.1.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE/tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build types
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(MAYA_BUILD_EXAMPLES "Build example programs" ON)
option(MAYA_BUILD_TESTS "Build tests" ON)
option(MAYA_USE_VULKAN "Use Vulkan renderer" ON)
option(MAYA_USE_OPENGL "Use OpenGL renderer (fallback)" ON)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Find dependencies
find_package(glfw3 REQUIRED)
# find_package(Vulkan REQUIRED)  # Will add in Phase 3

# Subdirectories
add_subdirectory(src)

if(MAYA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MAYA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
