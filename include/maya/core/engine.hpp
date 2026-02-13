#pragma once

#include "maya/platform/window.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/core/camera.hpp"
#include <memory>

namespace maya {

class Engine {
public:
    Engine();
    ~Engine();

    bool initialize();
    void run();
    void shutdown();

private:
    std::unique_ptr<Window> m_window;
    std::unique_ptr<GraphicsDevice> m_graphics_device;
    std::unique_ptr<Camera> m_camera;
    bool m_is_running = false;
};

} // namespace maya
