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

    // Initialize Camera
    // FOV: 60 deg, Aspect: 16:9, Near: 0.1, Far: 100.0
    m_camera = std::make_unique<Camera>(60.0f, 1280.0f / 720.0f, 0.1f, 100.0f);
    m_camera->set_position(math::Vec3(0.0f, 0.0f, 3.0f));

    // Load shader from file
    std::string shader_source = FileSystem::read_text("src/maya/rhi/metal/triangle.metal");
    if (shader_source.empty()) {
        std::cerr << "Failed to load shader file!" << std::endl;
        return false;
    }

    if (!m_graphics_device->create_pipeline(shader_source)) {
        return false;
    }

    // Define Cube Vertices (8 corners, distinct colors)
    std::vector<Vertex> vertices = {
        // Front Face
        { math::Vec3(-0.5f,  0.5f,  0.5f), math::Vec4(1.0f, 0.0f, 0.0f, 1.0f) }, // Top-Left (Red)
        { math::Vec3( 0.5f,  0.5f,  0.5f), math::Vec4(0.0f, 1.0f, 0.0f, 1.0f) }, // Top-Right (Green)
        { math::Vec3( 0.5f, -0.5f,  0.5f), math::Vec4(0.0f, 0.0f, 1.0f, 1.0f) }, // Bottom-Right (Blue)
        { math::Vec3(-0.5f, -0.5f,  0.5f), math::Vec4(1.0f, 1.0f, 0.0f, 1.0f) }, // Bottom-Left (Yellow)
        
        // Back Face
        { math::Vec3(-0.5f,  0.5f, -0.5f), math::Vec4(0.0f, 1.0f, 1.0f, 1.0f) }, // Top-Left (Cyan)
        { math::Vec3( 0.5f,  0.5f, -0.5f), math::Vec4(1.0f, 0.0f, 1.0f, 1.0f) }, // Top-Right (Magenta)
        { math::Vec3( 0.5f, -0.5f, -0.5f), math::Vec4(1.0f, 1.0f, 1.0f, 1.0f) }, // Bottom-Right (White)
        { math::Vec3(-0.5f, -0.5f, -0.5f), math::Vec4(0.5f, 0.5f, 0.5f, 1.0f) }  // Bottom-Left (Grey)
    };

    // Cube Indices (12 triangles)
    std::vector<uint32_t> indices = {
        // Front
        0, 3, 2, 2, 1, 0,
        // Back
        5, 6, 7, 7, 4, 5,
        // Left
        4, 7, 3, 3, 0, 4,
        // Right
        1, 2, 6, 6, 5, 1,
        // Top
        4, 0, 1, 1, 5, 4,
        // Bottom
        3, 7, 6, 6, 2, 3
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
    float rotation_speed = 0.5f; // Rad/s
    float current_rotation = 0.0f;
    double last_time = glfwGetTime();

    // Disable cursor for camera control
    glfwSetInputMode(m_window->get_glfw_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();

        // Handle Input
        Input& input = Input::instance();
        
        if (input.is_key_pressed(KeyCode::Escape)) {
            m_is_running = false;
        }

        // Calculate Delta Time
        double current_time = glfwGetTime();
        float delta_time = static_cast<float>(current_time - last_time);
        last_time = current_time;

        // Update Camera
        m_camera->update(delta_time);

        // Animate Cube
        current_rotation += rotation_speed * delta_time;
        
        UniformData uniforms;
        
        // 1. Model Matrix: Rotate around X and Z
        math::Mat4 rotZ = math::Mat4::rotate_z(current_rotation);
        math::Mat4 rotX = math::Mat4::rotate_x(current_rotation * 0.5f);
        uniforms.model_matrix = rotZ * rotX;

        // 2. View/Proj
        uniforms.view_projection_matrix = m_camera->get_view_projection_matrix();

        m_graphics_device->update_uniform_buffer(&uniforms, sizeof(UniformData));

        m_graphics_device->begin_frame();
        m_graphics_device->draw_indexed(36);
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