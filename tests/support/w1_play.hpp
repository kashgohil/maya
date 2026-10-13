#pragma once
// W1 (#1069) played as the player plays a world, for tests that render it: the persistent part in a session
// held at a tick, the terrain, the cells once the terrain under them collides, and scatter once the cells are
// in, all settled around one place, then drawn through the session World's visibility index with the
// project's levels of detail and culls. From a project's sources (with its cook cache) or a package's
// content (none).

#include "w1.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/renderer/visibility_index.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/script_assets.hpp"
#include "maya/streaming/scatter_streamer.hpp"
#include "maya/streaming/terrain_streamer.hpp"
#include "maya/streaming/world_streamer.hpp"
#include "support/metal_view.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>

namespace maya::test {

inline WorldDocument read_world_file(const std::filesystem::path& file) {
    auto input = std::ifstream(file, std::ios::binary);
    auto read = read_world(std::string(std::istreambuf_iterator<char>(input), {}));
    REQUIRE(read);
    return std::move(*read.document);
}

struct W1Play {
    std::shared_ptr<AssetRegistry> registry; // shared by several plays, so what one loaded the next has
    std::unique_ptr<PlaySession> session;
    std::unique_ptr<TerrainStreamer> terrain;
    std::unique_ptr<WorldStreamer> cells;
    std::unique_ptr<ScatterStreamer> scatter;
    std::unique_ptr<WorldScatterSink> sink;
    std::unique_ptr<VisibilityIndex> index;
    SkinBindingCache skins;
    float lod_pixels = 1.0f, cull_pixels = 2.0f;

    /// `world_file` is the project's w1.world; `cache` its cook cache, or none for a package.
    W1Play(GraphicsDevice& device, std::shared_ptr<AssetRegistry> assets, const ProjectSettings& project, const std::filesystem::path& world_file,
           std::shared_ptr<CookCache> cache)
        : registry(std::move(assets)) {
        lod_pixels = project_lod_pixels(project);
        cull_pixels = project_cull_pixels(project);
        const auto context = asset_property_context(*registry);
        auto world = read_world_file(world_file);
        auto persistent = load_scene_file(world_file.parent_path() / world.persistent, context);
        REQUIRE(persistent);
        auto started = PlaySession::start(std::move(persistent.document), context,
                                          play_systems(registry_script_sources(*registry), registry_animation_clips(*registry)));
        INFO(started.error);
        REQUIRE(started);
        session = std::move(started.session);
        session->clock().pause();
        const auto streaming = project_streaming_settings(project);
        auto terrain_settings = TerrainSettings{};
        terrain_settings.streaming = streaming;
        terrain_settings.collide = true;
        terrain = std::make_unique<TerrainStreamer>(&device, cache, terrain_settings);
        cells = std::make_unique<WorldStreamer>(std::move(world), cooked_cell_loader(world_file.parent_path(), cache, context), streaming);
        link_terrain(*cells, *terrain);
        auto scatter_settings = ScatterSettings{};
        scatter_settings.streaming = streaming;
        scatter = std::make_unique<ScatterStreamer>(cache, scatter_settings);
        sink = std::make_unique<WorldScatterSink>(session->world(), &session->physics());
        scatter->set_ready([this](CellIndex cell) { return !cells->world().cell(cell) || cells->state(cell) == CellState::active; });
    }
    ~W1Play() {
        if (scatter) scatter->clear(*sink);
        if (cells) cells->unload_all(session->world(), &session->physics());
        if (terrain) terrain->clear(&session->physics());
    }
    W1Play(const W1Play&) = delete;
    W1Play& operator=(const W1Play&) = delete;

