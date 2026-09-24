cmake_minimum_required(VERSION 3.20)
project(Maya VERSION 0.1.0 LANGUAGES CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "Maya currently requires macOS and the Metal backend.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MAYA_BUILD_EDITOR "Build the editor application" ON)
option(MAYA_BUILD_PLAYER "Build the standalone player with the basic-scene project" ON)
option(MAYA_BUILD_SAMPLES "Build sample executables" ON)
option(MAYA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(MAYA_SANITIZERS "address,undefined" CACHE STRING "Comma-separated Clang sanitizers when enabled")
include(CTest)
include(FetchContent)

function(maya_target_options target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(MAYA_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE "-fsanitize=${MAYA_SANITIZERS}"
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC "-fsanitize=${MAYA_SANITIZERS}")
    endif()
endfunction()

find_library(METAL_LIBRARY Metal REQUIRED)
find_library(QUARTZ_LIBRARY QuartzCore REQUIRED)
find_library(COCOA_LIBRARY Cocoa REQUIRED)

# Explicit source lists prevent editor/sample code from silently entering the runtime.
add_library(MayaWorld STATIC
    src/maya/core/identity.cpp
    src/maya/world/world.cpp
    src/maya/world/spatial.cpp
    src/maya/properties/schema.cpp)
add_library(Maya::World ALIAS MayaWorld)
target_include_directories(MayaWorld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
maya_target_options(MayaWorld)

add_library(MayaAssets STATIC
    src/maya/assets/registry.cpp
    src/maya/assets/property_context.cpp
    src/maya/assets/file_provider.cpp
    src/maya/core/model_loader.cpp)
add_library(Maya::Assets ALIAS MayaAssets)
target_link_libraries(MayaAssets PUBLIC MayaWorld)
maya_target_options(MayaAssets)

# Scene persistence depends only on World data and a caller-supplied asset-reference resolver.
add_library(MayaScene STATIC
    src/maya/scene/scene_document.cpp
    src/maya/scene/scene_codec.cpp)
add_library(Maya::Scene ALIAS MayaScene)
target_link_libraries(MayaScene PUBLIC MayaWorld)
maya_target_options(MayaScene)

add_library(MayaRuntime STATIC
    src/maya/core/engine.cpp
    src/maya/core/camera.cpp
    src/maya/core/file_system.cpp
    src/maya/core/model_loader_legacy.cpp
    src/maya/core/primitives.cpp
    src/maya/core/scene.cpp
    src/maya/rhi/metal/metal_device.mm
)
add_library(Maya::Runtime ALIAS MayaRuntime)
target_include_directories(MayaRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(MayaRuntime PUBLIC MayaWorld MayaAssets MayaScene PRIVATE ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${COCOA_LIBRARY})
target_compile_options(MayaRuntime PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
maya_target_options(MayaRuntime)

if(MAYA_BUILD_EDITOR OR MAYA_BUILD_PLAYER OR MAYA_BUILD_SAMPLES)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)

    find_program(XXD_EXECUTABLE xxd REQUIRED)
    set(MAYA_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/maya.png")
    set(MAYA_ICON_EMBED "${CMAKE_CURRENT_BINARY_DIR}/generated/maya_icon_png.cpp")
    add_custom_command(
        OUTPUT "${MAYA_ICON_EMBED}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${XXD_EXECUTABLE}" -n maya_icon_png -i "${MAYA_ICON_PNG}" "${MAYA_ICON_EMBED}"
        DEPENDS "${MAYA_ICON_PNG}"
        COMMENT "Embedding maya.png as the application icon"
        VERBATIM)

    add_library(MayaDesktop STATIC
        src/maya/platform/window.cpp
        src/maya/platform/desktop_application.cpp
        src/maya/platform/app_icon.mm
        "${MAYA_ICON_EMBED}")
    target_link_libraries(MayaDesktop PUBLIC MayaRuntime PRIVATE glfw ${COCOA_LIBRARY})
    target_compile_options(MayaDesktop PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    maya_target_options(MayaDesktop)
endif()

if(MAYA_BUILD_PLAYER OR MAYA_BUILD_SAMPLES)
    add_library(MayaBasicScene STATIC samples/basic_scene/basic_scene.cpp)
    target_include_directories(MayaBasicScene PUBLIC samples/basic_scene)
    target_link_libraries(MayaBasicScene PUBLIC MayaRuntime)
    maya_target_options(MayaBasicScene)
endif()

if(MAYA_BUILD_PLAYER)
    add_executable(maya_player apps/player/main.cpp)
    target_link_libraries(maya_player PRIVATE MayaDesktop MayaBasicScene)
    maya_target_options(maya_player)
endif()

if(MAYA_BUILD_EDITOR)
    add_library(MayaEditor STATIC apps/editor/editor_application.cpp)
    target_include_directories(MayaEditor PUBLIC apps/editor)
    target_link_libraries(MayaEditor PUBLIC MayaRuntime)
    maya_target_options(MayaEditor)

    add_executable(maya_editor apps/editor/main.cpp)
    target_link_libraries(maya_editor PRIVATE MayaDesktop MayaEditor)
    maya_target_options(maya_editor)
endif()

if(MAYA_BUILD_SAMPLES)
    add_executable(maya_sample samples/basic_scene/main.cpp)
    target_link_libraries(maya_sample PRIVATE MayaDesktop MayaBasicScene)
    maya_target_options(maya_sample)
endif()

if(BUILD_TESTING)
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.4.0)
    FetchContent_MakeAvailable(catch2)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/world_tests.cpp")
        add_executable(maya_world_tests tests/world_tests.cpp tests/spatial_tests.cpp)
        target_link_libraries(maya_world_tests PRIVATE MayaWorld Catch2::Catch2WithMain)
        maya_target_options(maya_world_tests)
        add_test(NAME maya_world COMMAND maya_world_tests)
        set_tests_properties(maya_world PROPERTIES LABELS "cpu;world" TIMEOUT 60)
    endif()

    add_executable(maya_property_tests tests/property_tests.cpp)
    target_link_libraries(maya_property_tests PRIVATE MayaAssets Catch2::Catch2WithMain)
    maya_target_options(maya_property_tests)
    add_test(NAME maya_properties COMMAND maya_property_tests)
    set_tests_properties(maya_properties PROPERTIES LABELS "cpu;properties" TIMEOUT 60)

    add_executable(maya_asset_tests tests/asset_tests.cpp)
    target_link_libraries(maya_asset_tests PRIVATE MayaAssets Catch2::Catch2WithMain)
    maya_target_options(maya_asset_tests)
    add_test(NAME maya_assets COMMAND maya_asset_tests)
    set_tests_properties(maya_assets PROPERTIES LABELS "cpu;assets" TIMEOUT 60)

    add_executable(maya_tests
        tests/math_tests.cpp
        tests/rhi_tests.cpp
        tests/camera_tests.cpp
        tests/vector_tests.cpp
        tests/matrix_tests.cpp
        tests/quaternion_tests.cpp
        tests/model_loader_tests.cpp
        tests/input_tests.cpp
        tests/vertex_tests.cpp
        tests/camera_extended_tests.cpp
        tests/math_utils_tests.cpp
        tests/mesh_tests.cpp
        tests/texture_tests.cpp
        tests/engine_tests.cpp)
    target_link_libraries(maya_tests PRIVATE MayaRuntime Catch2::Catch2WithMain)
    maya_target_options(maya_tests)

    add_test(NAME maya_cpu COMMAND maya_tests "~[rhi]")
    set_tests_properties(maya_cpu PROPERTIES LABELS "cpu" TIMEOUT 60)
    add_test(NAME maya_rhi COMMAND maya_tests "[rhi]")
    set_tests_properties(maya_rhi PROPERTIES LABELS "gpu" TIMEOUT 60)

    if(TARGET MayaDesktop)
        add_executable(maya_desktop_tests tests/desktop_lifecycle_tests.cpp)
        target_link_libraries(maya_desktop_tests PRIVATE MayaDesktop Catch2::Catch2WithMain)
        maya_target_options(maya_desktop_tests)
        add_test(NAME maya_desktop_lifecycle COMMAND maya_desktop_tests)
        set_tests_properties(maya_desktop_lifecycle PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE)
    endif()

    foreach(app maya_player maya_editor maya_sample)
        if(TARGET ${app})
            add_test(NAME ${app}_help COMMAND ${app} --help)
            set_tests_properties(${app}_help PROPERTIES LABELS "cpu" TIMEOUT 15)
            add_test(NAME ${app}_reject_zero_frames COMMAND ${app} --smoke 0)
            set_tests_properties(${app}_reject_zero_frames PROPERTIES LABELS "cpu" WILL_FAIL TRUE TIMEOUT 15)
            add_test(NAME ${app}_smoke COMMAND ${app} --smoke 5)
            set_tests_properties(${app}_smoke PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE)
        endif()
    endforeach()
endif()
