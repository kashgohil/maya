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
option(MAYA_BUILD_PROTOTYPES "Build the world-scale prototypes (#1060)" OFF)
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
    src/maya/core/file_replace.cpp
    src/maya/core/sha256.cpp
    src/maya/world/world.cpp
    src/maya/world/spatial.cpp
    src/maya/world/name_path.cpp
    src/maya/assets/animation.cpp
    src/maya/properties/schema.cpp)
add_library(Maya::World ALIAS MayaWorld)
target_include_directories(MayaWorld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
maya_target_options(MayaWorld)

# The process's job system: frame and background tiers, dependencies, cancellation, and owner-thread
# completions (docs/jobs.md). It depends on nothing else in Maya.
add_library(MayaJobs STATIC src/maya/jobs/jobs.cpp)
add_library(Maya::Jobs ALIAS MayaJobs)
target_include_directories(MayaJobs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
maya_target_options(MayaJobs)

# CPU-side graphics API: handle tables, validation, deferred retirement, and a null backend.
add_library(MayaRHI STATIC
    src/maya/rhi/graphics_device.cpp
    src/maya/rhi/null_device.cpp)
add_library(Maya::RHI ALIAS MayaRHI)
target_include_directories(MayaRHI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
maya_target_options(MayaRHI)

# Dependency headers are system headers, so Maya's warnings apply to Maya code only.
function(maya_system_headers target)
    get_target_property(directories ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${directories}")
endfunction()

# stb (public domain or MIT; no releases are tagged, so pinned by commit) and astc-encoder
# (Apache-2.0), for cooking textures (docs/architecture/rendering-content-decision.md).
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_Declare(astcenc GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git GIT_TAG 5.7.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(stb)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(astcenc)
set_property(DIRECTORY ${astcenc_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)

# Texture descriptors, KTX2, cooking source images, and cooking environments (stb and astcenc stay
# private to it).
add_library(MayaTextures STATIC
    src/maya/assets/texture_settings.cpp
    src/maya/assets/ktx2.cpp
    src/maya/assets/texture_cook.cpp
    src/maya/assets/environment_cook.cpp
    src/maya/assets/stb_implementation.cpp)
add_library(Maya::Textures ALIAS MayaTextures)
target_include_directories(MayaTextures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(MayaTextures SYSTEM PRIVATE ${stb_SOURCE_DIR} ${astcenc_SOURCE_DIR}/Source)
target_link_libraries(MayaTextures PUBLIC MayaRHI MayaJobs PRIVATE astcenc-neon-static)
maya_target_options(MayaTextures)
# stb's own code is not sanitized, like the other dependencies: stb_image_resize2 indexes its sRGB
# table through a pointer offset before the table (in bounds once indexed), which UBSan reports.
if(MAYA_ENABLE_SANITIZERS)
    set_source_files_properties(src/maya/assets/stb_implementation.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=all")
endif()

# MikkTSpace (zlib), the reference tangent generator glTF requires; no releases are tagged, so pinned
# by commit. Its one C file is built as C, unchanged and without Maya's warnings.
enable_language(C)
FetchContent_Declare(mikktspace GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git GIT_TAG 3e895b49d05ea07e4c2133156cfa94369e19e409)
FetchContent_MakeAvailable(mikktspace)
add_library(MayaMikkTSpace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(MayaMikkTSpace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})

# cgltf (MIT, one header), the glTF 2.0 parser #1030 chose, pinned at v1.15's commit. Only MayaAssets
# includes it (gltf.cpp); its types never appear in Maya's headers.
FetchContent_Declare(cgltf GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git GIT_TAG 360db1a95480fe102ae9c69b27c5d101167ff5ba
    SOURCE_SUBDIR not-a-cmake-project)
FetchContent_MakeAvailable(cgltf)

add_library(MayaAssets STATIC
    src/maya/assets/registry.cpp
    src/maya/assets/property_context.cpp
    src/maya/assets/file_provider.cpp
    src/maya/assets/asset_cooker.cpp
    src/maya/assets/package.cpp
    src/maya/assets/material_file.cpp
    src/maya/assets/project.cpp
    src/maya/core/model_loader.cpp
    src/maya/core/tangents.cpp
    src/maya/assets/gltf.cpp
    src/maya/assets/import_file.cpp
    src/maya/assets/cook_cache.cpp)
add_library(Maya::Assets ALIAS MayaAssets)
target_link_libraries(MayaAssets PUBLIC MayaWorld MayaRHI MayaTextures PRIVATE MayaMikkTSpace)
target_include_directories(MayaAssets SYSTEM PRIVATE ${cgltf_SOURCE_DIR})
maya_target_options(MayaAssets)

# Scene persistence depends only on World data and a caller-supplied asset-reference resolver.
add_library(MayaScene STATIC
    src/maya/scene/scene_document.cpp
    src/maya/scene/scene_codec.cpp
    src/maya/scene/scene_binary.cpp
    src/maya/scene/world_io.cpp)
add_library(Maya::Scene ALIAS MayaScene)
target_link_libraries(MayaScene PUBLIC MayaWorld)
maya_target_options(MayaScene)

# Importing content (#1036): reads glTF files with MayaAssets and writes material, scene, import, and
# catalog files. It loads nothing on the GPU.
add_library(MayaImport STATIC src/maya/import/gltf_import.cpp)
add_library(Maya::Import ALIAS MayaImport)
target_link_libraries(MayaImport PUBLIC MayaAssets MayaScene)
maya_target_options(MayaImport)

# Render extraction, views, offscreen targets, and passes. Reads Worlds; never owns or edits them.
add_library(MayaRenderer STATIC
    src/maya/renderer/render_snapshot.cpp
    src/maya/renderer/light_plan.cpp
    src/maya/renderer/draw_batches.cpp
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

# Jolt Physics (MIT), pinned (docs/architecture/physics-scripting-decision.md). Double precision
# positions (#1065, docs/architecture/world-scale-decision.md#coordinates-double-positions),
# 32-bit object layers, no GPU compute, no serialization, no install rules. Jolt builds without
# exceptions and with its own warnings. RTTI is on so UndefinedBehaviorSanitizer's vptr check can see
# Jolt's classes, as in the rest of Maya. Only the Jolt library itself is part of `all`.
FetchContent_Declare(jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build)
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
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
target_link_libraries(MayaPhysics PUBLIC MayaWorld MayaJobs PRIVATE Jolt)
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
    src/maya/simulation/animation.cpp
    src/maya/simulation/scripting/script_host.cpp
    src/maya/simulation/scripting/script_api.cpp
    src/maya/simulation/scripting/scripting.cpp)
add_library(Maya::Simulation ALIAS MayaSimulation)
# Luau is private to the scripting host (src/maya/simulation/scripting/); checked by maya_library_headers.
target_link_libraries(MayaSimulation PUBLIC MayaWorld MayaScene MayaPhysics MayaBuildInfo PRIVATE Luau.VM Luau.Compiler)
maya_target_options(MayaSimulation)

# World streaming (#1064, docs/world.md#streaming): cells load on jobs, cook to binary through the cook
# cache, and activate into a play session's World within a frame budget.
add_library(MayaStreaming STATIC src/maya/streaming/world_streamer.cpp)
add_library(Maya::Streaming ALIAS MayaStreaming)
target_include_directories(MayaStreaming PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(MayaStreaming PUBLIC MayaSimulation MayaAssets MayaJobs)
maya_target_options(MayaStreaming)

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
    target_link_libraries(MayaPlayer PUBLIC MayaRuntime MayaStreaming)
    maya_target_options(MayaPlayer)
endif()

if(MAYA_BUILD_PLAYER)
    add_executable(maya_player apps/player/main.cpp)
    target_link_libraries(maya_player PRIVATE MayaDesktop MayaPlayer)
    maya_target_options(maya_player)
endif()

if(MAYA_BUILD_PLAYER)
    # Packages a project as a standalone application with its cooked content (docs/projects.md#packages).
    add_library(MayaPackage STATIC src/maya/package/packager.cpp)
    target_link_libraries(MayaPackage PUBLIC MayaAssets MayaScene MayaBuildInfo MayaMetrics)
    maya_target_options(MayaPackage)
    add_executable(maya_package apps/package/main.cpp)
    target_link_libraries(maya_package PRIVATE MayaPackage MayaRuntime)
    add_dependencies(maya_package maya_player) # bundled from beside it
    maya_target_options(maya_package)
endif()

if(MAYA_BUILD_PLAYER OR MAYA_BUILD_SAMPLES)
    # R1, the realistic reference environment (#1040), assembled from the fetched samples (docs/acceptance.md#r1).
    add_library(MayaR1 STATIC apps/r1/r1.cpp)
    target_include_directories(MayaR1 PUBLIC apps/r1)
    target_link_libraries(MayaR1 PUBLIC MayaImport MayaScene)
    maya_target_options(MayaR1)
    add_executable(maya_r1 apps/r1/main.cpp)
    target_link_libraries(maya_r1 PRIVATE MayaR1)
    maya_target_options(maya_r1)

    # The benchmark runner: headless, offscreen, driven by versioned manifests (docs/performance.md).
    add_library(MayaBenchmark STATIC apps/benchmark/benchmark.cpp apps/benchmark/physics_workload.cpp apps/benchmark/import_workload.cpp
        apps/benchmark/animation_workload.cpp apps/benchmark/stream_workload.cpp)
    target_include_directories(MayaBenchmark PUBLIC apps/benchmark)
    target_link_libraries(MayaBenchmark PUBLIC MayaRuntime MayaImport MayaBuildInfo MayaStreaming)
    maya_target_options(MayaBenchmark)
    add_executable(maya_benchmark apps/benchmark/main.cpp)
    # A window, and a display kept awake, for presenting runs.
    target_link_libraries(maya_benchmark PRIVATE MayaBenchmark MayaDesktop "-framework IOKit" "-framework CoreFoundation")
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
        apps/editor/import_files.cpp
        apps/editor/input_router.cpp
        apps/editor/inspector.cpp
        apps/editor/picking.cpp
        apps/editor/play_mode.cpp
        apps/editor/project_files.cpp
        apps/editor/scene_editor.cpp
        apps/editor/script_files.cpp
        apps/editor/watched_files.cpp
        apps/editor/texture_thumbnails.cpp
        apps/editor/ui_renderer.cpp
        apps/editor/viewport_tools.cpp)
    target_include_directories(MayaEditor PUBLIC apps/editor)
    target_link_libraries(MayaEditor PUBLIC MayaRuntime MayaImport MayaImGui)
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

if(MAYA_BUILD_PROTOTYPES)
    # Prototypes for the world-scale decisions (#1060, docs/architecture/world-scale-decision.md). They
    # are not engine code; the milestone's issues replace them. Sources are pinned, and their headers are
    # system headers, so Maya's warnings apply to Maya code only.
    # enkiTS (zlib) and Taskflow (MIT): job system candidates beside a Maya-owned pool and GCD.
    FetchContent_Declare(enkits GIT_REPOSITORY https://github.com/dougbinks/enkiTS.git GIT_TAG v1.12 GIT_SHALLOW TRUE)
    FetchContent_Declare(taskflow GIT_REPOSITORY https://github.com/taskflow/taskflow.git GIT_TAG v4.1.0 GIT_SHALLOW TRUE)
    # meshoptimizer (MIT): LOD simplification at cook time.
    FetchContent_Declare(meshoptimizer GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git GIT_TAG v1.3 GIT_SHALLOW TRUE)
    set(ENKITS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ENKITS_BUILD_C_INTERFACE OFF CACHE BOOL "" FORCE)
    set(ENKITS_INSTALL OFF CACHE BOOL "" FORCE)
    set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TF_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(enkits meshoptimizer taskflow)
    foreach(library enkiTS meshoptimizer)
        maya_system_headers(${library})
    endforeach()

    # Jolt again with JPH_DOUBLE_PRECISION, for the coordinate comparison: the same sources, definitions,
    # and options as the engine's single-precision Jolt. The two are never linked into one executable.
    get_target_property(jolt_sources Jolt SOURCES)
    list(FILTER jolt_sources INCLUDE REGEX "\\.cpp$")
    add_library(MayaPrototypeJoltDouble STATIC ${jolt_sources})
    get_target_property(jolt_includes Jolt INCLUDE_DIRECTORIES)
    get_target_property(jolt_definitions Jolt COMPILE_DEFINITIONS)
    get_target_property(jolt_options Jolt COMPILE_OPTIONS)
    target_include_directories(MayaPrototypeJoltDouble SYSTEM PUBLIC ${jolt_includes})
    target_compile_definitions(MayaPrototypeJoltDouble PUBLIC ${jolt_definitions} JPH_DOUBLE_PRECISION)
    if(jolt_options)
        target_compile_options(MayaPrototypeJoltDouble PRIVATE ${jolt_options})
    endif()
    # Jolt's own directory flags (Build/CMakeLists.txt), which a target outside that directory lacks.
    target_compile_options(MayaPrototypeJoltDouble PRIVATE -frtti -fno-exceptions -ffp-contract=on -faligned-allocation)
    set_target_properties(MayaPrototypeJoltDouble PROPERTIES CXX_STANDARD 17)

    add_executable(maya_jobs_prototype prototypes/world/jobs_prototype.cpp)
    target_include_directories(maya_jobs_prototype SYSTEM PRIVATE ${taskflow_SOURCE_DIR})
    target_link_libraries(maya_jobs_prototype PRIVATE enkiTS Jolt)
    add_executable(maya_precision_prototype prototypes/world/precision_prototype.cpp)
    target_link_libraries(maya_precision_prototype PRIVATE Jolt)
    add_executable(maya_precision_prototype_double prototypes/world/precision_prototype.cpp)
    target_link_libraries(maya_precision_prototype_double PRIVATE MayaPrototypeJoltDouble)
    add_executable(maya_cells_prototype prototypes/world/cells_prototype.cpp)
    target_link_libraries(maya_cells_prototype PRIVATE MayaScene MayaAssets Jolt)
    add_executable(maya_visibility_prototype prototypes/world/visibility_prototype.cpp)
    target_include_directories(maya_visibility_prototype SYSTEM PRIVATE ${cgltf_SOURCE_DIR})
    target_link_libraries(maya_visibility_prototype PRIVATE MayaAssets meshoptimizer)
    target_compile_definitions(maya_visibility_prototype PRIVATE MAYA_DEFAULT_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/build/render-samples"
        MAYA_DEFAULT_WORLD_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/build/world-samples")
    add_executable(maya_terrain_prototype prototypes/world/terrain_prototype.mm)
    target_link_libraries(maya_terrain_prototype PRIVATE MayaWorld Jolt "-framework Foundation" ${METAL_LIBRARY})
    target_compile_options(maya_terrain_prototype PRIVATE -fobjc-arc)
    foreach(prototype maya_jobs_prototype maya_precision_prototype maya_precision_prototype_double maya_cells_prototype
            maya_visibility_prototype maya_terrain_prototype)
        target_include_directories(${prototype} PRIVATE prototypes)
        maya_target_options(${prototype})
    endforeach()
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
    target_include_directories(maya_rhi_tests PRIVATE tests)
    maya_target_options(maya_rhi_tests)
    add_test(NAME maya_rhi_api COMMAND maya_rhi_tests)
    set_tests_properties(maya_rhi_api PROPERTIES LABELS "cpu;rhi" TIMEOUT 60)

    add_executable(maya_scene_tests tests/scene_tests.cpp)
    target_link_libraries(maya_scene_tests PRIVATE MayaScene MayaAssets Catch2::Catch2WithMain)
    target_compile_definitions(maya_scene_tests PRIVATE MAYA_SAMPLE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/samples/basic_scene/assets")
    maya_target_options(maya_scene_tests)
    add_test(NAME maya_scene COMMAND maya_scene_tests)
    set_tests_properties(maya_scene PROPERTIES LABELS "cpu;scene" TIMEOUT 60)

    add_executable(maya_renderer_tests tests/renderer_tests.cpp tests/light_plan_tests.cpp tests/draw_batches_tests.cpp)
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

    # The job system (docs/jobs.md). The timing case runs alone: it measures every core.
    add_executable(maya_jobs_tests tests/jobs_tests.cpp)
    target_link_libraries(maya_jobs_tests PRIVATE MayaJobs MayaMetrics Catch2::Catch2WithMain)
    maya_target_options(maya_jobs_tests)
    add_test(NAME maya_jobs COMMAND maya_jobs_tests "~[timing]")
    set_tests_properties(maya_jobs PROPERTIES LABELS "cpu;jobs" TIMEOUT 120)
    add_test(NAME maya_jobs_timing COMMAND maya_jobs_tests "[timing]")
    set_tests_properties(maya_jobs_timing PROPERTIES LABELS "cpu;jobs" TIMEOUT 60 RUN_SERIAL TRUE)

    add_executable(maya_physics_tests tests/physics_tests.cpp)
    target_link_libraries(maya_physics_tests PRIVATE MayaSimulation Catch2::Catch2WithMain)
    maya_target_options(maya_physics_tests)
    add_test(NAME maya_physics COMMAND maya_physics_tests)
    set_tests_properties(maya_physics PROPERTIES LABELS "cpu;physics" TIMEOUT 120)

    add_executable(maya_streaming_tests tests/streaming_tests.cpp)
    target_link_libraries(maya_streaming_tests PRIVATE MayaStreaming Catch2::Catch2WithMain)
    maya_target_options(maya_streaming_tests)
    add_test(NAME maya_streaming COMMAND maya_streaming_tests)
    set_tests_properties(maya_streaming PROPERTIES LABELS "cpu;streaming" TIMEOUT 120)

    add_executable(maya_scripting_tests tests/scripting_tests.cpp tests/precision_tests.cpp)
    target_link_libraries(maya_scripting_tests PRIVATE MayaSimulation Catch2::Catch2WithMain)
    maya_target_options(maya_scripting_tests)
    add_test(NAME maya_scripting COMMAND maya_scripting_tests)
    set_tests_properties(maya_scripting PROPERTIES LABELS "cpu;scripting" TIMEOUT 120)

    # Only MayaPhysics includes Jolt and only the scripting host includes Luau; public headers carry
    # neither library's types (docs/architecture/runtime-world-contracts.md#physics-boundary).
    add_test(NAME maya_library_headers COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_library_headers.cmake)
    set_tests_properties(maya_library_headers PROPERTIES LABELS "cpu;isolation" TIMEOUT 30)
    # check_milestone.sh reports failed and flaky tests and keeps their logs (#1027): checked on a throwaway
    # CTest project with a passing, a failing, and a flaky test.
    add_test(NAME maya_check_milestone_selftest COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_milestone_selftest.sh"
        "${CMAKE_CURRENT_BINARY_DIR}/check-milestone-selftest")
    set_tests_properties(maya_check_milestone_selftest PROPERTIES LABELS "cpu" TIMEOUT 60)

    # Khronos sample content from tools/fetch_render_samples.sh: the glTF, animation, and R1 tests read it
    # when it is present.
    set(MAYA_RENDER_SAMPLES "${CMAKE_CURRENT_SOURCE_DIR}/build/render-samples" CACHE PATH
        "Khronos sample content, from tools/fetch_render_samples.sh")
    add_executable(maya_asset_tests tests/asset_tests.cpp tests/project_tests.cpp tests/texture_asset_tests.cpp tests/material_tests.cpp
        tests/environment_tests.cpp tests/gltf_tests.cpp tests/import_tests.cpp tests/cook_cache_tests.cpp tests/asset_loading_tests.cpp)
    # The system zlib writes test PNGs (tests/support/png.hpp).
    target_link_libraries(maya_asset_tests PRIVATE MayaImport Catch2::Catch2WithMain z)
    target_include_directories(maya_asset_tests PRIVATE tests)
    target_compile_definitions(maya_asset_tests PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        MAYA_RENDER_SAMPLES="${MAYA_RENDER_SAMPLES}")
    maya_target_options(maya_asset_tests)
    add_test(NAME maya_assets COMMAND maya_asset_tests "~[samples]")
    set_tests_properties(maya_assets PROPERTIES LABELS "cpu;assets" TIMEOUT 60)
    # The glTF tests that read the Khronos samples (skipped when they are not fetched).
    add_test(NAME maya_assets_samples COMMAND maya_asset_tests "[samples]")
    set_tests_properties(maya_assets_samples PROPERTIES LABELS "cpu;assets" TIMEOUT 120)

    # Skeletal animation (#1038): sampling, binding, the animation system, and skinning's joint matrices
    # against an independent evaluator that reads the Khronos samples with cgltf alone.
    add_executable(maya_animation_tests tests/animation_tests.cpp tests/animation_samples_tests.cpp)
    target_link_libraries(maya_animation_tests PRIVATE MayaImport MayaSimulation MayaRenderer Catch2::Catch2WithMain)
    target_include_directories(maya_animation_tests PRIVATE tests)
    target_include_directories(maya_animation_tests SYSTEM PRIVATE ${cgltf_SOURCE_DIR})
    target_compile_definitions(maya_animation_tests PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        MAYA_RENDER_SAMPLES="${MAYA_RENDER_SAMPLES}")
    maya_target_options(maya_animation_tests)
    add_test(NAME maya_animation COMMAND maya_animation_tests "~[samples]")
    set_tests_properties(maya_animation PROPERTIES LABELS "cpu;animation" TIMEOUT 60)
    add_test(NAME maya_animation_samples COMMAND maya_animation_tests "[samples]")
    set_tests_properties(maya_animation_samples PROPERTIES LABELS "cpu;animation" TIMEOUT 180)

    # KTX2 interoperability: Khronos's `ktx validate` (KTX-Software 4.3 or later) checks every format
    # and layout Maya writes, when the tool is installed or MAYA_KTX_TOOL names it.
    add_executable(maya_ktx2_samples tests/ktx2_samples.cpp)
    target_link_libraries(maya_ktx2_samples PRIVATE MayaTextures)
    maya_target_options(maya_ktx2_samples)
    find_program(MAYA_KTX_TOOL ktx DOC "Khronos's ktx tool, for validating KTX2 files")
    if(MAYA_KTX_TOOL)
        add_test(NAME maya_ktx2_validate COMMAND ${CMAKE_COMMAND} -DSAMPLES=$<TARGET_FILE:maya_ktx2_samples>
            -DKTX=${MAYA_KTX_TOOL} -DFOLDER=${CMAKE_CURRENT_BINARY_DIR}/ktx2-samples
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_ktx2.cmake)
        set_tests_properties(maya_ktx2_validate PROPERTIES LABELS "cpu;assets" TIMEOUT 60)
    else()
        message(STATUS "Khronos's ktx tool was not found: KTX2 files are not checked by ktx validate (set MAYA_KTX_TOOL)")
    endif()

    add_executable(maya_tests
        tests/math_tests.cpp
        tests/rhi_tests.cpp
        tests/renderer_gpu_tests.cpp
        tests/material_gpu_tests.cpp
        tests/lighting_gpu_tests.cpp
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
        tests/texture_gpu_tests.cpp
        tests/engine_tests.cpp)
    target_link_libraries(maya_tests PRIVATE MayaRuntime Catch2::Catch2WithMain z)
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
            tests/editor_scripting_tests.cpp tests/editor_replay_tests.cpp tests/editor_physics_debug_tests.cpp tests/physics_debug_gpu_tests.cpp tests/editor_texture_tests.cpp tests/editor_material_tests.cpp tests/editor_environment_tests.cpp tests/editor_import_tests.cpp tests/editor_lighting_tests.cpp tests/editor_debug_view_tests.cpp tests/sample_viewer_tests.cpp tests/animation_gpu_tests.cpp tests/editor_animation_tests.cpp tests/r1_tests.cpp tests/content_acceptance_tests.cpp tests/acceptance_tests.cpp
            tests/physics_acceptance_tests.cpp)
        target_include_directories(maya_editor_tests PRIVATE tests)
        # The system zlib compresses reference images (tests/support/png.hpp). Linking it directly keeps
        # the SDK's C headers from being added ahead of libc++'s, as the ZLIB package would.
        target_link_libraries(maya_editor_tests PRIVATE MayaEditor MayaR1 Catch2::Catch2WithMain z)
        set(MAYA_ACCEPTANCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/acceptance")
        set(MAYA_R1_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/r1")
        target_compile_definitions(maya_editor_tests PRIVATE MAYA_ACCEPTANCE_DIR="${MAYA_ACCEPTANCE_DIR}"
            MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" MAYA_RENDER_SAMPLES="${MAYA_RENDER_SAMPLES}" MAYA_R1_PROJECT="${MAYA_R1_PROJECT}")
        maya_target_options(maya_editor_tests)
        add_test(NAME maya_editor COMMAND maya_editor_tests "~[gpu]~[acceptance]~[r1]")
        set_tests_properties(maya_editor PROPERTIES LABELS "cpu;editor" TIMEOUT 120)
        add_test(NAME maya_editor_gpu COMMAND maya_editor_tests "[gpu]~[acceptance]~[visual]")
        set_tests_properties(maya_editor_gpu PROPERTIES LABELS "gpu;editor" TIMEOUT 60
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")

        # Milestone acceptance (docs/acceptance.md): authoring in the editor writes a project that the
        # player's path renders on Metal and the real maya_player runs from another directory.
        add_test(NAME maya_acceptance_author COMMAND maya_editor_tests "[acceptance][author]~[physics]~[content]")
        set_tests_properties(maya_acceptance_author PROPERTIES LABELS "cpu;acceptance" TIMEOUT 60
            FIXTURES_SETUP authored_project)
        add_test(NAME maya_acceptance_render COMMAND maya_editor_tests "[acceptance][gpu]")
        set_tests_properties(maya_acceptance_render PROPERTIES LABELS "gpu;acceptance" TIMEOUT 120 RUN_SERIAL TRUE
            FIXTURES_REQUIRED authored_project ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        # Milestone 3 (#1040): content imported, placed, edited, lit, played, reloaded, and saved in the editor;
        # package_tests.cpp then packages it and runs it outside the checkout.
        add_test(NAME maya_content_acceptance_author COMMAND maya_editor_tests "[acceptance][content][author]")
        set_tests_properties(maya_content_acceptance_author PROPERTIES LABELS "cpu;acceptance" TIMEOUT 120
            FIXTURES_SETUP content_project)
        # Milestone 2 (#1024): the reference physics interaction authored, played, edited, replayed, reset,
        # and broken in the editor; then the real player runs it from another directory.
        add_test(NAME maya_physics_acceptance_author COMMAND maya_editor_tests "[acceptance][physics][author]")
        set_tests_properties(maya_physics_acceptance_author PROPERTIES LABELS "cpu;acceptance" TIMEOUT 120
            FIXTURES_SETUP physics_project)
        if(TARGET maya_player)
            add_test(NAME maya_physics_acceptance_player COMMAND ${CMAKE_COMMAND}
                "-DCOMMAND=$<TARGET_FILE:maya_player>|../Physics Game|levels/door.scene|--smoke|900"
                -DEXIT_CODE=0 "-DEXPECT=the door slides open for (Crate|Ball)" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
            set_tests_properties(maya_physics_acceptance_player PROPERTIES LABELS "gpu;smoke;acceptance" TIMEOUT 90 RUN_SERIAL TRUE
                WORKING_DIRECTORY "${MAYA_ACCEPTANCE_DIR}/elsewhere" FIXTURES_REQUIRED physics_project
                ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        endif()
        add_test(NAME maya_visual_reference COMMAND maya_editor_tests "[visual]~[r1]")
        set_tests_properties(maya_visual_reference PROPERTIES LABELS "gpu;acceptance;visual" TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        # Milestone 3 (#1040): R1 assembled from the fetched samples (skipped, 77, without them), with RGBA8
        # textures, which unoptimized builds cook far faster than ASTC; its cook cache stays between runs, so
        # only the first run cooks. Then its views and camera path against the approved references.
        add_test(NAME maya_r1_assemble COMMAND maya_r1 --compression rgba8 "${MAYA_RENDER_SAMPLES}" "${MAYA_R1_PROJECT}")
        set_tests_properties(maya_r1_assemble PROPERTIES LABELS "cpu;acceptance;r1" TIMEOUT 60 SKIP_RETURN_CODE 77
            FIXTURES_SETUP r1_project)
        add_test(NAME maya_r1_steady COMMAND maya_editor_tests "[r1][steady]")
        set_tests_properties(maya_r1_steady PROPERTIES LABELS "cpu;acceptance;r1" TIMEOUT 600 FIXTURES_REQUIRED r1_project)
        add_test(NAME maya_r1_reference COMMAND maya_editor_tests "[r1][visual]~[precision]")
        set_tests_properties(maya_r1_reference PROPERTIES LABELS "gpu;acceptance;visual;r1" TIMEOUT 900 RUN_SERIAL TRUE
            FIXTURES_REQUIRED r1_project ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        # R1 at 100 m to 100 km from the origin against the origin (#1065, the origin-offset sweep).
        add_test(NAME maya_r1_offsets COMMAND maya_editor_tests "[r1][precision]")
        set_tests_properties(maya_r1_offsets PROPERTIES LABELS "gpu;acceptance;visual;r1;precision" TIMEOUT 1200 RUN_SERIAL TRUE
            FIXTURES_REQUIRED r1_project ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
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
        # So do the shading debug views (#1037); an unknown one is refused.
        add_test(NAME maya_player_debug_view COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_player>|${sample_project}|materials.scene|--debug-view|shading-normals|--smoke|60"
            -DEXIT_CODE=0 "-DEXPECT=showing the shading-normals debug view" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        add_test(NAME maya_player_debug_view_unknown COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_player>|${sample_project}|--debug-view|sepia"
            -DEXIT_CODE=2 "-DEXPECT=--debug-view needs a view" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_player_debug_view maya_player_debug_view_unknown PROPERTIES LABELS "gpu;smoke" TIMEOUT 30 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
    endif()

    # Packages (#1039): packaging on the CPU, and packaged players on Metal, run from outside the checkout.
    if(TARGET maya_package)
        add_executable(maya_package_tests tests/package_tests.cpp)
        target_link_libraries(maya_package_tests PRIVATE MayaPackage MayaRuntime MayaR1 MayaStreaming Catch2::Catch2WithMain z)
        target_include_directories(maya_package_tests PRIVATE tests)
        target_compile_definitions(maya_package_tests PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            MAYA_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}" MAYA_RENDER_SAMPLES="${MAYA_RENDER_SAMPLES}"
            MAYA_PLAYER="$<TARGET_FILE:maya_player>" MAYA_R1_PROJECT="${CMAKE_CURRENT_BINARY_DIR}/r1"
            MAYA_ACCEPTANCE_DIR="${CMAKE_CURRENT_BINARY_DIR}/acceptance")
        add_dependencies(maya_package_tests maya_player)
        maya_target_options(maya_package_tests)
        add_test(NAME maya_package COMMAND maya_package_tests "~[gpu]")
        set_tests_properties(maya_package PROPERTIES LABELS "cpu;package" TIMEOUT 120)
        add_test(NAME maya_package_gpu COMMAND maya_package_tests "[gpu]~[acceptance]")
        set_tests_properties(maya_package_gpu PROPERTIES LABELS "gpu;smoke;package" TIMEOUT 900 RUN_SERIAL TRUE
            FIXTURES_REQUIRED r1_project ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        # Milestone 3 (#1040): the content workflow's scene, packaged and run outside the checkout.
        add_test(NAME maya_content_acceptance_package COMMAND maya_package_tests "[acceptance][content]")
        set_tests_properties(maya_content_acceptance_package PROPERTIES LABELS "gpu;smoke;acceptance" TIMEOUT 300 RUN_SERIAL TRUE
            FIXTURES_REQUIRED content_project ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
    endif()

    # The benchmark runner: its logic on the null device (CPU), and its command line on Metal with
    # the small smoke manifests, which must complete and report results.
    if(TARGET MayaBenchmark)
        add_executable(maya_benchmark_tests tests/benchmark_tests.cpp)
        # The system zlib writes the import workload's test PNGs (tests/support/png.hpp).
        target_link_libraries(maya_benchmark_tests PRIVATE MayaBenchmark Catch2::Catch2WithMain z)
        target_compile_definitions(maya_benchmark_tests PRIVATE MAYA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_include_directories(maya_benchmark_tests PRIVATE tests)
        maya_target_options(maya_benchmark_tests)
        add_test(NAME maya_benchmark COMMAND maya_benchmark_tests "~[gpu]")
        set_tests_properties(maya_benchmark PROPERTIES LABELS "cpu;benchmark" TIMEOUT 120)
        add_test(NAME maya_benchmark_gpu COMMAND maya_benchmark_tests "[gpu]")
        set_tests_properties(maya_benchmark_gpu PROPERTIES LABELS "gpu;benchmark" TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        foreach(manifest sample sample_present materials i1_1k l1_load l1_play)
            add_test(NAME maya_benchmark_smoke_${manifest} COMMAND ${CMAKE_COMMAND}
                "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/smoke/${manifest}.benchmark|${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-${manifest}.json"
                -DEXIT_CODE=0 "-DEXPECT=results: " -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
            set_tests_properties(maya_benchmark_smoke_${manifest} PROPERTIES LABELS "gpu;smoke;benchmark" TIMEOUT 120
                RUN_SERIAL TRUE ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        endforeach()
        # A1 at a glance (#1038), when the samples it plays are there.
        if(EXISTS "${MAYA_RENDER_SAMPLES}/Models/CesiumMan/glTF-Binary/CesiumMan.glb")
            add_test(NAME maya_benchmark_smoke_a1 COMMAND ${CMAKE_COMMAND}
                "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/smoke/a1.benchmark|${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-a1.json"
                -DEXIT_CODE=0 "-DEXPECT=results: " -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
            set_tests_properties(maya_benchmark_smoke_a1 PROPERTIES LABELS "gpu;smoke;benchmark" TIMEOUT 120
                RUN_SERIAL TRUE ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        endif()
        # P1 at a tenth of its size: it must complete, and every run and worker count must end alike (#1024).
        add_test(NAME maya_benchmark_smoke_p1_small COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/p1_small.benchmark|${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-p1_small.json"
            -DEXIT_CODE=0 "-DEXPECT=every run ended in the same state" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_benchmark_smoke_p1_small PROPERTIES LABELS "gpu;smoke;benchmark" TIMEOUT 300
            RUN_SERIAL TRUE ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
        # The stream workload, small (#1064): cells must stream in and out, every load succeeding.
        add_test(NAME maya_benchmark_smoke_stream COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/smoke/stream.benchmark|${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-stream.json"
            -DEXIT_CODE=0 "-DEXPECT=crossings" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_benchmark_smoke_stream PROPERTIES LABELS "cpu;smoke;benchmark;streaming" TIMEOUT 300)
        add_test(NAME maya_benchmark_bad_manifest COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=$<TARGET_FILE:maya_benchmark>|${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
            -DEXIT_CODE=2 "-DEXPECT=expected a maya-benchmark header" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_exit.cmake)
        set_tests_properties(maya_benchmark_bad_manifest PROPERTIES LABELS "cpu;benchmark" TIMEOUT 15)
    endif()

    # The world-scale prototypes (#1060), when built. They time work on every core, so they run alone, and
    # fail with UNEXPECTED lines when what the decision relies on breaks. The LOD half of the visibility
    # prototype reads build/render-samples and build/world-samples where they were fetched.
    if(MAYA_BUILD_PROTOTYPES)
        add_test(NAME maya_prototype_jobs COMMAND maya_jobs_prototype)
        add_test(NAME maya_prototype_precision COMMAND maya_precision_prototype)
        add_test(NAME maya_prototype_precision_double COMMAND maya_precision_prototype_double)
        add_test(NAME maya_prototype_cells COMMAND maya_cells_prototype ${CMAKE_CURRENT_BINARY_DIR}/cells-prototype)
        add_test(NAME maya_prototype_visibility COMMAND maya_visibility_prototype)
        add_test(NAME maya_prototype_terrain COMMAND maya_terrain_prototype)
        set_tests_properties(maya_prototype_jobs maya_prototype_precision maya_prototype_precision_double maya_prototype_cells
            maya_prototype_visibility PROPERTIES LABELS "cpu;prototype" RUN_SERIAL TRUE TIMEOUT 300)
        set_tests_properties(maya_prototype_terrain PROPERTIES LABELS "gpu;prototype" RUN_SERIAL TRUE TIMEOUT 300
            ENVIRONMENT "${MAYA_GPU_TEST_ENVIRONMENT}")
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
