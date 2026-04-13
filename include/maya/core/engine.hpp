#pragma once

#include "maya/platform/window.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/resource.hpp"
#include "maya/core/camera.hpp"
#include "maya/core/scene.hpp"
#include "maya/core/texture.hpp"
#include <memory>

namespace maya {

class Engine {
public:
    Engine();
    ~Engine();

    bool initialize();
    void run();
    void run_for_frames(uint32_t frames);
    void shutdown();

private:
    void run_impl(bool enable_input_capture, uint32_t max_frames);

    std::unique_ptr<Window> m_window;
    std::unique_ptr<GraphicsDevice> m_graphics_device;
    std::unique_ptr<Camera> m_camera;

    Scene m_scene;
    DirectionalLighting m_directional_light = DirectionalLighting::default_sun();
    std::unique_ptr<Texture> m_checker_texture;
    UniformBufferHandle m_uniform_buffer;

    bool m_is_running = false;
};

} // namespace maya
