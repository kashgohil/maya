#include "basic_scene.hpp"
#include "maya/core/camera.hpp"
#include "maya/core/file_system.hpp"
#include "maya/core/mesh.hpp"
#include "maya/assets/registry.hpp"
#include <fstream>
#include "maya/core/primitives.hpp"
#include "maya/core/scene.hpp"
#include "maya/core/texture.hpp"
#include <iostream>

namespace maya::samples {
namespace {

class BasicScene final : public Application {
public:
    bool on_start(GraphicsDevice& device) override {
        m_camera = std::make_unique<Camera>(60.0f, 1280.0f / 720.0f, 0.1f, 100.0f);
        m_camera->set_position({0.0f, 0.0f, 3.0f});
        const auto source = FileSystem::read_text("resources/shaders/metal/triangle.metal");
        if (source.empty()) return false;
        const auto textured = device.create_pipeline(source);
        const auto unlit = device.create_pipeline(source, "vertexMain", "fragmentUnlit");
        if (textured.handle == INVALID_HANDLE || unlit.handle == INVALID_HANDLE) return false;

        const auto catalog_path = FileSystem::resolve("samples/basic_scene/assets/catalog.maya");
        if (!catalog_path) return false;
        auto catalog_file = std::ifstream(*catalog_path);
        const auto catalog = read_asset_catalog(catalog_file);
        if (!catalog) { std::cerr << catalog.diagnostic.message << '\n'; return false; }
        m_assets = std::make_unique<AssetRegistry>(catalog_path->parent_path(),
            std::make_unique<FileAssetProvider>(device));
        for (const auto& record : catalog.records) {
            const auto error = m_assets->register_asset(record);
            if (error) { std::cerr << error.message << '\n'; return false; }
        }
        auto pyramid = m_assets->acquire(AssetRef<MeshAsset>{{0x6d617961,1}});
        if (!pyramid) std::cerr << pyramid.diagnostic.message << '\n';
        auto cube = make_color_cube(device, 0.35f, {1.0f, 1.0f, 1.0f});
        if (!pyramid || !cube) return false;
        const uint32_t checkerboard[] = {0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFFFFFFFF};
        m_texture = std::make_unique<Texture>(device, checkerboard, 2, 2);
        m_uniform_buffer = device.create_uniform_buffer(sizeof(SceneDrawUniforms));
        if (m_uniform_buffer.handle == INVALID_HANDLE) return false;
        m_scene.add_object(std::move(pyramid.lease), Material{textured, m_texture.get()});
        m_scene.add_object(std::move(cube), Material{unlit, nullptr});
        std::cerr << "[BasicScene] loaded pyramid and cube\n";
        return true;
    }

    void on_update(float delta_time, bool input_enabled) override {
        if (input_enabled) m_camera->update(delta_time);
        m_rotation += 0.5f * delta_time;
        auto& objects = m_scene.objects();
        objects[0].model_matrix = math::Mat4::rotate_z(m_rotation)
            * math::Mat4::rotate_x(m_rotation * 0.5f);
        objects[1].model_matrix = math::Mat4::translate({-1.35f, 0.0f, 0.0f})
            * math::Mat4::rotate_y(m_rotation * 0.35f);
    }

    void on_render(GraphicsDevice& device) override {
        m_scene.render(device, m_uniform_buffer, m_camera->get_view_projection_matrix(),
            DirectionalLighting::default_sun(), m_camera->get_position());
    }

    void on_resize(uint32_t width, uint32_t height) override {
        m_camera->set_aspect_ratio(static_cast<float>(width) / static_cast<float>(height));
    }

    void on_stop() noexcept override {
        m_scene = Scene{};
        m_assets.reset();
        m_texture.reset();
        m_camera.reset();
        m_uniform_buffer = {};
        m_rotation = 0.0f;
    }

private:
    std::unique_ptr<AssetRegistry> m_assets;
    Scene m_scene;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Texture> m_texture;
    UniformBufferHandle m_uniform_buffer;
    float m_rotation = 0.0f;
};

} // namespace

std::unique_ptr<Application> create_basic_scene() {
    return std::make_unique<BasicScene>();
}

} // namespace maya::samples
