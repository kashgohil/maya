# Maya Engine Context

Maya is a C++20 engine under architectural redevelopment for realistic 3D games. The current platform is macOS/Metal with Objective-C++, GLFW, and CMake. See README.md for build options, lifecycle contracts, and known limitations.

Before implementing world, asset, simulation, or renderer changes, read [docs/architecture/README.md](docs/architecture/README.md) and its linked contracts from #990. They distinguish intended architecture from current APIs and leave product/platform/performance decisions explicitly open. Update the relevant record when changing a contract.

## Build and validation

```bash
cmake -S . -B build
cmake --build build -j 4
./build/maya_player
./build/maya_editor
./build/maya_sample
ctest --test-dir build -L cpu --output-on-failure
ctest --test-dir build -L gpu --output-on-failure
tools/check_milestone.sh build   # CPU, headless GPU, and windowed groups; reports what could not run
```

GPU tests require an interactive macOS session. Applications support `--smoke [positive frame count]`. Use `MAYA_ENABLE_SANITIZERS=ON` in a separate build directory for ASan/UBSan.

## Boundaries

- `MayaAssets` / `Maya::Assets`: Project catalog, typed asset handles, shared version leases, the initial OBJ/material/texture providers, and project files (`project.hpp`: content root, catalog, startup scene). See [docs/assets.md](docs/assets.md) for loading and GPU ownership boundaries. Resolve project content through `Project::resolve`, never the working directory or FileSystem search roots ([docs/projects.md](docs/projects.md)).
- `MayaTextures` / `Maya::Textures` (#1031): texture descriptors (`.texture`), Maya's own KTX2 reader and writer, and cooking source images (stb_image, stb_image_resize2, astcenc; all three private to it). MayaAssets links it to cook PNG and JPEG sources at load. See [docs/assets.md](docs/assets.md#textures). Normal maps are stored as x in RGB and y in alpha; never write a KTX2 subset beyond what `ktx validate` accepts (`maya_ktx2_validate` with `MAYA_KTX_TOOL`).
- `MayaWorld` / `Maya::World`: CPU-only entity identity, packed component storage, and atomic structural command batches. No RHI, window, or editor dependency. See [docs/world.md](docs/world.md) for APIs and scoped-borrow rules.
- `MayaScene` / `Maya::Scene`: Versioned scene files, detached scene documents, validated loading into a new World, and atomic saves. Links only MayaWorld; asset references are checked through `asset_property_context(registry)`. See [docs/scene.md](docs/scene.md).
- `MayaRHI` / `Maya::RHI`: GraphicsDevice validation, per-session handles, deferred retirement, and `NullGraphicsDevice` for CPU tests. The Metal backend lives in MayaRuntime. See [docs/rhi.md](docs/rhi.md).
- `MayaRenderer` / `Maya::Renderer`: Render snapshot extraction from a World, camera views, offscreen color/depth targets, the lit pass, and presentation into a window or panel. Reads Worlds; never edits them. See [docs/renderer.md](docs/renderer.md).
- `MayaRuntime`: Engine session lifecycle, existing core utilities, and graphics backend. No editor, sample, GLFW, or desktop-loop dependency.
- `MayaDesktop`: Window ownership, event loop, input events and window metrics, cursor capture, clipboard, cursor, and close-request services, window closes the application can defer (`Application::on_close_requested`), framebuffer resize, and CLI launch handling.
- `MayaEditor`: Editor shell on Dear ImGui (`MayaImGui`, editor-only): docked hierarchy/viewport/inspector/assets/diagnostics panels, an editor camera, and UI/viewport input routing. See [docs/editor.md](docs/editor.md). `SceneEditor` owns the open scene's World; make every editor change through it so it is undoable ([docs/editing.md](docs/editing.md)). Edits are recorded as before/after records of the touched entities only; never keep runtime handles in history. Continuous controls (drags, gizmos) wrap their frames in `begin_group`/`end_group`; typed values apply when entry ends ([docs/inspector.md](docs/inspector.md)). ImGuizmo (MIT) is editor-only and pinned to `22a6c900`. Projects, scene open/save, the unsaved-changes prompt, and the Assets panel are in `project_files.cpp` ([docs/projects.md](docs/projects.md)); ask before replacing a dirty scene (`request_open_scene`, `request_new_scene`, `request_close`). Play mode is in `play_mode.cpp`: it locks the SceneEditor and runs a separate `PlaySession`; the input router's third owner is the game.
- `MayaImGui`: Dear ImGui core sources, fetched only with `MAYA_BUILD_EDITOR`. Runtime, desktop, player, and sample must never link it; the `*_no_editor_ui` CTest checks enforce this.
- `MayaSimulation` / `Maya::Simulation`: Play sessions: the fixed clock, gameplay input frames, ordered `SimulationSystem`s, and the session's physics world, on a World built from a scene document. CPU-only; shared by the player and editor play ([docs/play.md](docs/play.md)). Gameplay writes go through the tick's commands; never touch the authored scene from play.
- `MayaMetrics` / `Maya::Metrics`: Nearest-rank summaries, sample windows, `Stopwatch`, and `FrameTiming`. CPU-only. Engine reports `FrameTiming` to `Application::on_frame_timing`.
- `maya_benchmark` (`MayaBenchmark`, `apps/benchmark/`): headless benchmark runner for `benchmarks/*.benchmark` manifests; JSON results with raw samples ([docs/performance.md](docs/performance.md)). `present on` opens a window and measures display pacing (#1026); compare presenting runs only with presenting runs. Report measured values as observations; never invent budgets. Keep tracked bytes and platform-reported memory apart; GPU time, per frame and per pass, comes only from `take_gpu_timings()`. Give new render passes a `RenderPassDesc::label`: it names their GPU time in the editor and in results. The `physics` workload (P1, `physics_workload.cpp`) is headless; changing its recipe makes a new version ([performance-baseline.md](docs/architecture/performance-baseline.md#p1-physics-stress)). Benchmark only on a cool machine.
- Milestone acceptance ([docs/acceptance.md](docs/acceptance.md)): `tests/acceptance_tests.cpp` authors a project in `build/acceptance` that `maya_acceptance_player` runs with the real player, and `tests/physics_acceptance_tests.cpp` does the same for milestone 2's reference interaction (`maya_physics_acceptance_player`); V1 reference images live in `tests/references/v1` (re-bless with `MAYA_BLESS_REFERENCES=1` and inspect them before committing).
- `MayaPhysics` / `Maya::Physics`: Rigid bodies on Jolt Physics 5.6.0 ([docs/physics.md](docs/physics.md), [decision](docs/architecture/physics-scripting-decision.md)). A play session owns one `PhysicsWorld` and builds the scene's authored bodies at start, all or none; systems make requests through `TickContext::bodies` and read `TickContext::physics`. Only `src/maya/physics/` includes Jolt (`maya_library_headers` checks it); never expose Jolt types or body IDs. Physics writes kinematic and dynamic bodies' transforms: move them with forces, velocities, kinematic targets, or teleports, never `set_transform`. Jolt's hooks, types, and job pool are process-wide (`set_physics_worker_threads`).
- Recording and replay (#1023, [docs/play.md](docs/play.md#recording-and-replay)): `PlayRecording` holds the tick-indexed input (`InputTrack`, changes only), seed, scene text, asset hashes, build (`maya/core/build_info.hpp`, in `MayaBuildInfo`), Jolt configuration, reloads, and a `state_hash` checkpoint every 60 ticks. `replay_refusal` refuses other builds, physics, changed assets, and reloads; never replay a refused recording. Anything new that affects the simulation must feed the state hash or the event/message trace, and replay tests must keep content free of `pairs` order over table, function, or userdata keys.
- Debug views (#1022, [docs/physics.md](docs/physics.md#debug-views), [docs/renderer.md](docs/renderer.md#debug-lines)): producers fill a `DebugDraw` (lines and box/sphere/capsule outlines, `MayaWorld`), passed through `RenderExtractOptions::debug`; an empty one must cost nothing (no pipelines, uploads, or draws). Physics outlines come from collider descriptions (`play_physics_debug` from the physics world at shown poses, `authored_physics_debug` from components), not Jolt's `DebugRenderer`. Contacts and queries are captured only while `PhysicsWorld::set_debug_capture` is on, and capture must never change results. Editor preferences (`editor_preferences.hpp`) are per person, never in project files. The player draws debug views only with `--debug-physics`.
- Presentation (#1016): play views draw `PlaySession::presentation()` (poses between the last two ticks, as `PresentationPoses`) through `RenderExtractOptions::poses` and `extract_render_view`. Phase 2 keeps each changed entity's previous local transform as the batch commits; teleports, reparenting, and spawns reset it. Presentation never writes the World; compose hierarchy from shown poses, never blend matrices.
- Physics events and queries (#1021): Jolt's contact listener only appends records; `PhysicsWorld::take_events` turns them into sorted per-pair events in phase 7, and systems read them in `SimulationSystem::late_fixed_update` (`TickContext::events`). Queries (`raycast`, `shape_cast`, `overlap`) run against the last completed step and sort by distance, then EntityId. Body requests made in phase 7 apply at the next step; never let phase 7 change the completed one.
- Scripting (in `MayaSimulation`, `src/maya/simulation/scripting/`): Luau 0.740 scripts attached by `maya.script` components ([docs/scripting.md](docs/scripting.md)). One sandboxed VM per play session with a memory limit and a deterministic work budget per hook call (a project file may set both: `script_work`, `script_memory`); `play_systems(sources)` gives the built-in systems plus the script system. Script changes go through the tick's commands and are validated like Inspector edits; a failing hook call is rolled back, reported through `PlayFrame::messages`, and stops only that instance. Only the scripting folder includes Luau (`maya_library_headers` checks it); never expose Luau types in public headers. The editor (`script_files.cpp`) watches script files, keeps each script's last good version for the Inspector and Play, and hands changed scripts to a playing session through `ScriptReloads`; the player never watches files.
- `MayaPlayer`: Runs a project's saved scene through a play session (`create_player_application`); the player and the sample both use it. No sample-specific C++: scene behavior is data (`maya.spin`, `maya.fly_control`, and scripts through `maya.script`).
- `apps/player/main.cpp`: Launches the player; takes an optional project and scene, `--record file` or `--replay file`, and `--debug-physics`.
- `apps/editor/main.cpp`: Launches the editor; takes an optional project file or folder.
- `samples/basic_scene/main.cpp`: Launches the player on the sample project.

The host owns the window; Engine owns device and Application. Stop and destroy application content before device shutdown, then destroy the window. Shutdown is idempotent, startup failures roll back, and stopped engines can initialize a fresh session. Callbacks must not re-enter lifecycle methods.

`Mesh` and `Texture`/`Sampler` (`core/texture.hpp`) own the GPU resources behind MeshAsset and TextureAsset; the fly-camera `Camera` controller is a prototype utility retained pending its replacement issue; the legacy Scene and Material were removed by #998. Physics bodies come from `maya.collider`, `maya.rigid_body`, and `maya.physics_settings` components at Play start (#1019, `authored_physics`) or from code (#1017). Scripting is not implemented yet; only its #1015 prototypes exist.

World storage and the initial Name/Transform/MeshRenderer/Camera/Light schemas exist independently of those sample utilities. Structural edits use WorldCommands and explicit commit; never retain a component reference outside a query callback. Hierarchy and camera calculations are implemented; transform queries are read-only and edits use validated set_transform/reparent commands. Asset registry/residency services are implemented by #993. Shared [property metadata and validated edits](docs/properties.md) are implemented by #994; use this boundary for authoring/import/script values. Native non-transform component writes remain trusted. [Scene save/load](docs/scene.md) is implemented by #995: load into a separate World and replace the active one only on success. The [renderer](docs/renderer.md) (#998) extracts immutable snapshots from a World and renders camera views into offscreen targets; applications never issue draws from World data themselves. See [spatial APIs and tolerances](docs/spatial.md).

## Conventions

- C++20 in core/platform, Objective-C++ confined to the Metal backend.
- snake_case methods/variables; PascalCase classes.
- All rendering calls go through GraphicsDevice: explicit render passes inside Engine's frame, surface acquired only for presentation, resources destroyed through the device (never assume Metal retains them), and per-draw constants uploaded with `upload_transient` rather than rewritten in a shared buffer.
- Keep GPU struct layouts in `include/maya/renderer/shader_constants.hpp` and `include/maya/rhi/vertex.hpp` in step with the shader; Metal `float3` is 16 bytes, `packed_float3` is 12.
- Explicit CMake source lists keep runtime, editor, and sample dependencies separate.
- Native Metal state stays opaque to C++ consumers and uses ARC ownership.
- Use framebuffer pixel dimensions, not logical window size, for Metal and camera aspect. Input positions and UI layout are in points; convert with `WindowMetrics::scale()`.
- Editor input goes through `InputRouter`; never read held keys from the `Input` singleton in editor tools, or typing will leak into them.
- Runtime cleanup must not throw; application stop must tolerate partial initialization.

## Files

- `include/maya/core/application.hpp`: Content lifecycle boundary.
- `include/maya/core/engine.hpp`: Runtime session owner.
- `src/maya/platform/desktop_application.cpp`: Desktop loop.
- `samples/basic_scene/assets/`: Sample catalog, `basic.scene`, meshes, material files, and two textures (a tile grid and its normal map).
- `resources/shaders/metal/renderer.metal`: Lit pass and view presentation shaders.
- `resources/shaders/metal/editor_ui.metal`: Editor UI (Dear ImGui) shader, and the Assets panel's texture thumbnails.
- `resources/fonts/`: Inter and Geist Mono (SIL OFL) and Phosphor Light icons (MIT), licenses alongside, for the editor UI. Add icons as named constants in `apps/editor/editor_icons.hpp` so they enter the font atlas. Use the palette and helpers in `apps/editor/editor_theme.hpp` for new editor UI instead of raw ImGui colors.
- `apps/editor/`: Editor shell, UI renderer, input router, and editor camera.

FileSystem searches MAYA_RESOURCES, executable parents, and the working directory. A resource root for the sample contains both resources/ and samples/basic_scene/assets/. Failed resolution logs every candidate path.

CMake exports build/compile_commands.json; the root symlink supports clangd.
