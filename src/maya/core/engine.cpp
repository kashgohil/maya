#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include "maya/core/model_loader.hpp"
#include "maya/core/primitives.hpp"
#include "maya/platform/input.hpp"
#include "maya/rhi/vertex.hpp"
#include "maya/math/matrix.hpp"
#include <GLFW/glfw3.h>
#include <iomanip>
#include <iostream>
#include <sstream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace maya {

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
        std::cerr << "Failed to read shader source (see [FileSystem] messages above for search paths).\n";
        return false;
    }

    const PipelineHandle pipeline_textured = m_graphics_device->create_pipeline(shader_source);
    if (pipeline_textured.handle == INVALID_HANDLE) {
        std::cerr << "Failed to create textured pipeline" << std::endl;
        return false;
    }

    const PipelineHandle pipeline_unlit = m_graphics_device->create_pipeline(shader_source, "vertexMain", "fragmentUnlit");
    if (pipeline_unlit.handle == INVALID_HANDLE) {
        std::cerr << "Failed to create unlit pipeline" << std::endl;
        return false;
    }

    auto pyramid = ModelLoader::load_obj(*m_graphics_device, "assets/models/pyramid.obj");
    if (!pyramid) {
        std::cerr << "Failed to load pyramid model.\n";
        return false;
    }

    auto unlit_cube = make_color_cube(*m_graphics_device, 0.35f, math::Vec3(1.0f, 1.0f, 1.0f));
    if (!unlit_cube) {
        std::cerr << "Failed to create unlit cube mesh.\n";
        return false;
    }

    uint32_t checkerboard[] = { 0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFFFFFFFF };
    m_checker_texture = std::make_unique<Texture>(*m_graphics_device, checkerboard, 2, 2);

    m_uniform_buffer = m_graphics_device->create_uniform_buffer(sizeof(SceneDrawUniforms));

    m_scene.add_object(std::move(pyramid), Material{pipeline_textured, m_checker_texture.get()});
    m_scene.add_object(std::move(unlit_cube), Material{pipeline_unlit, nullptr});

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

        const math::Mat4 vp = m_camera->get_view_projection_matrix();

        math::Mat4 rotZ = math::Mat4::rotate_z(current_rotation);
        math::Mat4 rotX = math::Mat4::rotate_x(current_rotation * 0.5f);
        const math::Mat4 pyramid_model = rotZ * rotX;

        const float cube_rot = current_rotation * 0.35f;
        const math::Mat4 cube_model = math::Mat4::translate(math::Vec3(-1.35f, 0.0f, 0.0f))
            * math::Mat4::rotate_y(cube_rot);

        std::vector<SceneObject>& objects = m_scene.objects();
        if (objects.size() >= 2) {
            objects[0].model_matrix = pyramid_model;
            objects[1].model_matrix = cube_model;
        }

        m_graphics_device->begin_frame();
        m_scene.render(*m_graphics_device, m_uniform_buffer, vp, m_directional_light,
            m_camera->get_position());
        m_graphics_device->end_frame();
        input.update();

        const uint32_t draw_calls = m_scene.drawable_count();

        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(m_window->get_glfw_window(), &fb_w, &fb_h);
        const math::Vec3& pos = m_camera->get_position();
        std::ostringstream title;
        title << std::fixed << std::setprecision(1)
              << "Maya | " << fps_smooth << " fps | " << (delta_time * 1000.0f) << " ms | "
              << fb_w << "x" << fb_h << " | draws " << draw_calls << " | cam "
              << std::setprecision(2) << pos.x << ", " << pos.y << ", " << pos.z;
        m_window->set_title(title.str());
    }
}

void Engine::shutdown() {
    if (m_graphics_device) m_graphics_device->shutdown();
    m_is_running = false;
}

} // namespace maya
