#include "maya/simulation/script_assets.hpp"
#include "player_application.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/input.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/simulation/play_session.hpp"
#include <iostream>
#include <stdexcept>

namespace maya::player {
namespace {

class PlayerApplication final : public Application {
public:
    explicit PlayerApplication(PlayerOptions options) : m_options(std::move(options)) {}

    bool on_start(GraphicsDevice& device) override {
        if (device.surface_format() == Format::undefined) return fail("a presentable window surface is required");
        auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        if (shader.empty()) return fail("the renderer shader was not found (resources/shaders/metal/renderer.metal)");
        if (!m_options.project) m_options.project = FileSystem::resolve("samples/basic_scene/project.maya");
        if (!m_options.project) return fail("no project given, and the sample project was not found");
        auto opened = open_project(*m_options.project);
        if (!opened) return fail(opened.error);
        const auto& project = opened.project;
        auto assets = open_project_assets(project, std::make_unique<FileAssetProvider>(device));
        if (!assets) return fail(assets.error);
        m_assets = std::move(assets.registry);

        const auto scene_path = m_options.scene ? project.resolve(*m_options.scene) : project.startup_scene;
        if (!scene_path) {
            return fail(m_options.scene ? m_options.scene->generic_string() + " is outside the project's content root"
                                        : project.file.string() + " has no startup scene; name a scene to run");
        }
        const auto context = asset_property_context(*m_assets);
        auto loaded = load_scene_file(*scene_path, context);
        if (!loaded) return fail(*scene_path, loaded.diagnostics);
        auto started = PlaySession::start(std::move(loaded.document), context, play_systems(registry_script_sources(*m_assets), project_script_settings(project.settings)));
        if (!started) return started.diagnostics.empty() ? fail(started.error) : fail(*scene_path, started.diagnostics);
        m_session = std::move(started.session);
        if (!m_session->camera()) return fail(scene_path->filename().string() + " has no camera to show; add one in the editor");

        m_renderer = std::make_unique<Renderer>(device, std::move(shader));
        m_view = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, false, "player view"});
        std::cerr << "[Player] " << project.name() << " / " << project.relative(*scene_path).generic_string() << ": "
                  << m_session->world().size() << " entities\n";
        return true;
    }

    void on_update(float delta_time, bool input_enabled) override {
        // Every window event belongs to the game in the player.
        if (input_enabled) m_session->input().feed(Input::instance().events());
        const auto frame = m_session->update(delta_time);
        for (const auto& message : frame.messages) // script logs and failed script instances
            std::cerr << "[" << message.source << "] " << message.text << '\n';
        if (!frame.error.empty()) throw std::runtime_error("[Player] " + frame.error);
    }

    void on_render(GraphicsDevice& device) override {
        const auto surface = device.acquire_surface();
        if (!surface) return; // no drawable or zero-sized window: skip the view this frame
        const auto& target = surface.target;
        const auto& world = m_session->world();
        const auto camera = world.find(*m_session->camera());
        if (!camera) throw std::runtime_error("[Player] the camera entity no longer exists");
        if (auto error = m_view->resize(target.width, target.height)) throw std::runtime_error(error.message);
        // Poses between the last two ticks, so motion is smooth at any display rate.
        const auto poses = m_session->presentation();
        const auto view = extract_render_view(world, *camera, target.width, target.height, &poses);
        if (!view) throw std::runtime_error("[Player] the camera entity has no valid view");
        auto options = RenderExtractOptions{};
        options.poses = &poses;
        const auto snapshot = extract_render_snapshot(world, *m_assets, options);
        if (snapshot.diagnostics.size() != m_reported) { // report changes, not every frame
            for (const auto& problem : snapshot.diagnostics) std::cerr << "[Player] " << problem.message << '\n';
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
        m_session.reset();
        m_assets.reset();
        m_reported = 0;
    }

private:
    static bool fail(const std::string& reason) {
        std::cerr << "[Player] " << reason << '\n';
        return false;
    }
    static bool fail(const std::filesystem::path& scene, const SceneDiagnostics& problems) {
        std::cerr << "[Player] " << scene.filename().string() << " cannot be run:\n";
        for (const auto& problem : problems) std::cerr << "  " << problem.message << '\n';
        return false;
    }

    PlayerOptions m_options;
    std::unique_ptr<AssetRegistry> m_assets;
    std::unique_ptr<PlaySession> m_session;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<RenderTarget> m_view;
    size_t m_reported = 0;
};

} // namespace

std::unique_ptr<Application> create_player_application(PlayerOptions options) {
    return std::make_unique<PlayerApplication>(std::move(options));
}

} // namespace maya::player
