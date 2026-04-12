# Maya

Small **C++20** 3D engine on **macOS** with a **Metal** rendering backend behind an RHI (`GraphicsDevice`), **GLFW** for windowing, and **CMake** for builds.

## Requirements

- macOS with Apple Clang (C++20, Objective-C++ for Metal)
- CMake 3.20+
- Xcode or Command Line Tools (Metal, Cocoa, QuartzCore)

## Build

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

## Run

From the **build** directory (or anywhere—the engine resolves asset paths from the executable and repo roots):

```bash
./maya
```

Tests:

```bash
./maya_tests
```

Metal-related tests need a normal macOS environment (not a sandbox that blocks GPU access).

## Assets and working directory

Runtime files live under the repository:

- `resources/` — shaders (e.g. `resources/shaders/metal/triangle.metal`)
- `assets/` — models and other data (e.g. `assets/models/pyramid.obj`)

`FileSystem::initialize` (called from `main`) builds a search path from:

1. `MAYA_RESOURCES` — optional directory containing `resources/` and `assets/`
2. Parents of the executable (so `build/maya` finds the repo root)
3. The current working directory

You can run `./maya` from `build/` without manually setting `MAYA_RESOURCES`.

## Notes

- **Framebuffer vs logical size:** On Retina displays, the GLFW framebuffer size in pixels differs from the window’s logical size. The engine uses framebuffer dimensions for Metal’s drawable and for the camera aspect ratio (see `GEMINI.md`).
