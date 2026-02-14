#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include "maya/core/model_loader.hpp"
#include "maya/platform/input.hpp"
#include "maya/rhi/vertex.hpp"
#include "maya/math/matrix.hpp"
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
        std::cerr << "Failed to create window native handle" << std::endl;
        return false;
    }

    m_graphics_device = GraphicsDevice::create_default();
    if (!m_graphics_device->initialize(m_window->get_native_handle())) {
        std::cerr << "Failed to initialize graphics device" << std::endl;
        return false;
    }

    m_camera = std::make_unique<Camera>(60.0f, 1280.0f / 720.0f, 0.1f, 100.0f);
    m_camera->set_position(math::Vec3(0.0f, 0.0f, 3.0f));

    std::string shader_source = FileSystem::read_text("src/maya/rhi/metal/triangle.metal");
    if (shader_source.empty()) {
        std::cerr << "Failed to read shader source" << std::endl;
        return false;
    }
    if (!m_graphics_device->create_pipeline(shader_source)) {
        std::cerr << "Failed to create pipeline" << std::endl;
        return false;
    }

    m_cube_mesh = ModelLoader::load_obj(*m_graphics_device, "assets/models/pyramid.obj");
    if (!m_cube_mesh) {
        std::cerr << "Failed to load pyramid model" << std::endl;
        return false;
    }

    uint32_t checkerboard[] = { 0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFFFFFFFF };
    m_checker_texture = std::make_unique<Texture>(*m_graphics_device, checkerboard, 2, 2);

    m_uniform_buffer = m_graphics_device->create_uniform_buffer(sizeof(UniformData));

    m_is_running = true;
    return true;
}

void Engine::run() {
    float rotation_speed = 0.5f;
    float current_rotation = 0.0f;
    double last_time = glfwGetTime();

    glfwSetInputMode(m_window->get_glfw_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();
        Input& input = Input::instance();
        if (input.is_key_pressed(KeyCode::Escape)) m_is_running = false;

        double current_time = glfwGetTime();
        float delta_time = static_cast<float>(current_time - last_time);
        last_time = current_time;

        m_camera->update(delta_time);
        current_rotation += rotation_speed * delta_time;
        
        UniformData uniforms;
        math::Mat4 rotZ = math::Mat4::rotate_z(current_rotation);
        math::Mat4 rotX = math::Mat4::rotate_x(current_rotation * 0.5f);
        uniforms.model_matrix = rotZ * rotX;
        uniforms.view_projection_matrix = m_camera->get_view_projection_matrix();

        m_graphics_device->update_uniform_buffer(m_uniform_buffer, &uniforms, sizeof(UniformData));

        m_graphics_device->begin_frame();
        
        m_graphics_device->bind_uniform_buffer(m_uniform_buffer, 1);
        m_checker_texture->bind(0);
        m_cube_mesh->draw();

        m_graphics_device->end_frame();
        input.update();
    }
}

void Engine::shutdown() {
    if (m_graphics_device) m_graphics_device->shutdown();
    m_is_running = false;
}

} // namespace maya