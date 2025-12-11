# Getting Started with Maya Engine

## Prerequisites

1. C++20 compatible compiler
2. CMake 3.20 or later
3. Git (for version control)

## Your First Application

The engine is built around the `Application` class. Here's a minimal example:

```cpp
#include "maya/core/engine.hpp"
#include "maya/core/application.hpp"
#include <iostream>

using namespace maya;

class MyGame : public Application {
public:
    bool on_initialize() override {
        std::cout << "Game initialized!\n";
        return true;
    }

    void on_update(f32 delta_time) override {
        // Update game logic
    }

    void on_render() override {
        // Render your game
    }

    void on_shutdown() override {
        std::cout << "Game shutdown!\n";
    }
};

int main() {
    EngineConfig config;
    config.app_name = "My Game";
    config.window_width = 1920;
    config.window_height = 1080;

    Engine& engine = Engine::instance();

    if (!engine.initialize(config)) {
        return 1;
    }

    MyGame game;
    engine.run(&game);
    engine.shutdown();

    return 0;
}
```

## Math Library Usage

The engine includes a complete math library:

```cpp
#include "maya/math/vector.hpp"
#include "maya/math/matrix.hpp"
#include "maya/math/quaternion.hpp"

using namespace maya;

// Vectors
Vec3 position(1.0f, 2.0f, 3.0f);
Vec3 direction(0.0f, 1.0f, 0.0f);
Vec3 velocity = direction * 5.0f;

f32 length = velocity.length();
Vec3 normalized = velocity.normalized();

// Matrix operations
Mat4 translation = Mat4::translate(Vec3(10.0f, 0.0f, 0.0f));
Mat4 rotation = Mat4::rotate(3.14159f / 4.0f, Vec3(0.0f, 1.0f, 0.0f));
Mat4 scale = Mat4::scale(Vec3(2.0f, 2.0f, 2.0f));

Mat4 transform = translation * rotation * scale;

// Camera matrices
Mat4 view = Mat4::look_at(
    Vec3(0.0f, 0.0f, 5.0f),  // eye
    Vec3(0.0f, 0.0f, 0.0f),  // target
    Vec3(0.0f, 1.0f, 0.0f)   // up
);

Mat4 projection = Mat4::perspective(
    1.57f,   // fov (90 degrees)
    16.0f/9.0f,  // aspect ratio
    0.1f,    // near plane
    100.0f   // far plane
);

// Quaternions for rotation
Quat rot = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), 1.57f);
Vec3 rotated = rot.rotate(Vec3(1.0f, 0.0f, 0.0f));

// Smooth rotation interpolation
Quat from = Quat::from_euler(0.0f, 0.0f, 0.0f);
Quat to = Quat::from_euler(1.57f, 0.0f, 0.0f);
Quat interpolated = Quat::slerp(from, to, 0.5f);
```

## Project Structure

When adding new features:

1. **Headers**: Add public APIs to `include/maya/`
2. **Implementation**: Add .cpp files to `src/maya/`
3. **Update CMakeLists.txt**: Add new source files to the build

Example:
```cmake
# In src/CMakeLists.txt
set(MAYA_SOURCES
    maya/core/engine.cpp
    maya/myfeature/myfile.cpp  # Add your new file
    # ...
)
```

## Next Steps

1. Integrate GLFW for real windowing (Phase 2)
2. Add Vulkan renderer initialization (Phase 3)
3. Implement ECS system (Phase 4)
4. Add AI systems (Phase 5)

## Tips for Learning

- Study the existing math library implementation
- Read "Game Engine Architecture" by Jason Gregory
- Check out [learnopengl.com](https://learnopengl.com) for graphics concepts
- Explore the Vulkan Tutorial for modern graphics API understanding
- Join game dev communities (r/gamedev, gamedev.net)

## Common Patterns

### Singleton Pattern
Used for engine subsystems:
```cpp
Renderer& renderer = Renderer::instance();
renderer.initialize(config);
```

### Time Management
```cpp
void on_update(f32 delta_time) {
    f32 speed = 10.0f;
    position += velocity * delta_time * speed;
}
```

### Resource Management
Always use RAII principles:
```cpp
class Texture {
public:
    Texture() = default;
    ~Texture() { destroy(); }

    // Delete copy, allow move
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = default;
    Texture& operator=(Texture&&) = default;
};
```

Happy coding!
