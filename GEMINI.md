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
- `MayaRuntime`: Engine session lifecycle, existing core utilities, and graphics backend. No editor, sample, GLFW, or desktop-loop dependency.
- `MayaDesktop`: Window ownership, event loop, input, framebuffer resize, and CLI launch handling.
- `MayaEditor`: Editor-only application factory. Initially an empty host; authoring UI comes later.
- `MayaBasicScene`: Demo content and animation, owned by the sample project.
- `apps/player/main.cpp`: Selects the initial native sample project for the player.
- `apps/editor/main.cpp`: Launches the editor.
- `samples/basic_scene/main.cpp`: Launches the sample directly.

The host owns the window; Engine owns device and Application. Stop and destroy application content before device shutdown, then destroy the window. Shutdown is idempotent, startup failures roll back, and stopped engines can initialize a fresh session. Callbacks must not re-enter lifecycle methods.

Current Scene, Material, Mesh, and Camera are prototype utilities retained pending their dedicated replacement issues. The application split does not implement world authoring, a new renderer, physics, or scripting.

World storage and the initial Name/Transform/MeshRenderer/Camera/Light schemas exist independently of those sample utilities. Structural edits use WorldCommands and explicit commit; never retain a component reference outside a query callback. Hierarchy and camera calculations are implemented; transform queries are read-only and edits use validated set_transform/reparent commands. Asset registry/residency services are implemented by #993. Shared [property metadata and validated edits](docs/properties.md) are implemented by #994; use this boundary for authoring/import/script values. Native non-transform component writes remain trusted. [Scene save/load](docs/scene.md) is implemented by #995: load into a separate World and replace the active one only on success. World renderer integration follows in dependent issues. See [spatial APIs and tolerances](docs/spatial.md).

## Conventions

- C++20 in core/platform, Objective-C++ confined to the Metal backend.
- snake_case methods/variables; PascalCase classes.
- All rendering calls go through GraphicsDevice.
- Explicit CMake source lists keep runtime, editor, and sample dependencies separate.
- Native Metal state stays opaque to C++ consumers and uses ARC ownership.
- Use framebuffer pixel dimensions, not logical window size, for Metal and camera aspect.
- Runtime cleanup must not throw; application stop must tolerate partial initialization.

## Files

- `include/maya/core/application.hpp`: Content lifecycle boundary.
- `include/maya/core/engine.hpp`: Runtime session owner.
- `src/maya/platform/desktop_application.cpp`: Desktop loop.
- `samples/basic_scene/assets/pyramid.obj`: Sample model.
- `resources/shaders/metal/triangle.metal`: Existing shared demo shader.

FileSystem searches MAYA_RESOURCES, executable parents, and the working directory. A resource root for the sample contains both resources/ and samples/basic_scene/assets/. Failed resolution logs every candidate path.

CMake exports build/compile_commands.json; the root symlink supports clangd.