    /// Everything around `at` in, as a loading screen has it, and every asset the World draws loaded.
    void settle_at(const math::DVec3& at) {
        auto& world = session->world();
        terrain->set_sources({at});
        cells->set_sources({at});
        scatter->set_sources({at});
        terrain->settle(world, *registry, &session->physics());
        cells->settle(world, &session->physics());
        scatter->settle(world, *registry, *sink);
        preload_render_assets(world, *registry);
        registry->wait_idle();
        registry->update();
    }
    /// As Play starts: everything around S1's camera in, then its first two ticks, the first of which poses the
    /// clips' starts (frames show poses between the last two ticks), each followed by a frame from S1's camera at
    /// the size drawn later: a level of detail depends on the frames before (docs/renderer.md#levels-of-detail).
    void start(uint32_t width, uint32_t height) {
        const auto point = w1::s1_point(0);
        settle_at({point.x, 0.0, point.z});
        while (session->clock().tick() < start_ticks) {
            run_to(session->clock().tick() + 1);
            const auto& world = session->world();
            const auto poses = session->presentation();
            auto view = extract_render_view(world, *world.find(*session->camera()), width, height, &poses);
            REQUIRE(view);
            (void)extract_render_snapshot(world, *registry, options(*view, poses));
        }
    }
    static constexpr uint64_t start_ticks = 2;
    /// Runs ticks until `tick` ticks have completed.
    void run_to(uint64_t tick) {
        while (session->clock().tick() < tick) {
            session->clock().step();
            const auto frame = session->update(0.0);
            REQUIRE(frame.error.empty());
            for (const auto& message : frame.messages) UNSCOPED_INFO(message.text);
            REQUIRE(frame.messages.empty());
        }
    }
    /// What the player draws from `camera` at this size, after the frames it takes the assets it asks for to load.
    RgbImage render(Gpu& gpu, EntityId camera, uint32_t width, uint32_t height) {
        const auto& world = session->world();
        const auto handle = world.find(camera);
        REQUIRE(handle);
        const auto poses = session->presentation();
        auto view = extract_render_view(world, *handle, width, height, &poses);
        REQUIRE(view);
        for (int i = 0; i < 3; ++i) {
            gpu.render(world, *registry, *view, options(*view, poses));
            registry->wait_idle();
            registry->update();
        }
        return gpu.render(world, *registry, *view, options(*view, poses));
    }
    /// What the player extracts for `view`: the poses between the last two ticks, through the session World's
    /// visibility index, with the project's levels of detail and culls, and the terrain.
    RenderExtractOptions options(const RenderView& view, const PresentationPoses& poses) {
        if (!index) index = std::make_unique<VisibilityIndex>(session->world());
        auto result = RenderExtractOptions{};
        result.poses = &poses;
        result.skins = &skins;
        result.origin = view.position;
        result.view = &view;
        result.index = index.get();
        result.terrains = terrain->terrains();
        result.lod_pixels = lod_pixels;
        result.cull_pixels = cull_pixels;
        return result;
    }
};

/// A project's assets, from its sources through its cook cache: one for several plays, which then load each asset
/// once (an unoptimized build reads W1's cooked textures slowly).
inline std::shared_ptr<AssetRegistry> w1_assets(GraphicsDevice& device, const std::filesystem::path& folder) {
    auto opened = open_project(folder);
    REQUIRE(opened);
    auto assets = open_project_assets(opened.project,
                                      std::make_unique<FileAssetProvider>(device, std::make_shared<CookCache>(cook_cache_folder(opened.project))));
    REQUIRE(assets);
    return std::move(assets.registry);
}

/// W1 from a project's sources, with its cook cache, played afresh with `assets` (or its own).
inline std::unique_ptr<W1Play> w1_from_sources(GraphicsDevice& device, const std::filesystem::path& folder,
                                               std::shared_ptr<AssetRegistry> assets = {}) {
    auto opened = open_project(folder);
    REQUIRE(opened);
    if (!assets) assets = w1_assets(device, folder);
    return std::make_unique<W1Play>(device, std::move(assets), opened.project.settings, folder / "w1.world",
                                    std::make_shared<CookCache>(cook_cache_folder(opened.project)));
}

} // namespace maya::test
