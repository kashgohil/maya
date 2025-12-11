#pragma once

#include "maya/math/types.hpp"
#include "maya/platform/window.hpp"

namespace maya {

class Application;

struct EngineConfig {
    const char* app_name = "Maya Application";
    u32 window_width = 1280;
    u32 window_height = 720;
    bool fullscreen = false;
    bool vsync = true;
    bool enable_validation = true; // For Vulkan debug layers
};

class Engine {
public:
    static Engine& instance();

    bool initialize(const EngineConfig& config);
    void shutdown();

    void run(Application* app);

    bool is_running() const { return m_running; }
    void quit() { m_running = false; }

    f64 get_time() const;
    f32 get_delta_time() const { return m_delta_time; }

    Window& get_window() { return m_window; }
    const Window& get_window() const { return m_window; }

private:
    Engine() = default;
    ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void update(f32 delta_time);
    void render();

    bool m_running = false;
    f32 m_delta_time = 0.0f;
    f64 m_last_frame_time = 0.0;

    Window m_window;
    Application* m_application = nullptr;
};

} // namespace maya
