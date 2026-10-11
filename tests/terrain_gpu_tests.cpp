#include "maya/assets/generator_file.hpp"
#include "maya/streaming/terrain_streamer.hpp"
#include "maya/streaming/scatter_streamer.hpp"
#include "support/metal_view.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace maya;
using namespace maya::test;

namespace {
const auto grass = MaterialAsset{{0.25f, 0.55f, 0.2f, 1.0f}, 0.0f, 0.9f};
const auto rock = MaterialAsset{{0.45f, 0.43f, 0.4f, 1.0f}, 0.0f, 0.8f};

/// A 6 x 6-cell terrain at W1's place, its two layers by slope, under a sun; a streamer keeping all of it
/// drawn around a point.
struct TerrainScene {
    Gpu gpu;
    TestProject project{gpu.device, {}, {{"grass.material", grass}, {"rock.material", rock}}};
    World world;
    std::unique_ptr<TerrainStreamer> streamer;
    TerrainScene() {
        auto generator = GeneratorAsset{};
        generator.region_min_x = 40;
        generator.region_min_z = 33;
        generator.region_max_x = 45;
        generator.region_max_z = 38;
        project.write("hills.generator", write_generator_file(generator));
        const auto heights = AssetRef<GeneratorAsset>{{0x7465, 0x900}};
        REQUIRE_FALSE(project.registry->register_asset({heights.id, AssetKind::generator, "hills.generator"}));
        const auto ground = project.add<MaterialAsset>(1, "grass.material"), cliff = project.add<MaterialAsset>(2, "rock.material");
        auto terrain = TerrainComponent{heights, ground, cliff};
        terrain.layer1_slope = {18.0f, 28.0f};
        build_world(world, [&](WorldCommands& commands) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{});
            commands.add(entity, terrain);
            auto sun = commands.create();
            commands.add(sun, TransformComponent{{}, math::Quat::from_axis_angle(math::Vec3{1, 0.3f, 0.2f}.normalized(), -0.9f), {1.0f}});
            commands.add(sun, LightComponent{});
        });
        auto settings = TerrainSettings{};
        settings.streaming.load_radius = 2000.0;
        settings.streaming.activate_radius = 2000.0;
        streamer = std::make_unique<TerrainStreamer>(&gpu.device, nullptr, settings);
        streamer->set_sources({{43 * 128.0, 0.0, 36 * 128.0}});
        for (int i = 0; i < 200 && (streamer->stats().drawn < 36 || streamer->terrains().empty() || !streamer->terrains().front().overview); ++i) {
            project.registry->update();
            project.registry->wait_idle();
            streamer->wait_idle();
            streamer->update(world, *project.registry, nullptr);
        }
        REQUIRE(streamer->stats().drawn == 36);
    }
    /// A view from `height` metres above the ground at `x, z`, pitched down `pitch` radians, facing +x.
    RenderView view_from(double x, double z, double height, float pitch, uint32_t width = 320, uint32_t height_px = 180) {
        const auto ground = streamer->height_at(x, z);
        REQUIRE(ground);
        auto pose = math::Affine{};
        pose.translation = {x, double(*ground) + height, z};
        pose.linear = (math::Quat::from_axis_angle({0, 1, 0}, -math::PI * 0.5f) * math::Quat::from_axis_angle({1, 0, 0}, pitch)).to_mat4();
        auto view = make_render_view(CameraComponent{}, pose, width, height_px);
        REQUIRE(view);
        view->clear_color = {1.0, 0.0, 1.0, 1.0}; // magenta shows through any crack
        return *view;
    }
    RgbImage render(const RenderView& view) {
        auto options = RenderExtractOptions{};
        options.terrains = streamer->terrains();
        return gpu.render(world, *project.registry, view, options);
    }
};
bool magenta(const uint8_t* p) { return p[0] > 80 && p[2] > 80 && p[1] < std::min(p[0], p[2]) / 2; }
} // namespace

