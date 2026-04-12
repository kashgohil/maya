# Maya

Small **C++20** 3D engine on **macOS** with a **Metal** rendering backend behind an RHI (`GraphicsDevice`), **GLFW** for windowing, and **CMake** for builds.

**Platform:** macOS only (Metal + Objective-C++). There is no Vulkan/D3D12 backend yet.

## Requirements

- macOS with Apple Clang (C++20, Objective-C++ for Metal)
- CMake 3.20+
- Xcode or Command Line Tools (Metal, Cocoa, QuartzCore)

## Quick start

Clone and build from the repository root:

```bash
git clone <your-repo-url>
cd maya    # use your clone directory name
mkdir -p build && cd build
cmake ..
cmake --build .
```

Run the sample (from `build/` is fine):

```bash
./maya
```

Run the unit tests:

```bash
./maya_tests
```

Metal-related tests expect a normal macOS environment with GPU access. If tests fail in a **sandboxed** terminal or CI without Metal, run them locally in Terminal.app or grant the environment GPU access.

While the app is running, the **window title** shows a lightweight debug readout: smoothed FPS, last-frame time, framebuffer size, camera position, and indexed draw-call count for the current frame.

## Build (reference)

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

Release build:

```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

IDE integration: the project sets `CMAKE_EXPORT_COMPILE_COMMANDS`; symlink or copy `build/compile_commands.json` to the repo root for `clangd` (see `GEMINI.md`).

## Assets and working directory

Runtime files live under the repository:

- `resources/` — shaders (e.g. `resources/shaders/metal/triangle.metal`)
- `assets/` — models and other data (e.g. `assets/models/pyramid.obj`)

`FileSystem::initialize` (called from `main`) builds a search path from:

1. `MAYA_RESOURCES` — optional directory containing `resources/` and `assets/`
2. Parents of the executable (so `build/maya` finds the repo root)
3. The current working directory

You can run `./maya` from `build/` without manually setting `MAYA_RESOURCES`.

Example when installing or copying only the `build/` tree elsewhere:

```bash
export MAYA_RESOURCES=/path/to/maya
./maya
```

That directory should contain `resources/` and `assets/` as in the repo.

### Troubleshooting

- **Shader or model failed to load:** Check stderr. Failed relative paths print `[FileSystem]` lines listing search roots and each candidate path tried.
- **Tests pass locally but fail in a sandbox:** Run `./maya_tests` outside the sandbox (Metal shader compilation needs a full macOS GPU stack in practice).
- **Wrong aspect ratio after resize:** The engine uses GLFW framebuffer size; on Retina, that is the backing-store size in pixels (see `GEMINI.md`).

## CI

On push and pull requests to `master` or `main`, [GitHub Actions](.github/workflows/ci.yml) configures CMake, builds Release, and runs `maya_tests` on `macos-latest` (Metal available). You can also run the workflow manually from the Actions tab (`workflow_dispatch`).

## Notes

- **Framebuffer vs logical size:** On Retina displays, the GLFW framebuffer size in pixels differs from the window’s logical size. The engine uses framebuffer dimensions for Metal’s drawable and for the camera aspect ratio (see `GEMINI.md`).
