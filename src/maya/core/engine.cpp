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
    math::Mat4 view_projection_matrix;
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
    std::string shader_source = FileSystem::read_text("src/maya/rhi/metal/triangle.metal");
    if (shader_source.empty()) {
        std::cerr << "Failed to load shader file!" << std::endl;
        return false;
    }

    if (!m_graphics_device->create_pipeline(shader_source)) {
        return false;
    }

    // Define square vertices
    std::vector<Vertex> vertices = {
        { math::Vec3(-0.5f,  0.5f, 0.0f), math::Vec4(1.0f, 0.0f, 0.0f, 1.0f) }, // 0: Top-Left
        { math::Vec3( 0.5f,  0.5f, 0.0f), math::Vec4(0.0f, 1.0f, 0.0f, 1.0f) }, // 1: Top-Right
        { math::Vec3( 0.5f, -0.5f, 0.0f), math::Vec4(0.0f, 0.0f, 1.0f, 1.0f) }, // 2: Bottom-Right
        { math::Vec3(-0.5f, -0.5f, 0.0f), math::Vec4(1.0f, 1.0f, 0.0f, 1.0f) }  // 3: Bottom-Left
    };

    // Correct CCW Indices for 2 triangles
    std::vector<uint32_t> indices = {
        0, 3, 2, // Triangle 1
        2, 1, 0  // Triangle 2
    };

    if (!m_graphics_device->create_vertex_buffer(vertices.data(), vertices.size() * sizeof(Vertex))) {
        return false;
    }

    if (!m_graphics_device->create_index_buffer(indices.data(), indices.size() * sizeof(uint32_t))) {
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
        
        // 1. Model Matrix: Rotate around Z
        uniforms.model_matrix = math::Mat4::rotate_z(current_rotation);

        // 2. View/Proj: Identity (Pass-through 2D)
        uniforms.view_projection_matrix = math::Mat4::identity();

        m_graphics_device->update_uniform_buffer(&uniforms, sizeof(UniformData));

        m_graphics_device->begin_frame();
        m_graphics_device->draw_indexed(6);
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