TEST_CASE("Terrain draws its tiles and overview with no cracks between tiles or levels, and casts shadows", "[rhi][terrain]") {
    TerrainScene scene;
    const auto centre_x = 43 * 128.0, centre_z = 36 * 128.0;
    // From low over the ground, where the finest levels meet the coarser ones, to high above, where the
    // overview meets the tiles: from the middle of the terrain every pixel sees it.
    // Pitched down enough that the top of the view meets the ground well inside the region.
    for (const auto& [height, pitch] : {std::pair{3.0, -1.0f}, {12.0, -0.9f}, {60.0, -1.0f}, {220.0, -1.3f}}) {
        for (const auto offset : {0.0, 37.5, 101.25}) {
            INFO("height " << height << ", offset " << offset);
            const auto view = scene.view_from(centre_x + offset, centre_z - offset * 0.5, height, pitch);
            const auto image = scene.render(view);
            auto holes = size_t{0}, green = size_t{0}, dark = size_t{0};
            for (size_t i = 0; i < image.rgb.size(); i += 3) {
                holes += magenta(&image.rgb[i]);
                green += image.rgb[i + 1] > image.rgb[i] && image.rgb[i + 1] > image.rgb[i + 2];
                dark += int(image.rgb[i]) + image.rgb[i + 1] + image.rgb[i + 2] < 60;
            }
            CHECK(holes == 0);
            CHECK(green > 0); // grass where it is gentle; rock on the slopes
            CHECK(dark < image.rgb.size() / 3 / 20); // lit, not black: normals and shading work
            const auto& stats = scene.gpu.renderer->stats();
            CHECK(stats.terrain_patches > 0);
            CHECK(stats.terrain_shadow_patches > 0);
        }
    }
}

TEST_CASE("Terrain views match their approved references", "[rhi][terrain][visual]") {
    TerrainScene scene;
    // Facing +x from inside the region's west edge: down onto the ground, across the hills, and from high
    // above, where the tiles meet the overview; then that view's levels of detail.
    const auto x = 41 * 128.0, z = 36 * 128.0;
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    auto above = RenderView{};
    for (const auto& [name, height, pitch] : {std::tuple{"terrain-ground", 4.0, -0.25f}, {"terrain-hills", 40.0, -0.4f}, {"terrain-above", 300.0, -0.9f}}) {
        auto view = scene.view_from(x, z, height, pitch, 640, 360);
        view.clear_color = {0.45, 0.6, 0.85, 1.0}; // a plain sky where the region ends
        images.emplace_back(name, scene.render(view));
        above = view;
    }
    above.debug_view = DebugView::lod;
    images.emplace_back("terrain-lod", scene.render(above));
    compare_with_references(std::filesystem::path(MAYA_SOURCE_DIR) / "tests/references/terrain", MAYA_VISUAL_DIFFS, images);
}

TEST_CASE("Terrain is drawn in every debug view, with no holes", "[rhi][terrain]") {
    TerrainScene scene;
    const auto view = scene.view_from(43 * 128.0, 36 * 128.0, 40.0, -0.9f); // all ground: the clear color would show through a hole
    const auto lit = scene.render(view);
    for (auto debug = uint8_t(DebugView::luminance); debug <= uint8_t(DebugView::texels); ++debug) {
        INFO("debug view " << debug_view_name(DebugView(debug)));
        auto shown = view;
        shown.debug_view = DebugView(debug);
        const auto image = scene.render(shown);
        auto holes = size_t{0};
        for (size_t i = 0; i < image.rgb.size(); i += 3) holes += magenta(&image.rgb[i]);
        CHECK(holes == 0);
        if (shown.debug_view != DebugView::emissive) CHECK(image.rgb != lit.rgb); // the view's own image, not the lit one
    }
}

