#include "basic_scene.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/core/camera.hpp"
#include "maya/core/file_system.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/scene/scene_io.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace maya::samples {
namespace {

constexpr auto camera_id = EntityId{0x6d617961, 0x100};
constexpr auto pyramid_id = EntityId{0x6d617961, 0x200};

/// Loads assets/basic.scene and runs it through the shared renderer: the World is extracted into a
/// snapshot, the camera entity's view renders offscreen, and the view is presented to the window.
class BasicScene final : public Application {
public:
    bool on_start(GraphicsDevice& device) override {
        if (device.surface_format() == Format::undefined) {
            std::cerr << "[BasicScene] requires a presentable surface\n";
            return false;
        }
        auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        if (shader.empty()) return false;
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
        auto opened = open_scene_file(catalog_path->parent_path() / "basic.scene", asset_property_context(*m_assets));
        for (const auto& problem : opened.diagnostics) std::cerr << problem.message << '\n';
        if (!opened) return false;
        m_world = std::move(opened.world);
        const auto camera = m_world->find(camera_id);
        const auto pyramid = m_world->find(pyramid_id);
        if (!camera || !pyramid) { std::cerr << "[BasicScene] scene lacks its camera or pyramid\n"; return false; }
        m_camera = *camera;
        m_pyramid = *pyramid;
        m_world->with<TransformComponent>(m_pyramid, [&](const TransformComponent& value) { m_pyramid_pose = value; });

        // The fly controller is input state; it drives the camera entity's transform.
        m_controller = std::make_unique<Camera>(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
        m_world->with<TransformComponent>(m_camera, [&](const TransformComponent& value) {
            m_controller->set_position(value.translation);
        });
        m_renderer = std::make_unique<Renderer>(device, std::move(shader));
        m_view = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, false, "player view"});
        std::cerr << "[BasicScene] loaded " << m_world->size() << " entities\n";
        return true;
    }

    void on_update(float delta_time, bool input_enabled) override {
        if (input_enabled) m_controller->update(delta_time);
        m_rotation += 0.5f * delta_time;
        auto commands = m_world->commands();
        auto pyramid = m_pyramid_pose;
        pyramid.rotation = math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, m_rotation);
        commands.set_transform(m_pyramid, pyramid);
        if (const auto pose = inverse_affine(m_controller->get_view_matrix()))
            if (const auto transform = decompose_transform(*pose)) commands.set_transform(m_camera, *transform);
        if (const auto result = m_world->commit(commands); !result)
            throw std::runtime_error("[BasicScene] animation commit failed");
    }

    void on_render(GraphicsDevice& device) override {
        const auto surface = device.acquire_surface();
        if (!surface) return; // no drawable or zero-sized window: skip the view this frame
        const auto& target = surface.target;
        if (auto error = m_view->resize(target.width, target.height)) throw std::runtime_error(error.message);
        const auto view = extract_render_view(*m_world, m_camera, target.width, target.height);
        if (!view) throw std::runtime_error("[BasicScene] the camera entity has no valid view");
        const auto snapshot = extract_render_snapshot(*m_world, *m_assets);
        if (snapshot.diagnostics.size() != m_reported) { // report changes, not every frame
            for (const auto& problem : snapshot.diagnostics) std::cerr << "[BasicScene] " << problem.message << '\n';
            m_reported = snapshot.diagnostics.size();
        }
        if (auto error = m_renderer->render(snapshot, *view, *m_view)) throw std::runtime_error(error.message);
        if (auto error = m_renderer->present(*m_view, target.texture, {0, 0, target.width, target.height}))
            throw std::runtime_error(error.message);
    }

    void on_stop() noexcept override {
        // Device shutdown releases anything still pending after these owners are gone.
        m_renderer.reset();
        m_view.reset();
        m_world.reset();
        m_assets.reset();
        m_controller.reset();
        m_rotation = 0.0f;
        m_reported = 0;
    }

private:
    std::unique_ptr<AssetRegistry> m_assets;
    std::unique_ptr<World> m_world;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<RenderTarget> m_view;
    std::unique_ptr<Camera> m_controller;
    EntityHandle m_camera;
    EntityHandle m_pyramid;
    TransformComponent m_pyramid_pose;
    float m_rotation = 0.0f;
    size_t m_reported = 0;
};

} // namespace

std::unique_ptr<Application> create_basic_scene() {
    return std::make_unique<BasicScene>();
}

} // namespace maya::samples
