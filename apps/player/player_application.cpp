#include "maya/assets/cook_cache.hpp"
#include "maya/assets/package.hpp"
#include "maya/simulation/project_recording.hpp"
#include "maya/simulation/script_assets.hpp"
#include "player_application.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/core/file_system.hpp"
#include "maya/core/system_info.hpp"
#include "maya/platform/input.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/simulation/physics_debug.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/streaming/world_streamer.hpp"
#include <algorithm>
#include <fstream>
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
        // A package (docs/projects.md#packages) runs its own project, from its bundle, with cooked content only.
        const auto& package = FileSystem::package_resources();
        auto manifest = PackageManifest{};
        if (package) {
            if (m_options.project) return fail("a package runs only its own project; it takes no project argument");
            auto input = std::ifstream(*package / package_manifest_name);
            auto read = read_package_manifest(input);
            if (!read) return fail(std::string(package_manifest_name) + ": " + read.error);
            manifest = std::move(read.manifest);
            m_options.project = *package / manifest.project;
            if (m_options.scene && std::ranges::find(manifest.scenes, *m_options.scene) == manifest.scenes.end())
                return fail(m_options.scene->generic_string() + " is not in this package");
        }
        if (!m_options.project) m_options.project = FileSystem::resolve("samples/basic_scene/project.maya");
        if (!m_options.project) return fail("no project given, and the sample project was not found");
        auto opened = open_project(*m_options.project);
        if (!opened) return fail(opened.error);
        const auto& project = opened.project;
        // From sources, the editor's cooked textures and meshes load from the project's cook cache
        // (docs/assets.md#cook-cache); a package holds them cooked.
        auto provider = std::unique_ptr<AssetProvider>{};
        if (package) provider = std::make_unique<PackageAssetProvider>(device);
        else provider = std::make_unique<FileAssetProvider>(device, std::make_shared<CookCache>(cook_cache_folder(project)));
        auto assets = open_project_assets(project, std::move(provider));
        if (!assets) return fail(assets.error);
        m_assets = std::move(assets.registry);
        m_assets->set_budgets(project_residency_budgets(project.settings)); // docs/assets.md#residency
        m_device = &device;

        const auto context = asset_property_context(*m_assets);
        auto scripts = project_script_settings(project.settings);
        scripts.assets = context; // scripts set clips by asset ID, checked against the catalog
        auto document = SceneDocument{};
        auto scene_name = std::string{};
        if (m_options.replay) {
            // The recording's own scene, checked against this build and the project's assets as they are.
            auto file = std::ifstream(*m_options.replay);
            if (!file) return fail("cannot read the recording " + m_options.replay->string());
            auto read = read_recording(file);
            if (!read) return fail(m_options.replay->filename().string() + ": " + read.error);
            m_recording = std::move(*read.recording);
            auto scene = read_scene(m_recording.scene, context);
            if (!scene) return fail(std::filesystem::path(m_recording.scene_name), scene.diagnostics);
            document = std::move(scene.document);
            if (auto refused = replay_refusal(m_recording, recorded_assets(*m_assets, document), {}); !refused.empty())
                return fail("cannot replay " + m_options.replay->filename().string() + ": " + refused);
            scripts.seed = m_recording.seed;
            scene_name = m_recording.scene_name;
        } else {
            const auto scene_path = m_options.scene ? project.resolve(*m_options.scene) : project.startup_scene;
            if (!scene_path) {
                return fail(m_options.scene ? m_options.scene->generic_string() + " is outside the project's content root"
                                            : project.file.string() + " has no startup scene; name a scene to run");
            }
            if (scene_path->extension() == ".world") {
                // A world (#1064): its persistent part plays as the scene, and its cells stream around the camera.
                auto file = std::ifstream(*scene_path, std::ios::binary);
                auto world = read_world(std::string(std::istreambuf_iterator<char>(file), {}));
                if (!world) return fail(scene_path->filename().string() + ": " + world.error);
                if (m_options.record || m_options.replay) return fail("recording and replaying a streamed world are not supported yet");
                auto loaded = load_scene_file(scene_path->parent_path() / world.document->persistent, context);
                if (!loaded) return fail(scene_path->parent_path() / world.document->persistent, loaded.diagnostics);
                document = std::move(loaded.document);
                const auto cache = package ? std::shared_ptr<CookCache>{} : std::make_shared<CookCache>(cook_cache_folder(project));
                m_streamer = std::make_unique<WorldStreamer>(*world.document, cooked_cell_loader(scene_path->parent_path(), cache, context),
                                                             project_streaming_settings(project.settings));
            } else {
                auto loaded = load_scene_file(*scene_path, context);
                if (!loaded) return fail(*scene_path, loaded.diagnostics);
                document = std::move(loaded.document);
            }
            scene_name = project.relative(*scene_path).generic_string();
        }
        if (m_options.record)
            m_recording = begin_recording(scene_name, document, recorded_assets(*m_assets, document), scripts.seed, {}, {});
        auto started = PlaySession::start(document, context, play_systems(registry_script_sources(*m_assets), registry_animation_clips(*m_assets), scripts));
        if (!started) return started.diagnostics.empty() ? fail(started.error) : fail(std::filesystem::path(scene_name), started.diagnostics);
        m_session = std::move(started.session);
        if (!m_session->camera()) return fail(std::filesystem::path(scene_name).filename().string() + " has no camera to show; add one in the editor");
        if (m_options.record) m_session->start_recording();
        if (m_options.debug_physics) {
            m_session->set_physics_debug_capture(true);
            std::cerr << "[Player] drawing the physics debug views\n";
        }
        if (m_options.debug_skeletons) std::cerr << "[Player] drawing skeletons\n";
        if (m_options.debug_view != DebugView::none)
            std::cerr << "[Player] showing the " << debug_view_name(m_options.debug_view) << " debug view\n";
        if (m_options.replay) {
            m_session->start_replay(m_recording.inputs, m_recording.checkpoints);
            std::cerr << "[Player] replaying " << m_recording.inputs.size() << " ticks of " << scene_name << " from "
                      << m_options.replay->filename().string() << '\n';
        }

        // A loading screen: what the scene draws is resident before the first frame, which then streams
        // anything later (docs/assets.md#asynchronous-loading).
        const auto loading = Stopwatch{};
        if (m_streamer) { // the cells around the camera, before the first frame
            m_streamer->set_sources({camera_position()});
            const auto& streamed = m_streamer->settle(m_session->world(), &m_session->physics());
            std::cerr << "[Player] streamed " << streamed.cells[size_t(CellState::active)] << " cells in around the camera\n";
            if (!streamed.last_error.empty()) std::cerr << "[Player] " << streamed.last_error << '\n';
        }
        const auto loaded = preload_render_assets(m_session->world(), *m_assets);
        std::cerr << "[Player] loaded " << loaded << " assets in " << int(loading.milliseconds()) << " ms\n";
        m_renderer = std::make_unique<Renderer>(device, std::move(shader));
        m_view = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, false, "player view"});
        if (package)
            std::cerr << "[Player] package " << manifest.name << ", content " << sha256_text(manifest.content).substr(0, 12) << ", built by "
                      << manifest.build << '\n';
        std::cerr << "[Player] " << (package ? manifest.name : project.name()) << " / " << scene_name << ": "
                  << m_session->world().size() << " entities\n";
        return true;
    }

    void on_update(float delta_time, bool input_enabled) override {
        // Loading first: prepared assets are finalized within the budget; then the frame is marked.
        m_assets->update();
        m_assets->begin_frame();
        for (const auto& warning : m_assets->take_budget_warnings()) std::cerr << "[Player] " << warning << '\n';
        // Then the world's cells around the camera, within the frame's budget (#1064).
        if (m_streamer) {
            m_streamer->set_sources({camera_position()});
            const auto errors = m_streamer->stats().loads_failed + m_streamer->stats().pinned;
            const auto& streamed = m_streamer->update(m_session->world(), &m_session->physics());
            if (streamed.loads_failed + streamed.pinned != errors) std::cerr << "[Player] " << streamed.last_error << '\n';
        }
        // Every window event belongs to the game in the player.
        if (input_enabled) m_session->input().feed(Input::instance().events());
        const auto frame = m_session->update(delta_time);
        for (const auto& message : frame.messages) // script logs and failed script instances
            std::cerr << "[" << message.source << "] " << message.text << '\n';
        if (!frame.error.empty()) throw std::runtime_error("[Player] " + frame.error);
        const auto& replay = m_session->replay();
        if (replay.replaying && replay.finished && !m_replay_done) {
            m_replay_done = true;
            if (replay.first_difference)
                throw std::runtime_error("[Player] the replay differs from the recording at tick " + std::to_string(*replay.first_difference));
            if (m_session->full_state_hash() != m_recording.final_state)
                throw std::runtime_error("[Player] the replay ends in a different state than the recording");
            // It stays on its last frame, paused, until the window is closed.
            std::cerr << "[Player] the replay matches the recording: " << m_session->clock().tick() << " ticks, " << replay.checked
                      << " checkpoints, and the final state\n";
        }
    }

    void on_render(GraphicsDevice& device) override {
        struct EndFrame {
            AssetRegistry& assets;
            ~EndFrame() { assets.end_frame(); }
        } end_frame{*m_assets};
        const auto surface = device.acquire_surface();
        if (!surface) return; // no drawable or zero-sized window: skip the view this frame
        const auto& target = surface.target;
        const auto& world = m_session->world();
        const auto camera = world.find(*m_session->camera());
        if (!camera) throw std::runtime_error("[Player] the camera entity no longer exists");
        if (auto error = m_view->resize(target.width, target.height)) throw std::runtime_error(error.message);
        // Poses between the last two ticks, so motion is smooth at any display rate.
        const auto poses = m_session->presentation();
        auto view = extract_render_view(world, *camera, target.width, target.height, &poses);
        if (!view) throw std::runtime_error("[Player] the camera entity has no valid view");
        view->debug_view = m_options.debug_view; // none unless asked for
        auto options = RenderExtractOptions{};
        options.poses = &poses;
        options.skins = &m_skins;
        options.loading = AssetLoading::stream; // frames never wait for assets
        options.origin = view->position; // camera-relative (#1065)
        m_debug.clear(); // debug views are never drawn unless asked for
        m_debug.origin = view->position;
        if (m_options.debug_physics) play_physics_debug(world, m_session->physics(), {all_physics_debug, all_collision_groups}, &poses, m_debug);
        if (m_options.debug_skeletons) skeleton_debug(world, *m_assets, &poses, m_debug, &m_skins);
        if (!m_debug.empty()) options.debug = &m_debug;
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
        if (m_options.record && m_session) {
            try {
                finish_recording(m_recording, *m_session);
                auto file = std::ofstream(*m_options.record);
                write_recording(file, m_recording);
                if (!file) throw std::runtime_error("cannot write " + m_options.record->string());
                std::cerr << "[Player] recorded " << m_recording.inputs.size() << " ticks to " << m_options.record->string() << '\n';
            } catch (const std::exception& error) {
                std::cerr << "[Player] the recording was not saved: " << error.what() << '\n';
            }
        }
        if (m_assets) { // a frame never waits for a load; anything else is a bug (docs/assets.md#asynchronous-loading)
            const auto loading = m_assets->load_stats();
            std::cerr << "[Player] loads: " << loading.finalized << " finished, " << loading.waited_in_frames << " waited for inside a frame\n";
            if (m_device) { // what was resident at the end (docs/assets.md#residency)
                const auto memory = process_memory();
                const auto report = residency_report(m_assets->residency(), m_streamer ? m_streamer->stats().bytes_loaded : 0,
                                                     (m_renderer ? m_renderer->shadow_bytes() + m_renderer->table_bytes() : 0) +
                                                         (m_view ? m_view->gpu_bytes() : 0),
                                                     m_device->stats(), m_device->reported_memory(),
                                                     memory ? std::optional(size_t(memory->footprint)) : std::nullopt);
                std::cerr << "[Player] resident: " << residency_summary(report) << '\n';
            }
        }
        m_streamer.reset(); // its loads end before the session and the assets they use
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
    GraphicsDevice* m_device = nullptr;
    std::unique_ptr<AssetRegistry> m_assets;
    std::unique_ptr<PlaySession> m_session;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<RenderTarget> m_view;
    std::unique_ptr<WorldStreamer> m_streamer; // when the scene is a world: its cells around the camera
    math::DVec3 camera_position() const {
        const auto camera = m_session->world().find(*m_session->camera());
        const auto pose = camera ? m_session->world().world_matrix(*camera) : std::nullopt;
        return pose ? pose->translation : math::DVec3{};
    }
    size_t m_reported = 0;
    DebugDraw m_debug; // --debug-physics and --debug-skeletons: this frame's, reused
    SkinBindingCache m_skins; // the session's skins' joints, between frames (#1038)
    PlayRecording m_recording; // being made (--record), or being replayed (--replay)
    bool m_replay_done = false;
};

} // namespace

std::unique_ptr<Application> create_player_application(PlayerOptions options) {
    return std::make_unique<PlayerApplication>(std::move(options));
}

} // namespace maya::player
