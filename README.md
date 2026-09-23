# Maya

Maya is being built as a production engine for realistic 3D games, with rendering and physics as its focus. The current code is the first architectural foundation: separate runtime, desktop host, editor, player, and sample targets. World authoring, physics, scripting, and the new renderer are subsequent milestones.

The current backend is **Metal on macOS**, with **C++20**, **Objective-C++**, **GLFW**, and **CMake**.

## Build and run

Requirements: macOS, Apple Clang/Xcode Command Line Tools, and CMake 3.20+. The initial configure fetches pinned GLFW and Catch2 sources.

```bash
cmake -S . -B build
cmake --build build -j 4

./build/maya_player
./build/maya_editor
./build/maya_sample
```

- `maya_player` runs the basic-scene native sample project. Project selection is in its entry point; loading serialized game projects comes later.
- `maya_editor` opens an empty editor host with an uncaptured cursor. Panels, inspectors, and authoring are not implemented yet.
- `maya_sample` runs the extracted rotating pyramid/cube demo. It replaces the old `maya` executable.
- Player/sample controls: WASD, mouse look, Space to ascend, Escape to exit.
- Window titles show smoothed FPS, frame time, and framebuffer dimensions.

For Release builds, add `-DCMAKE_BUILD_TYPE=Release` to configure. CMake exports `build/compile_commands.json` for clangd.

## Independent targets

| Target | Responsibility | Dependencies |
| --- | --- | --- |
| MayaRuntime / Maya::Runtime | Engine lifecycle, existing core utilities, RHI/Metal | Apple graphics frameworks; no GLFW, editor, or sample code |
| MayaDesktop | Window, event loop, input routing, launch arguments | MayaRuntime, GLFW |
| MayaBasicScene | Sample meshes, camera, materials, and animation | MayaRuntime |
| MayaEditor | Editor application boundary | MayaRuntime |
| maya_player | Standalone player entry point | MayaDesktop, MayaBasicScene |
| maya_editor | Editor entry point | MayaDesktop, MayaEditor |
| maya_sample | Explicit sample entry point | MayaDesktop, MayaBasicScene |

All application targets can be disabled independently. Tests are controlled by `BUILD_TESTING`; Catch2 is not fetched when tests are disabled. GLFW is not fetched when all desktop applications are disabled.

Build only the runtime:

```bash
cmake -S . -B build/runtime-only \
  -DMAYA_BUILD_EDITOR=OFF -DMAYA_BUILD_PLAYER=OFF \
  -DMAYA_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build build/runtime-only --target MayaRuntime
```

Build the player without editor or test dependencies:

```bash
cmake -S . -B build/player-only \
  -DMAYA_BUILD_EDITOR=OFF -DMAYA_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build build/player-only --target maya_player
```

Sources are listed explicitly in CMake so new editor/sample files cannot silently become runtime dependencies.

## Application lifecycle

The desktop host owns the native window. `Engine` owns the graphics device and an `Application` for one session. The application owns game/editor content.

1. Create the window, initialize the device, then call `Application::on_start`.
2. Send framebuffer-pixel dimensions through `Engine::resize`.
3. The host supplies time and input availability to `Engine::tick`: update, begin frame, render, end frame.
4. Call `on_stop` and destroy application content while the device is alive.
5. Shut down the device, retire GPU work, and release its resources before destroying the window.

An initialization failure rolls back acquired state. Once `on_start` has been entered, `on_stop` runs exactly once, even if startup fails or throws. A failed device initialization never starts the application. Frame/resize exceptions terminate the session cleanly and return failure to the host.

Shutdown is idempotent. A stopped or failed engine can start a fresh session; initializing an active engine is rejected. Applications and devices must tolerate destruction before startup, and cleanup must not throw. Lifecycle callbacks must not re-enter the engine. Desktop operations remain on the main thread.

Metal uses an opaque implementation with ARC-owned objects and autorelease pools. Shutdown drains submitted work. Per-resource RHI destruction and per-draw/frame uniform allocation are still separate planned issues; this change does not claim those renderer features are complete.

## Validation

```bash
# CPU unit/lifecycle and CLI tests; no window or GPU initialization
ctest --test-dir build -L cpu --output-on-failure

# Metal tests, repeated desktop lifecycle tests, and five-frame app smoke tests
# Requires an interactive macOS session with GPU access; briefly opens windows
ctest --test-dir build -L gpu --output-on-failure

# Direct CPU suite
./build/maya_tests '~[rhi]'

# Individual bounded sample run
./build/maya_sample --smoke 5
```

All applications accept `--help` and `--smoke [N]`. N must be a positive integer; omitted N defaults to 120. Smoke mode disables camera input/cursor capture and supplies a fixed 1/60-second update interval. It still uses a real window and Metal. It exits nonzero on startup failure or if the requested frames are not completed. Smoke success verifies lifecycle, not pixel correctness.

Run AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug -DMAYA_ENABLE_SANITIZERS=ON
cmake --build build/sanitized -j 4
ctest --test-dir build/sanitized -L cpu --output-on-failure
ctest --test-dir build/sanitized -L gpu --output-on-failure
```

The CPU lifecycle suite injects failures and checks cleanup order, repeated sessions, invalid input, and exception rollback. The desktop suite checks real Metal rollback/reinitialization and overlapping/failed windows.

If AddressSanitizer hangs before test startup, verify the toolchain with an empty sanitized program. A similar macOS startup hang is tracked in [LLVM #200447](https://github.com/llvm/llvm-project/issues/200447). Run UBSan separately with `-DMAYA_SANITIZERS=undefined`; a UBSan pass is not an ASan pass. Sanitizer findings fail the process, and CTest runs have bounded timeouts.

## Content discovery

Sample content lives in `samples/basic_scene/`; shared shader sources remain in `resources/shaders/metal/`. The demo model is `samples/basic_scene/assets/pyramid.obj`.

The desktop host initializes `FileSystem` using these search roots:

1. `MAYA_RESOURCES`, when set.
2. Parents of the executable.
3. The working directory.

Running from the build directory works because its parents include the repository. For binaries copied elsewhere, set `MAYA_RESOURCES=/path/to/maya`, pointing to a tree containing `resources/` and `samples/basic_scene/assets/`. Automatic content cooking/packaging is later work.

Failed file loads print attempted paths. On Retina displays, framebuffer pixel dimensions drive both Metal drawable size and the sample camera aspect ratio.

## Tracking

[Foundation issue #989](https://work.rezee.app/kash/issues/989) is part of [the author-save-run milestone](https://work.rezee.app/kash/issues/988). [DOC-58](https://work.rezee.app/kash/docs/58) records the production-engine direction and subsequent milestones.

[Architecture contracts](docs/architecture/README.md), recorded for [issue #990](https://work.rezee.app/kash/issues/990), define the intended world/asset identity, ownership, coordinate, scheduling, and render-extraction rules. They include proposed performance workloads and a scenario review; planned systems and unconfirmed budgets are distinguished from current implementation.
