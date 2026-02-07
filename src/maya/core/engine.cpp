#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/input.hpp"
#include "maya/rhi/vertex.hpp"
#include "maya/math/matrix.hpp"
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>

namespace maya {

struct UniformData {
    math::Mat4 model_matrix;
};

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

    // Define triangle vertices
    std::vector<Vertex> vertices = {
        { math::Vec3(0.0f,  0.5f, 0.0f), math::Vec4(1.0f, 0.0f, 0.0f, 1.0f) },
        { math::Vec3(-0.5f, -0.5f, 0.0f), math::Vec4(0.0f, 1.0f, 0.0f, 1.0f) },
        { math::Vec3(0.5f,  -0.5f, 0.0f), math::Vec4(0.0f, 0.0f, 1.0f, 1.0f) }
    };

    if (!m_graphics_device->create_vertex_buffer(vertices.data(), vertices.size() * sizeof(Vertex))) {
        return false;
    }

    // Create uniform buffer
    if (!m_graphics_device->create_uniform_buffer(sizeof(UniformData))) {
        return false;
    }

    m_is_running = true;
    return true;
}

void Engine::run() {
    float rotation_speed = 1.0f;
    float current_rotation = 0.0f;
    double last_time = glfwGetTime();

    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();

        // Handle Input
        Input& input = Input::instance();
        
        if (input.is_key_pressed(KeyCode::Escape)) {
            m_is_running = false;
        }

        if (input.is_key_down(KeyCode::Space)) {
            rotation_speed = 0.0f; // Pause on space
        } else {
            rotation_speed = 1.0f;
        }

        // Calculate Delta Time
        double current_time = glfwGetTime();
        float delta_time = static_cast<float>(current_time - last_time);
        last_time = current_time;

        // Animate
        current_rotation += rotation_speed * delta_time;
        
        UniformData uniforms;
        uniforms.model_matrix = math::Mat4::rotate_z(current_rotation);

        m_graphics_device->update_uniform_buffer(&uniforms, sizeof(UniformData));

        m_graphics_device->begin_frame();
        m_graphics_device->draw_triangle();
        m_graphics_device->end_frame();

        // Finalize input state for the next frame
        input.update();
    }
}

void Engine::shutdown() {
    if (m_graphics_device) {
        m_graphics_device->shutdown();
    }
    m_is_running = false;
}

} // namespace maya
