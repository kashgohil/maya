# Maya Engine Context

## Project Overview
Maya is a high-performance 3D game engine targeting realistic graphics, written in **C++20**. It employs a **Rendering Hardware Interface (RHI)** architecture to abstract graphics API details from the core engine logic.

- **Main Technologies:** C++20, Objective-C++ (for Metal), CMake, GLFW.
- **Current Backend:** Metal (macOS native).
- **Architecture:** 
  - `maya::Engine`: Central hub for lifecycle management.
  - `maya::GraphicsDevice`: Abstract RHI base class.
  - `maya::MetalDevice`: Metal-specific implementation of the RHI.
  - `maya::Window`: Platform-agnostic windowing wrapper around GLFW.

## Building and Running
The project uses CMake as its build system.

### Prerequisites
- CMake 3.20+
- AppleClang (supporting C++20 and OBJCXX)
- Homebrew (for dependency management)

### Commands
```bash
# Configure the project
mkdir -p build && cd build
cmake ..

# Build the project
cmake --build .

# Run the engine
./maya
```

## Project Structure
- `include/maya/`: Public header files.
  - `core/`: Engine lifecycle and core systems.
  - `platform/`: Platform abstraction (windowing, input).
  - `rhi/`: Rendering Hardware Interface abstractions and backends.
- `src/maya/`: Implementation files (.cpp, .mm).
- `src/main.cpp`: Application entry point.
- `resources/`: Runtime shaders and other packaged resources (loaded via `FileSystem`).
- `assets/`: Models and similar content referenced by path from the repo root.

## Window size vs framebuffer (Metal)

On Retina displays, the **logical** window size (what you pass to `glfwCreateWindow`) differs from the **framebuffer** size in **pixels** (backing store). Metal’s `CAMetalLayer.drawableSize` must match the framebuffer in pixels, not the logical size.

The engine uses **`glfwGetFramebufferSize`** at startup and **`glfwSetFramebufferSizeCallback`** on resize to drive `GraphicsDevice::resize` and `Camera::set_aspect_ratio`. That keeps the swapchain, depth buffer, and projection matrix aligned with the actual drawable resolution.

## Development Conventions
- **Language:** C++20 for core logic; Objective-C++ (`.mm`) for macOS/Metal specific code.
- **Naming:** `snake_case` for methods and variables; `PascalCase` for classes.
- **RHI Pattern:** All rendering calls must go through the `GraphicsDevice` abstraction to ensure cross-platform compatibility.
- **IDE Support:** 
  - `CMAKE_EXPORT_COMPILE_COMMANDS` is enabled.
  - A symbolic link `compile_commands.json -> build/compile_commands.json` exists in the root for tools like `clangd` and Zed.
  - `.clangd` configuration is provided for enhanced diagnostics.
