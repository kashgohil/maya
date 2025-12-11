# Maya Game Engine

A modern 3D game engine written in C++20 with built-in AI capabilities, designed for high-performance game development.

## Project Structure

```
maya/
├── src/                    # Source code
│   ├── main.cpp           # Entry point
│   └── maya/              # Engine implementation
│       ├── core/          # Core engine systems (Engine, Application)
│       ├── renderer/      # Rendering system (Vulkan/OpenGL/Metal)
│       ├── math/          # Math library (Vec2/3/4, Mat4, Quat, utilities)
│       ├── platform/      # Platform abstraction (Window, Input)
│       ├── ai/            # AI systems (behavior trees, ML inference)
│       └── physics/       # Physics simulation
├── include/maya/          # Public headers
├── build/                 # CMake build files (generated)
├── bin/                   # Output binaries (generated)
├── libs/                  # Third-party libraries
├── examples/              # Example projects
├── tests/                 # Unit tests
└── CMakeLists.txt         # CMake build configuration
```

## Requirements

- CMake 3.20 or later
- C++20 compatible compiler:
  - GCC 10+ (Linux)
  - Clang 10+ (macOS/Linux)
  - MSVC 2019+ (Windows)

## Building

### Quick Start
```bash
# Configure and build
mkdir build && cd build
cmake ..
cmake --build .

# Run the engine
./bin/maya
```

### Build Types
```bash
# Debug build (default)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release build (optimized)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Release with debug info
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
```

### Build Options
```bash
# Disable examples
cmake -DMAYA_BUILD_EXAMPLES=OFF ..

# Disable tests
cmake -DMAYA_BUILD_TESTS=OFF ..

# Use OpenGL instead of Vulkan
cmake -DMAYA_USE_VULKAN=OFF ..
```

## Architecture

### Core Systems

**Engine**: Main engine loop, time management, subsystem coordination

**Application**: Base class for user applications with lifecycle callbacks

**Math Library**: Industry-standard vector/matrix/quaternion operations

### Design Principles

- Modern C++20 features (concepts, modules in future)
- Header-only where appropriate, compiled libraries for performance
- Minimal dependencies, modular architecture
- Industry-standard conventions (column-major matrices, right-handed coords)
- Zero-cost abstractions

## Features

### Core ✅
- [x] Modern C++20 codebase
- [x] CMake build system
- [x] Engine loop with delta time
- [x] Math library (Vec2/3/4, Mat4, Quat)
- [x] Project structure

### Rendering (Planned)
- [ ] Vulkan backend (primary)
- [ ] OpenGL backend (fallback)
- [ ] Metal backend (macOS)
- [ ] PBR material system
- [ ] Deferred rendering pipeline
- [ ] Shadow mapping (CSM)
- [ ] Post-processing effects

### Platform (Planned)
- [ ] GLFW integration for windowing
- [ ] Cross-platform input handling
- [ ] File system abstraction
- [ ] Multi-threading support

### Physics (Planned)
- [ ] Rigid body dynamics
- [ ] Collision detection (SAT, GJK)
- [ ] Constraint solver
- [ ] Integration with Jolt Physics

### AI (Planned)
- [ ] Behavior tree system
- [ ] Hierarchical FSM
- [ ] Navigation mesh pathfinding
- [ ] ONNX Runtime integration for ML inference
- [ ] Neural network support for:
  - Dynamic NPC behavior
  - Procedural animation
  - Adaptive difficulty
  - Content generation

### Advanced Features (Future)
- [ ] Entity Component System (ECS)
- [ ] Scene graph management
- [ ] Asset pipeline with hot-reloading
- [ ] Scripting integration (Lua/Python)
- [ ] Profiling and debugging tools
- [ ] Editor interface

## Dependencies

### Current
- Standard C++ library (C++20)
- CMake (build system)

### Planned
- **Graphics**: Vulkan SDK, GLFW for windowing
- **Math**: GLM (optional, for compatibility)
- **Physics**: Jolt Physics Engine
- **AI/ML**: ONNX Runtime (C++ API)
- **Assets**: stb_image, Assimp, tinygltf
- **Audio**: OpenAL Soft or miniaudio

## Development Roadmap

### Phase 1: Foundation ✅ COMPLETED
- [x] C++ project structure
- [x] CMake build system
- [x] Basic engine loop
- [x] Math library (vectors, matrices, quaternions)
- [x] Core abstractions (Engine, Application)

### Phase 2: Platform Layer (Current)
- [ ] GLFW window integration
- [ ] Input system implementation
- [ ] File I/O abstraction
- [ ] Platform-specific utilities

### Phase 3: Rendering
- [ ] Vulkan initialization and setup
- [ ] Render pass architecture
- [ ] Basic mesh rendering
- [ ] Camera system
- [ ] Material system
- [ ] Texture loading

### Phase 4: Core Gameplay
- [ ] ECS implementation
- [ ] Physics integration (Jolt)
- [ ] Scene management
- [ ] Transform hierarchy
- [ ] Lighting system

### Phase 5: AI & Advanced Features
- [ ] Behavior tree framework
- [ ] ONNX Runtime integration
- [ ] Navigation system
- [ ] ML model inference
- [ ] Procedural systems

## Learning Resources

### C++ for Game Development
- "Effective Modern C++" by Scott Meyers
- "Game Programming Patterns" by Robert Nystrom
- [CppCon talks](https://www.youtube.com/user/CppCon)

### Game Engine Architecture
- "Game Engine Architecture" by Jason Gregory (industry bible)
- "Real-Time Rendering" 4th Edition
- [Vulkan Tutorial](https://vulkan-tutorial.com/)
- [LearnOpenGL](https://learnopengl.com/)

### AI & Machine Learning in Games
- "Programming Game AI by Example" by Mat Buckland
- "Artificial Intelligence for Games" by Ian Millington
- [ONNX Runtime Documentation](https://onnxruntime.ai/)

### Industry Resources
- [GDC Vault](https://gdcvault.com/)
- [Digital Foundry Tech Analysis](https://www.eurogamer.net/digitalfoundry)
- [GPU Open](https://gpuopen.com/)

## Career Path

This project is designed to build skills for AAA game development:
- Master modern C++ (industry standard)
- Understand low-level graphics APIs (Vulkan/DX12)
- Learn engine architecture patterns
- Explore AI/ML integration in games
- Build a strong portfolio piece

## License

MIT License

## Contributing

This is a learning project focused on building game engine skills for industry employment. Feel free to explore, experiment, and learn alongside!
