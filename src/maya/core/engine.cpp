#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include "maya/core/model_loader.hpp"
#include "maya/platform/input.hpp"
#include "maya/rhi/vertex.hpp"
#include "maya/math/matrix.hpp"
#include <GLFW/glfw3.h>
#include <iomanip>
#include <iostream>
#include <sstream>
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

static_assert(sizeof(Vertex) == 64, "Vertex struct size mismatch! Metal shader expects 64 bytes due to float4 alignment.");

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

    m_window->set_framebuffer_resize_callback([this](int width, int height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        m_graphics_device->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        m_camera->set_aspect_ratio(static_cast<float>(width) / static_cast<float>(height));
    });
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(m_window->get_glfw_window(), &fb_w, &fb_h);
    if (fb_w > 0 && fb_h > 0) {
        m_graphics_device->resize(static_cast<uint32_t>(fb_w), static_cast<uint32_t>(fb_h));
        m_camera->set_aspect_ratio(static_cast<float>(fb_w) / static_cast<float>(fb_h));
    }

    std::string shader_source = FileSystem::read_text("resources/shaders/metal/triangle.metal");
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
    float fps_smooth = 0.0f;

    glfwSetInputMode(m_window->get_glfw_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();
        Input& input = Input::instance();
        if (input.is_key_pressed(KeyCode::Escape)) m_is_running = false;

        double current_time = glfwGetTime();
        float delta_time = static_cast<float>(current_time - last_time);
        last_time = current_time;

        if (delta_time > 0.0f) {
            float inst_fps = 1.0f / delta_time;
            fps_smooth = (fps_smooth <= 0.0f) ? inst_fps : (fps_smooth * 0.92f + inst_fps * 0.08f);
        }

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

        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(m_window->get_glfw_window(), &fb_w, &fb_h);
        const math::Vec3& pos = m_camera->get_position();
        std::ostringstream title;
        title << std::fixed << std::setprecision(1)
              << "Maya | " << fps_smooth << " fps | " << (delta_time * 1000.0f) << " ms | "
              << fb_w << "x" << fb_h << " | cam "
              << std::setprecision(2) << pos.x << ", " << pos.y << ", " << pos.z;
        m_window->set_title(title.str());
    }
}

void Engine::shutdown() {
    if (m_graphics_device) m_graphics_device->shutdown();
    m_is_running = false;
}

} // namespace maya
