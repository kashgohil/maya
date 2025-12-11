#include "maya/core/engine.hpp"
#include "maya/core/application.hpp"
#include "maya/platform/input.hpp"
#include <chrono>
#include <iostream>

namespace maya {

Engine &Engine::instance() {
  static Engine s_instance;
  return s_instance;
}

bool Engine::initialize(const EngineConfig &config) {
  std::cout << "[Engine] Initializing Maya Engine...\n";
  std::cout << "[Engine] Application: " << config.app_name << "\n";

  WindowConfig window_config;
  window_config.title = config.app_name;
  window_config.width = config.window_width;
  window_config.height = config.window_height;
  window_config.fullscreen = config.fullscreen;
  window_config.resizable = true;

  if (!m_window.create(window_config)) {
    std::cerr << "[Engine] Failed to create window!\n";
    return false;
  }

  Input::instance().init(m_window.get_native_handle());

  m_last_frame_time = get_time();
  m_running = true;

  std::cout << "[Engine] Engine initialized successfully\n";
  return true;
}

void Engine::shutdown() {
  std::cout << "[Engine] Shutting down Maya Engine...\n";

  m_window.destroy();
  m_running = false;

  std::cout << "[Engine] Engine shutdown complete\n";
}

void Engine::run(Application *app) {
  if (!app) {
    std::cerr << "[Engine] Error: No application provided!\n";
    return;
  }

  m_application = app;

  if (!m_application->on_initialize()) {
    std::cerr << "[Engine] Error: Application initialization failed!\n";
    return;
  }

  std::cout << "[Engine] Starting main loop...\n";

  while (m_running && !m_window.should_close()) {
    m_window.poll_events();
    Input::instance().update();

    f64 current_time = get_time();
    m_delta_time = static_cast<f32>(current_time - m_last_frame_time);
    m_last_frame_time = current_time;

    update(m_delta_time);
    render();
  }

  m_application->on_shutdown();
  std::cout << "[Engine] Main loop ended\n";
}

f64 Engine::get_time() const {
  using namespace std::chrono;
  auto now = high_resolution_clock::now();
  auto dur = now.time_since_epoch();
  return duration_cast<std::chrono::duration<f64>>(dur).count();
}

void Engine::update(f32 delta_time) {
  if (m_application) {
    m_application->on_update(delta_time);
  }
}

void Engine::render() {
  if (m_application) {
    m_application->on_render();
  }
}

} // namespace maya
