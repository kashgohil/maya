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
```

GPU tests require an interactive macOS session. Applications support `--smoke [positive frame count]`. Use `MAYA_ENABLE_SANITIZERS=ON` in a separate build directory for ASan/UBSan.

## Boundaries

- `MayaAssets` / `Maya::Assets`: Project catalog, typed asset handles, shared version leases, and the initial OBJ/material providers. See [docs/assets.md](docs/assets.md) for loading and GPU ownership boundaries.
- `MayaWorld` / `Maya::World`: CPU-only entity identity, packed component storage, and atomic structural command batches. No RHI, window, or editor dependency. See [docs/world.md](docs/world.md) for APIs and scoped-borrow rules.
- `MayaScene` / `Maya::Scene`: Versioned scene files, detached scene documents, validated loading into a new World, and atomic saves. Links only MayaWorld; asset references are checked through `asset_property_context(registry)`. See [docs/scene.md](docs/scene.md).
- `MayaRHI` / `Maya::RHI`: GraphicsDevice validation, per-session handles, deferred retirement, and `NullGraphicsDevice` for CPU tests. The Metal backend lives in MayaRuntime. See [docs/rhi.md](docs/rhi.md).
- `MayaRenderer` / `Maya::Renderer`: Render snapshot extraction from a World, camera views, offscreen color/depth targets, the lit pass, and presentation into a window or panel. Reads Worlds; never edits them. See [docs/renderer.md](docs/renderer.md).
- `MayaRuntime`: Engine session lifecycle, existing core utilities, and graphics backend. No editor, sample, GLFW, or desktop-loop dependency.
- `MayaDesktop`: Window ownership, event loop, input events and window metrics, cursor capture, clipboard and cursor services, framebuffer resize, and CLI launch handling.
- `MayaEditor`: Editor shell on Dear ImGui (`MayaImGui`, editor-only): docked hierarchy/viewport/inspector/assets/diagnostics panels, an editor camera, and UI/viewport input routing. See [docs/editor.md](docs/editor.md). Authoring comes later.
- `MayaImGui`: Dear ImGui core sources, fetched only with `MAYA_BUILD_EDITOR`. Runtime, desktop, player, and sample must never link it; the `*_no_editor_ui` CTest checks enforce this.
- `MayaBasicScene`: Opens the sample project's scene file, drives its camera and animation through World commands, and renders it through the renderer.
- `apps/player/main.cpp`: Selects the initial native sample project for the player.
- `apps/editor/main.cpp`: Launches the editor.
- `samples/basic_scene/main.cpp`: Launches the sample directly.

The host owns the window; Engine owns device and Application. Stop and destroy application content before device shutdown, then destroy the window. Shutdown is idempotent, startup failures roll back, and stopped engines can initialize a fresh session. Callbacks must not re-enter lifecycle methods.

Mesh, Texture, and the fly-camera `Camera` controller are prototype utilities retained pending their dedicated replacement issues; the legacy Scene and Material were removed by #998. World authoring, physics, and scripting are not implemented yet.

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
- `samples/basic_scene/assets/`: Sample catalog, `basic.scene`, meshes, and material files.
- `resources/shaders/metal/renderer.metal`: Lit pass and view presentation shaders.
- `resources/shaders/metal/editor_ui.metal`: Editor UI (Dear ImGui) shader.
- `apps/editor/`: Editor shell, UI renderer, input router, and editor camera.

FileSystem searches MAYA_RESOURCES, executable parents, and the working directory. A resource root for the sample contains both resources/ and samples/basic_scene/assets/. Failed resolution logs every candidate path.

CMake exports build/compile_commands.json; the root symlink supports clangd.