TEST_CASE("Terrain with scattered rocks matches its approved reference", "[rhi][terrain][visual][scatter]") {
    // The same hills, with cubes scattered over them by a scatter generator (#1093), as the player streams them.
    Gpu gpu;
    TestProject project{gpu.device, {{"rock.obj", unit_cube({0.55f, 0.53f, 0.5f, 1.0f})}}, {{"grass.material", grass}, {"rock.material", rock}}};
    auto heights_generator = GeneratorAsset{};
    heights_generator.region_min_x = 40;
    heights_generator.region_min_z = 33;
    heights_generator.region_max_x = 45;
    heights_generator.region_max_z = 38;
    const auto heights = AssetRef<GeneratorAsset>{{0x7465, 0x900}}, rocks = AssetRef<GeneratorAsset>{{0x7465, 0x901}};
    const auto cube = project.add<MeshAsset>(3, "rock.obj");
    const auto ground = project.add<MaterialAsset>(1, "grass.material"), cliff = project.add<MaterialAsset>(2, "rock.material");
    auto scatter = heights_generator;
    scatter.kind = GeneratorKind::scatter;
    scatter.version = scatter_version;
    scatter.seed = 1093;
    scatter.heights = heights;
    scatter.spacing = 8.0f;
    scatter.density = 0.35f;
    scatter.max_slope = 25.0f;
    scatter.min_scale = 1.5f;
    scatter.max_scale = 3.5f;
    scatter.mesh0 = cube;
    scatter.material0 = cliff;
    project.write("hills.generator", write_generator_file(heights_generator));
    project.write("rocks.generator", write_generator_file(scatter));
    REQUIRE_FALSE(project.registry->register_asset({heights.id, AssetKind::generator, "hills.generator"}));
    REQUIRE_FALSE(project.registry->register_asset({rocks.id, AssetKind::generator, "rocks.generator"}));
    World world;
    build_world(world, [&](WorldCommands& commands) {
        auto terrain = TerrainComponent{heights, ground, cliff};
        terrain.layer1_slope = {18.0f, 28.0f};
        auto entity = commands.create();
        commands.add(entity, TransformComponent{});
        commands.add(entity, terrain);
        auto runner = commands.create();
        commands.add(runner, GeneratorComponent{rocks});
        auto sun = commands.create();
        commands.add(sun, TransformComponent{{}, math::Quat::from_axis_angle(math::Vec3{1, 0.3f, 0.2f}.normalized(), -0.9f), {1.0f}});
        commands.add(sun, LightComponent{});
    });
    const auto x = 41 * 128.0, z = 36 * 128.0;
    auto terrain_settings = TerrainSettings{};
    terrain_settings.streaming.load_radius = 2000.0;
    terrain_settings.streaming.activate_radius = 2000.0;
    auto terrain = TerrainStreamer(&gpu.device, nullptr, terrain_settings);
    terrain.set_sources({{x, 0.0, z}});
    terrain.settle(world, *project.registry, nullptr);
    auto scatter_settings = ScatterSettings{};
    scatter_settings.streaming.activate_radius = 400.0;
    auto scattered = ScatterStreamer(nullptr, scatter_settings);
    auto sink = WorldScatterSink(world, nullptr);
    scattered.set_sources({{x, 0.0, z}});
    scattered.settle(world, *project.registry, sink);
    REQUIRE(scattered.stats().items > 100);
    // Across the hills, as the terrain references' "hills" view, from 40 m above the ground.
    const auto ground_height = terrain.height_at(x, z);
    REQUIRE(ground_height);
    auto pose = math::Affine{};
    pose.translation = {x, double(*ground_height) + 40.0, z};
    pose.linear = (math::Quat::from_axis_angle({0, 1, 0}, -math::PI * 0.5f) * math::Quat::from_axis_angle({1, 0, 0}, -0.4f)).to_mat4();
    auto view = make_render_view(CameraComponent{}, pose, 640, 360);
    REQUIRE(view);
    view->clear_color = {0.45, 0.6, 0.85, 1.0};
    auto options = RenderExtractOptions{};
    options.terrains = terrain.terrains();
    project.registry->update();
    for (int i = 0; i < 3; ++i) { // the cube and its material, loaded
        gpu.render(world, *project.registry, *view, options);
        project.registry->wait_idle();
        project.registry->update();
    }
    const auto image = gpu.render(world, *project.registry, *view, options);
    CHECK(gpu.renderer->last_view().drawn > 50); // rocks in view
    compare_with_references(std::filesystem::path(MAYA_SOURCE_DIR) / "tests/references/terrain", MAYA_VISUAL_DIFFS, {{"terrain-scatter", image}});
}
