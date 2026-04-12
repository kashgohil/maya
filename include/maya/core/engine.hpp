#pragma once

#include "maya/platform/window.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/resource.hpp"
#include "maya/core/camera.hpp"
#include "maya/core/mesh.hpp"
#include "maya/core/texture.hpp"
#include <memory>
#include <vector>

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

    PipelineHandle m_pipeline_textured{};
    PipelineHandle m_pipeline_unlit{};

    std::unique_ptr<Mesh> m_pyramid_mesh;
    std::unique_ptr<Mesh> m_unlit_cube_mesh;
    std::unique_ptr<Texture> m_checker_texture;
    UniformBufferHandle m_uniform_buffer;

    bool m_is_running = false;
};

} // namespace maya
