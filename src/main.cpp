#include "maya/core/application.hpp"
#include "maya/core/engine.hpp"
#include "maya/platform/input.hpp"
#include <iostream>

using namespace maya;

class SampleApp : public Application {
public:
  bool on_initialize() override {
    std::cout << "[App] Sample application initialized\n";
    std::cout << "[App] Press WASD to move, ESC to quit, SPACE to jump\n";
    std::cout << "[App] Click left mouse button to interact\n\n";

    return true;
  }

  void on_shutdown() override {
    std::cout << "\n[App] Sample application shutdown\n";
  }

  void on_update(f32 delta_time) override {
    frame_count++;

    Input &input = Input::instance();

    // Check for ESC to quit
    if (input.is_key_pressed(Key::Escape)) {
      std::cout << "[App] ESC pressed - quitting...\n";
      Engine::instance().quit();
      return;
    }

    // WASD movement
    if (input.is_key_down(Key::W)) {
      position.z += speed * delta_time;
      if (frame_count % 30 == 0) {
        std::cout << "[App] Moving forward - Position: " << position.x << ", "
                  << position.y << ", " << position.z << "\n";
      }
    }
    if (input.is_key_down(Key::S)) {
      position.z -= speed * delta_time;
    }
    if (input.is_key_down(Key::A)) {
      position.x -= speed * delta_time;
    }
    if (input.is_key_down(Key::D)) {
      position.x += speed * delta_time;
    }

    // Space to jump
    if (input.is_key_pressed(Key::Space)) {
      std::cout << "[App] JUMP! Current height: " << position.y << "\n";
      position.y += 2.0f;
    }

    // Gravity
    if (position.y > 0.0f) {
      position.y -= gravity * delta_time;
      if (position.y < 0.0f)
        position.y = 0.0f;
    }

    // Mouse input
    if (input.is_mouse_button_pressed(MouseButton::Left)) {
      Vec2 mouse_pos = input.get_mouse_position();
      std::cout << "[App] Left click at: " << mouse_pos.x << ", " << mouse_pos.y
                << "\n";
    }

    Vec2 mouse_delta = input.get_mouse_delta();
    if (mouse_delta.x != 0.0f || mouse_delta.y != 0.0f) {
      if (frame_count % 60 == 0) {
        std::cout << "[App] Mouse moved: delta(" << mouse_delta.x << ", "
                  << mouse_delta.y << ")\n";
      }
    }

    f32 scroll = input.get_scroll_delta();
    if (scroll != 0.0f) {
      std::cout << "[App] Scroll: " << scroll << "\n";
    }

    // Print FPS every 60 frames
    if (frame_count % 60 == 0) {
      std::cout << "[App] Frame: " << frame_count
                << " | FPS: " << static_cast<int>(1.0f / delta_time)
                << " | Position: (" << position.x << ", " << position.y << ", "
                << position.z << ")\n";
    }
  }

  void on_render() override {}

  void on_resize(u32 width, u32 height) override {
    std::cout << "[App] Window resized to: " << width << "x" << height << "\n";
  }

private:
  u32 frame_count = 0;
  Vec3 position{0.0f, 0.0f, 0.0f};
  f32 speed = 5.0f;
  f32 gravity = 9.8f;
};

int main() {
  std::cout << "================================\n";
  std::cout << "Maya Game Engine - Phase 2 Demo\n";
  std::cout << "================================\n\n";

  EngineConfig config;
  config.app_name = "Maya Phase 2 - Platform Layer";
  config.window_width = 1280;
  config.window_height = 720;
  config.fullscreen = false;
  config.vsync = true;

  Engine &engine = Engine::instance();

  if (!engine.initialize(config)) {
    std::cerr << "Failed to initialize engine!\n";
    return 1;
  }

  SampleApp app;
  engine.run(&app);

  engine.shutdown();

  return 0;
}
