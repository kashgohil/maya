#include "editor_application.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/core/file_system.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/scene/scene_io.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace maya::editor {
namespace {

constexpr std::array<double, 4> panel_color{0.06, 0.06, 0.07, 1.0};

// Editor-only startup boundary. Until project open/save lands (#1002), the editor opens the sample
// project's scene for viewing. Panels, input routing, and authoring belong to later issues.
class EditorApplication final : public Application {
public:
    bool on_start(GraphicsDevice& device) override {
        auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        if (shader.empty()) return false;
        m_renderer = std::make_unique<Renderer>(device, std::move(shader));
        m_viewport = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, false, "editor viewport"});
        open_sample_scene(device); // an editor without content still starts
        return true;
    }

    void on_render(GraphicsDevice& device) override {
        const auto surface = device.acquire_surface();
        if (!surface) return; // hidden, zero-sized, or no drawable this frame
        const auto& target = surface.target;
        // The viewport leaves a column for the future inspector, so its size differs from the window.
        const auto area = PixelRect{0, 0, std::max(1u, target.width * 3 / 4), target.height};
        auto view = std::optional<RenderView>{};
        if (m_world) view = make_render_view(CameraComponent{}, m_camera_pose, area.width, area.height);
        if (!view) {
            auto pass = RenderPassDesc{};
            pass.colors.push_back({target.texture, LoadAction::clear, StoreAction::store, panel_color});
            pass.label = "editor background";
            if (!device.begin_render_pass(pass)) device.end_render_pass();
            return;
        }
        if (auto error = m_viewport->resize(area.width, area.height)) throw std::runtime_error(error.message);
        const auto snapshot = extract_render_snapshot(*m_world, *m_assets);
        if (auto error = m_renderer->render(snapshot, *view, *m_viewport)) throw std::runtime_error(error.message);
        if (auto error = m_renderer->present(*m_viewport, target.texture, area, panel_color))
            throw std::runtime_error(error.message);
    }

    void on_stop() noexcept override {
        m_renderer.reset();
        m_viewport.reset();
        m_world.reset();
        m_assets.reset();
    }

private:
    void open_sample_scene(GraphicsDevice& device) {
        const auto catalog_path = FileSystem::resolve("samples/basic_scene/assets/catalog.maya");
        if (!catalog_path) {
            std::cerr << "[Editor] sample project not found; showing an empty viewport\n";
            return;
        }
        auto catalog_file = std::ifstream(*catalog_path);
        const auto catalog = read_asset_catalog(catalog_file);
        if (!catalog) { std::cerr << "[Editor] " << catalog.diagnostic.message << '\n'; return; }
        auto assets = std::make_unique<AssetRegistry>(catalog_path->parent_path(),
            std::make_unique<FileAssetProvider>(device));
        for (const auto& record : catalog.records)
            if (const auto error = assets->register_asset(record)) {
                std::cerr << "[Editor] " << error.message << '\n';
                return;
            }
        auto opened = open_scene_file(catalog_path->parent_path() / "basic.scene", asset_property_context(*assets));
        for (const auto& problem : opened.diagnostics) std::cerr << "[Editor] " << problem.message << '\n';
        if (!opened) return;
        m_assets = std::move(assets);
        m_world = std::move(opened.world);
    }

    std::unique_ptr<AssetRegistry> m_assets;
    std::unique_ptr<World> m_world;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<RenderTarget> m_viewport;
    // The editor camera is tool state, not an entity: it views the World without changing it.
    math::Mat4 m_camera_pose = *inverse_affine(math::Mat4::look_at({3.0f, 2.2f, 4.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
};

} // namespace

std::unique_ptr<Application> create_editor_application() {
    return std::make_unique<EditorApplication>();
}

} // namespace maya::editor
