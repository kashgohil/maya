#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/vertex.hpp"
#include <iostream>
#include <vector>

namespace maya {

Engine::Engine() = default;
Engine::~Engine() {
    shutdown();
}

bool Engine::initialize() {
    m_window = std::make_unique<Window>(1280, 720, "Maya Engine - RHI Metal Backend");
    if (!m_window->get_native_handle()) {
        return false;
    }

    m_graphics_device = GraphicsDevice::create_default();
    if (!m_graphics_device->initialize(m_window->get_native_handle())) {
        return false;
    }

    // Load shader from file
    std::string shader_source = FileSystem::read_text("../src/maya/rhi/metal/triangle.metal");
    if (shader_source.empty()) {
        std::cerr << "Failed to load shader file!" << std::endl;
        return false;
    }

    if (!m_graphics_device->create_pipeline(shader_source)) {
        return false;
    }

    // Define triangle vertices using math library
    std::vector<Vertex> vertices = {
        { math::Vec3(0.0f,  0.5f, 0.0f), math::Vec4(1.0f, 0.0f, 0.0f, 1.0f) },
        { math::Vec3(-0.5f, -0.5f, 0.0f), math::Vec4(0.0f, 1.0f, 0.0f, 1.0f) },
        { math::Vec3(0.5f,  -0.5f, 0.0f), math::Vec4(0.0f, 0.0f, 1.0f, 1.0f) }
    };

    if (!m_graphics_device->create_vertex_buffer(vertices.data(), vertices.size() * sizeof(Vertex))) {
        return false;
    }

    m_is_running = true;
    return true;
}

void Engine::run() {
    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();

        m_graphics_device->begin_frame();
        m_graphics_device->draw_triangle();
        m_graphics_device->end_frame();
    }
}

void Engine::shutdown() {
    if (m_graphics_device) {
        m_graphics_device->shutdown();
    }
    m_is_running = false;
}

} // namespace maya
