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

# CPU-side graphics API: handle tables, validation, deferred retirement, and a null backend.
add_library(MayaRHI STATIC
    src/maya/rhi/graphics_device.cpp
    src/maya/rhi/null_device.cpp)
add_library(Maya::RHI ALIAS MayaRHI)
target_include_directories(MayaRHI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
maya_target_options(MayaRHI)

add_library(MayaAssets STATIC
    src/maya/assets/registry.cpp
    src/maya/assets/property_context.cpp
    src/maya/assets/file_provider.cpp
    src/maya/assets/project.cpp
    src/maya/core/model_loader.cpp)
add_library(Maya::Assets ALIAS MayaAssets)
target_link_libraries(MayaAssets PUBLIC MayaWorld MayaRHI)
maya_target_options(MayaAssets)

# Scene persistence depends only on World data and a caller-supplied asset-reference resolver.
add_library(MayaScene STATIC
    src/maya/scene/scene_document.cpp
    src/maya/scene/scene_codec.cpp)
add_library(Maya::Scene ALIAS MayaScene)
target_link_libraries(MayaScene PUBLIC MayaWorld)
maya_target_options(MayaScene)

# Render extraction, views, offscreen targets, and passes. Reads Worlds; never owns or edits them.
add_library(MayaRenderer STATIC
    src/maya/renderer/render_snapshot.cpp
    src/maya/renderer/render_target.cpp
    src/maya/renderer/renderer.cpp)
add_library(Maya::Renderer ALIAS MayaRenderer)
target_link_libraries(MayaRenderer PUBLIC MayaWorld MayaAssets MayaRHI)
maya_target_options(MayaRenderer)

# Timing aggregation (percentiles, sample windows, frame timing) shared by live displays and benchmarks.
add_library(MayaMetrics STATIC src/maya/metrics/metrics.cpp)
add_library(Maya::Metrics ALIAS MayaMetrics)
target_include_directories(MayaMetrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
maya_target_options(MayaMetrics)

# Dependency headers are system headers, so Maya's warnings apply to Maya code only.
function(maya_system_headers target)
    get_target_property(directories ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${directories}")
endfunction()

# Jolt Physics (MIT), pinned (docs/architecture/physics-scripting-decision.md). Single precision,
# 32-bit object layers, no GPU compute, no serialization, no install rules. Jolt builds without
# exceptions and with its own warnings. RTTI is on so UndefinedBehaviorSanitizer's vptr check can see
# Jolt's classes, as in the rest of Maya. Only the Jolt library itself is part of `all`.
FetchContent_Declare(jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(OBJECT_LAYER_BITS 32 CACHE STRING "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
foreach(backend JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE)
    set(${backend} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_MakeAvailable(jolt)
set_property(DIRECTORY ${jolt_SOURCE_DIR}/Build PROPERTY EXCLUDE_FROM_ALL TRUE)
maya_system_headers(Jolt)

# Luau (MIT), pinned (docs/architecture/physics-scripting-decision.md#luau): the VM and the bytecode
# compiler only. The type checker, native code generation, and command-line tools are not built.
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 0.740
    GIT_SHALLOW TRUE)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(luau)
set_property(DIRECTORY ${luau_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
maya_system_headers(Luau.VM)
maya_system_headers(Luau.Compiler)

# Rigid-body physics for play sessions (docs/physics.md). The only target that includes Jolt's
# headers; its public headers carry no Jolt types (checked by maya_library_headers in CTest).
add_library(MayaPhysics STATIC
    src/maya/physics/jolt_runtime.cpp
    src/maya/physics/physics_world.cpp)
add_library(Maya::Physics ALIAS MayaPhysics)
target_link_libraries(MayaPhysics PUBLIC MayaWorld PRIVATE Jolt)
maya_target_options(MayaPhysics)

# How this build was made (revision, configuration, compiler), regenerated on every build so benchmark
# results and play recordings name the build they came from.
set(MAYA_BUILD_INFO "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp")
string(REPLACE ";" "," maya_sanitizers "${MAYA_SANITIZERS}")
if(NOT MAYA_ENABLE_SANITIZERS)
    set(maya_sanitizers "none")
endif()
add_custom_target(maya_build_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT=${MAYA_BUILD_INFO}
        -DBUILD_TYPE=${CMAKE_BUILD_TYPE} -DSANITIZERS=${maya_sanitizers}
        "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${MAYA_BUILD_INFO}
    COMMENT "Recording the build's revision and configuration"
    VERBATIM)
add_library(MayaBuildInfo STATIC ${MAYA_BUILD_INFO})
add_dependencies(MayaBuildInfo maya_build_info)
target_include_directories(MayaBuildInfo PUBLIC include)
maya_target_options(MayaBuildInfo)

# Play sessions: the fixed clock, gameplay input, and simulation systems shared by the player and the
# editor's play mode. CPU-only, on top of World and scene data.
add_library(MayaSimulation STATIC
    src/maya/simulation/simulation.cpp
    src/maya/simulation/authored_physics.cpp
    src/maya/simulation/play_session.cpp
    src/maya/simulation/recording.cpp
    src/maya/simulation/physics_debug.cpp
    src/maya/simulation/scripting/script_host.cpp
    src/maya/simulation/scripting/script_api.cpp
    src/maya/simulation/scripting/scripting.cpp)
add_library(Maya::Simulation ALIAS MayaSimulation)
# Luau is private to the scripting host (src/maya/simulation/scripting/); checked by maya_library_headers.
target_link_libraries(MayaSimulation PUBLIC MayaWorld MayaScene MayaPhysics MayaBuildInfo PRIVATE Luau.VM Luau.Compiler)
maya_target_options(MayaSimulation)

add_library(MayaRuntime STATIC
    src/maya/core/engine.cpp
    src/maya/core/camera.cpp
    src/maya/core/file_system.cpp
    src/maya/core/model_loader_legacy.cpp
    src/maya/core/primitives.cpp
    src/maya/core/system_info.mm
    src/maya/rhi/metal/metal_device.mm
)
add_library(Maya::Runtime ALIAS MayaRuntime)
target_include_directories(MayaRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(MayaRuntime PUBLIC MayaRHI MayaWorld MayaAssets MayaScene MayaRenderer MayaPhysics MayaSimulation MayaMetrics PRIVATE ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${COCOA_LIBRARY})
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
    # Runs a project's saved scene through a play session. The player and the sample share it.
    add_library(MayaPlayer STATIC apps/player/player_application.cpp)
    target_include_directories(MayaPlayer PUBLIC apps/player)
    target_link_libraries(MayaPlayer PUBLIC MayaRuntime)
    maya_target_options(MayaPlayer)
endif()

if(MAYA_BUILD_PLAYER)
    add_executable(maya_player apps/player/main.cpp)
    target_link_libraries(maya_player PRIVATE MayaDesktop MayaPlayer)
    maya_target_options(maya_player)
endif()

if(MAYA_BUILD_PLAYER OR MAYA_BUILD_SAMPLES)
    # The benchmark runner: headless, offscreen, driven by versioned manifests (docs/performance.md).
    add_library(MayaBenchmark STATIC apps/benchmark/benchmark.cpp)
    target_include_directories(MayaBenchmark PUBLIC apps/benchmark)
    target_link_libraries(MayaBenchmark PUBLIC MayaRuntime MayaBuildInfo)
    maya_target_options(MayaBenchmark)
    add_executable(maya_benchmark apps/benchmark/main.cpp)
    target_link_libraries(maya_benchmark PRIVATE MayaBenchmark)
    maya_target_options(maya_benchmark)
endif()

if(MAYA_BUILD_EDITOR)
    # Dear ImGui is an editor-only dependency: runtime, player, and sample targets never link it.
    # Only the core sources are built; Maya supplies its own platform and RHI renderer backends.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b-docking)
    FetchContent_MakeAvailable(imgui)
    add_library(MayaImGui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
    target_include_directories(MayaImGui SYSTEM PUBLIC ${imgui_SOURCE_DIR})
    target_include_directories(MayaImGui PUBLIC apps/editor/imgui)
    target_compile_definitions(MayaImGui PUBLIC IMGUI_USER_CONFIG="maya_imgui_config.h")
    # ImGuizmo (MIT) draws the transform gizmos. Pinned to the last commit before it moved to the
    # ImGui 1.92.8 drawing API; only its gizmo source is built, not its own CMake project.
    FetchContent_Declare(imguizmo
        GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
        GIT_TAG 22a6c900a33dcd7a72576bddc7bd21651a2deed4
        SOURCE_SUBDIR not-a-cmake-project)
    FetchContent_MakeAvailable(imguizmo)
    target_sources(MayaImGui PRIVATE ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
    target_include_directories(MayaImGui SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)

    add_library(MayaEditor STATIC
        apps/editor/editor_application.cpp
        apps/editor/editor_camera.cpp
        apps/editor/editor_preferences.cpp
        apps/editor/editor_shell.cpp
        apps/editor/editor_theme.cpp
        apps/editor/input_router.cpp
        apps/editor/inspector.cpp
        apps/editor/picking.cpp
        apps/editor/play_mode.cpp
        apps/editor/project_files.cpp
        apps/editor/scene_editor.cpp
        apps/editor/script_files.cpp
        apps/editor/ui_renderer.cpp
        apps/editor/viewport_tools.cpp)
    target_include_directories(MayaEditor PUBLIC apps/editor)
    target_link_libraries(MayaEditor PUBLIC MayaRuntime MayaImGui)
    maya_target_options(MayaEditor)

    add_executable(maya_editor apps/editor/main.cpp)
    target_link_libraries(maya_editor PRIVATE MayaDesktop MayaEditor)
    maya_target_options(maya_editor)
endif()

if(MAYA_BUILD_SAMPLES)
    add_executable(maya_sample samples/basic_scene/main.cpp)
    target_link_libraries(maya_sample PRIVATE MayaDesktop MayaPlayer)
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

    add_executable(maya_rhi_tests tests/rhi_validation_tests.cpp)
    target_link_libraries(maya_rhi_tests PRIVATE MayaRHI Catch2::Catch2WithMain)
    maya_target_options(maya_rhi_tests)
    add_test(NAME maya_rhi_api COMMAND maya_rhi_tests)
    set_tests_properties(maya_rhi_api PROPERTIES LABELS "cpu;rhi" TIMEOUT 60)

    add_executable(maya_scene_tests tests/scene_tests.cpp)
    target_link_libraries(maya_scene_tests PRIVATE MayaScene MayaAssets Catch2::Catch2WithMain)
    target_compile_definitions(maya_scene_tests PRIVATE MAYA_SAMPLE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/samples/basic_scene/assets")
    maya_target_options(maya_scene_tests)
    add_test(NAME maya_scene COMMAND maya_scene_tests)
    set_tests_properties(maya_scene PROPERTIES LABELS "cpu;scene" TIMEOUT 60)

    add_executable(maya_renderer_tests tests/renderer_tests.cpp)
    target_link_libraries(maya_renderer_tests PRIVATE MayaRenderer Catch2::Catch2WithMain)
    target_include_directories(maya_renderer_tests PRIVATE tests)
    maya_target_options(maya_renderer_tests)
    add_test(NAME maya_renderer COMMAND maya_renderer_tests)
    set_tests_properties(maya_renderer PROPERTIES LABELS "cpu;renderer" TIMEOUT 60)

    add_executable(maya_metrics_tests tests/metrics_tests.cpp)
    target_link_libraries(maya_metrics_tests PRIVATE MayaMetrics Catch2::Catch2WithMain)
    maya_target_options(maya_metrics_tests)
    add_test(NAME maya_metrics COMMAND maya_metrics_tests)
    set_tests_properties(maya_metrics PROPERTIES LABELS "cpu;metrics" TIMEOUT 30)

    add_executable(maya_simulation_tests tests/simulation_tests.cpp tests/replay_tests.cpp tests/physics_debug_tests.cpp)
    target_link_libraries(maya_simulation_tests PRIVATE MayaSimulation Catch2::Catch2WithMain)
    maya_target_options(maya_simulation_tests)
    add_test(NAME maya_simulation COMMAND maya_simulation_tests)
    set_tests_properties(maya_simulation PROPERTIES LABELS "cpu;simulation" TIMEOUT 60)

    add_executable(maya_physics_tests tests/physics_tests.cpp)
    target_link_libraries(maya_physics_tests PRIVATE MayaSimulation Catch2::Catch2WithMain)
    maya_target_options(maya_physics_tests)
    add_test(NAME maya_physics COMMAND maya_physics_tests)
    set_tests_properties(maya_physics PROPERTIES LABELS "cpu;physics" TIMEOUT 120)

    add_executable(maya_scripting_tests tests/scripting_tests.cpp)
    target_link_libraries(maya_scripting_tests PRIVATE MayaSimulation Catch2::Catch2WithMain)
    maya_target_options(maya_scripting_tests)
    add_test(NAME maya_scripting COMMAND maya_scripting_tests)
    set_tests_properties(maya_scripting PROPERTIES LABELS "cpu;scripting" TIMEOUT 120)

    # Only MayaPhysics includes Jolt and only the scripting host includes Luau; public headers carry
    # neither library's types (docs/architecture/runtime-world-contracts.md#physics-boundary).
    add_test(NAME maya_library_headers COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_library_headers.cmake)
    set_tests_properties(maya_library_headers PROPERTIES LABELS "cpu;isolation" TIMEOUT 30)

    add_executable(maya_asset_tests tests/asset_tests.cpp tests/project_tests.cpp)
    target_link_libraries(maya_asset_tests PRIVATE MayaAssets Catch2::Catch2WithMain)
    target_include_directories(maya_asset_tests PRIVATE tests)
    maya_target_options(maya_asset_tests)
    add_test(NAME maya_assets COMMAND maya_asset_tests)
    set_tests_properties(maya_assets PROPERTIES LABELS "cpu;assets" TIMEOUT 60)

    add_executable(maya_tests
        tests/math_tests.cpp
        tests/rhi_tests.cpp
        tests/renderer_gpu_tests.cpp
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
    target_include_directories(maya_tests PRIVATE tests)
    maya_target_options(maya_tests)

    add_test(NAME maya_cpu COMMAND maya_tests "~[rhi]")
    set_tests_properties(maya_cpu PROPERTIES LABELS "cpu" TIMEOUT 60)
    add_test(NAME maya_rhi COMMAND maya_tests "[rhi]")
    # GPU checks run with Metal API validation, which aborts on invalid Metal usage.
    set(MAYA_GPU_TEST_ENVIRONMENT "MTL_DEBUG_LAYER=1")
    set_tests_properties(maya_rhi PROPERTIES LABELS "gpu" TIMEOUT 60 ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")

    if(TARGET MayaEditor)
        add_executable(maya_editor_tests tests/editor_tests.cpp tests/editor_gpu_tests.cpp tests/scene_editor_tests.cpp
            tests/picking_tests.cpp tests/editor_project_tests.cpp tests/editor_play_tests.cpp tests/editor_physics_tests.cpp
            tests/editor_scripting_tests.cpp tests/editor_replay_tests.cpp tests/editor_physics_debug_tests.cpp tests/physics_debug_gpu_tests.cpp tests/acceptance_tests.cpp)
        target_include_directories(maya_editor_tests PRIVATE tests)
        # The system zlib compresses reference images (tests/support/png.hpp). Linking it directly keeps
        # the SDK's C headers from being added ahead of libc++'s, as the ZLIB package would.
        target_link_libraries(maya_editor_tests PRIVATE MayaEditor Catch2::Catch2WithMain z)
        set(MAYA_ACCEPTANCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/acceptance")
        target_compile_definitions(maya_editor_tests PRIVATE MAYA_ACCEPTANCE_DIR="${MAYA_ACCEPTANCE_DIR}"
            MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        maya_target_options(maya_editor_tests)
        add_test(NAME maya_editor COMMAND maya_editor_tests "~[gpu]~[acceptance]")
        set_tests_properties(maya_editor PROPERTIES LABELS "cpu;editor" TIMEOUT 60)
        add_test(NAME maya_editor_gpu COMMAND maya_editor_tests "[gpu]~[acceptance]~[visual]")
        set_tests_properties(maya_editor_gpu PROPERTIES LABELS "gpu;editor" TIMEOUT 60
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")

        # Milestone acceptance (docs/acceptance.md): authoring in the editor writes a project that the
        # player's path renders on Metal and the real maya_player runs from another directory.
        add_test(NAME maya_acceptance_author COMMAND maya_editor_tests "[acceptance][author]")
        set_tests_properties(maya_acceptance_author PROPERTIES LABELS "cpu;acceptance" TIMEOUT 60
            FIXTURES_SETUP authored_project)
        add_test(NAME maya_acceptance_render COMMAND maya_editor_tests "[acceptance][gpu]")
        set_tests_properties(maya_acceptance_render PROPERTIES LABELS "gpu;acceptance" TIMEOUT 120 RUN_SERIAL TRUE
            FIXTURES_REQUIRED authored_project ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        add_test(NAME maya_visual_reference COMMAND maya_editor_tests "[visual]")
        set_tests_properties(maya_visual_reference PROPERTIES LABELS "gpu;acceptance;visual" TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        if(TARGET maya_player)
            add_test(NAME maya_acceptance_player COMMAND ${CMAKE_COMMAND}
                "-DCOMMAND=$<TARGET_FILE:maya_player>|../Authored Game|levels/authored.scene|--smoke|120"
                -DEXIT_CODE=0 "-DEXPECT=Authored Game / levels/authored.scene: 6 entities"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
            set_tests_properties(maya_acceptance_player PROPERTIES LABELS "gpu;smoke;acceptance" TIMEOUT 60 RUN_SERIAL TRUE
                WORKING_DIRECTORY "${MAYA_ACCEPTANCE_DIR}/elsewhere" FIXTURES_REQUIRED authored_project
                ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        endif()
    endif()

    if(TARGET MayaDesktop)
        add_executable(maya_desktop_tests tests/desktop_lifecycle_tests.cpp)
        target_link_libraries(maya_desktop_tests PRIVATE MayaDesktop Catch2::Catch2WithMain)
        # Drive the real player and editor applications when they are built.
        if(TARGET MayaPlayer)
            target_link_libraries(maya_desktop_tests PRIVATE MayaPlayer)
            target_compile_definitions(maya_desktop_tests PRIVATE MAYA_TEST_PLAYER=1)
        endif()
        if(TARGET MayaEditor)
            target_link_libraries(maya_desktop_tests PRIVATE MayaEditor)
            target_compile_definitions(maya_desktop_tests PRIVATE MAYA_TEST_EDITOR=1)
        endif()
        maya_target_options(maya_desktop_tests)
        add_test(NAME maya_desktop_lifecycle COMMAND maya_desktop_tests)
        set_tests_properties(maya_desktop_lifecycle PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
    endif()

    # Runtime targets must stay free of the editor UI framework. The editor itself is the positive
    # control that proves the check detects it.
    foreach(target MayaRuntime MayaRenderer MayaDesktop maya_player maya_sample maya_benchmark maya_editor)
        if(TARGET ${target})
            add_test(NAME ${target}_no_editor_ui COMMAND ${CMAKE_COMMAND}
                -DBINARY=$<TARGET_FILE:${target}> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_no_editor_ui.cmake)
            set_tests_properties(${target}_no_editor_ui PROPERTIES LABELS "cpu;isolation" TIMEOUT 30)
        endif()
    endforeach()
    if(TARGET maya_editor)
        set_tests_properties(maya_editor_no_editor_ui PROPERTIES WILL_FAIL TRUE)
    endif()

    # The player explains why it cannot run a scene, with distinct exit codes for bad arguments (2)
    # and for projects and scenes that cannot be run (1). Commands separate their arguments with "|".
    if(TARGET maya_player)
        function(maya_player_failure name code expect)
            list(JOIN ARGN "|" arguments)
            add_test(NAME maya_player_${name} COMMAND ${CMAKE_COMMAND} "-DCOMMAND=$<TARGET_FILE:maya_player>|${arguments}"
                -DEXIT_CODE=${code} "-DEXPECT=${expect}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
            if(code EQUAL 2)
                set_tests_properties(maya_player_${name} PROPERTIES LABELS "cpu" TIMEOUT 15)
            else()
                set_tests_properties(maya_player_${name} PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE
                    ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
            endif()
        endfunction()
        set(sample_project ${CMAKE_CURRENT_SOURCE_DIR}/samples/basic_scene)
        maya_player_failure(extra_argument 2 "unexpected argument: c" a b c)
        maya_player_failure(missing_project 1 "Cannot read project file" ${CMAKE_CURRENT_BINARY_DIR}/no-such-project --smoke 3)
        maya_player_failure(missing_scene 1 "missing.scene cannot be run" ${sample_project} missing.scene --smoke 3)
        maya_player_failure(scene_outside_project 1 "outside the project's content root" ${sample_project} ../project.maya --smoke 3)
        maya_player_failure(record_without_file 2 "--record needs a file" --record)
        maya_player_failure(record_and_replay 2 "--record and --replay cannot be used together" --record a --replay b)
        maya_player_failure(missing_recording 1 "cannot read the recording" --replay ${CMAKE_CURRENT_BINARY_DIR}/no-such.recording --smoke 3)
        # A recorded smoke run replays to the same result (#1023).
        set(smoke_recording ${CMAKE_CURRENT_BINARY_DIR}/smoke-physics.recording)
        add_test(NAME maya_player_record COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_player>|${sample_project}|physics.scene|--smoke|120|--record|${smoke_recording}"
            -DEXIT_CODE=0 "-DEXPECT=recorded 120 ticks" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        add_test(NAME maya_player_replay COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_player>|${sample_project}|--smoke|150|--replay|${smoke_recording}"
            -DEXIT_CODE=0 "-DEXPECT=the replay matches the recording: 120 ticks, 2 checkpoints" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_player_record PROPERTIES FIXTURES_SETUP smoke_recording)
        set_tests_properties(maya_player_replay PROPERTIES FIXTURES_REQUIRED smoke_recording)
        set_tests_properties(maya_player_record maya_player_replay PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        # Physics debug views draw in the player only when asked (#1022).
        add_test(NAME maya_player_debug_physics COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_player>|${sample_project}|physics.scene|--debug-physics|--smoke|60"
            -DEXIT_CODE=0 "-DEXPECT=drawing the physics debug views" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_player_debug_physics PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
    endif()

    # The benchmark runner: its logic on the null device (CPU), and its command line on Metal with
    # the small smoke manifests, which must complete and report results.
    if(TARGET MayaBenchmark)
        add_executable(maya_benchmark_tests tests/benchmark_tests.cpp)
        target_link_libraries(maya_benchmark_tests PRIVATE MayaBenchmark Catch2::Catch2WithMain)
        maya_target_options(maya_benchmark_tests)
        add_test(NAME maya_benchmark COMMAND maya_benchmark_tests "~[gpu]")
        set_tests_properties(maya_benchmark PROPERTIES LABELS "cpu;benchmark" TIMEOUT 120)
        add_test(NAME maya_benchmark_gpu COMMAND maya_benchmark_tests "[gpu]")
        set_tests_properties(maya_benchmark_gpu PROPERTIES LABELS "gpu;benchmark" TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        foreach(manifest sample i1_1k l1_load l1_play)
            add_test(NAME maya_benchmark_smoke_${manifest} COMMAND ${CMAKE_COMMAND}
                "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/smoke/${manifest}.benchmark|${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-${manifest}.json"
                -DEXIT_CODE=0 "-DEXPECT=results: " -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
            set_tests_properties(maya_benchmark_smoke_${manifest} PROPERTIES LABELS "gpu;smoke;benchmark" TIMEOUT 120
                RUN_SERIAL TRUE ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        endforeach()
        add_test(NAME maya_benchmark_bad_manifest COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
            -DEXIT_CODE=2 "-DEXPECT=expected a maya-benchmark header" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_benchmark_bad_manifest PROPERTIES LABELS "cpu;benchmark" TIMEOUT 15)
    endif()

    foreach(app maya_player maya_editor maya_sample)
        if(TARGET ${app})
            add_test(NAME ${app}_help COMMAND ${app} --help)
            set_tests_properties(${app}_help PROPERTIES LABELS "cpu" TIMEOUT 15)
            add_test(NAME ${app}_reject_zero_frames COMMAND ${app} --smoke 0)
            set_tests_properties(${app}_reject_zero_frames PROPERTIES LABELS "cpu" WILL_FAIL TRUE TIMEOUT 15)
            add_test(NAME ${app}_smoke COMMAND ${app} --smoke 5)
            set_tests_properties(${app}_smoke PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE
                ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        endif()
    endforeach()
endif()
