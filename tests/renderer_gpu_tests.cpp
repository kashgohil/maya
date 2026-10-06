#include <catch2/catch_test_macros.hpp>
#include "support/tone_mapping.hpp"
#include "maya/core/file_system.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/world/spatial.hpp"
#include "support/render_scene.hpp"
#include <array>
#include <cstdlib>
#include <cstring>

using namespace maya;
using namespace maya::test;

namespace {
using Pixel = std::array<int, 4>;
constexpr auto black = std::array<double, 4>{0.0, 0.0, 0.0, 1.0};
constexpr auto no_ambient = RenderExtractOptions{{0.0f, 0.0f, 0.0f}};

Pixel pixel(const std::vector<std::byte>& pixels, uint32_t width, uint32_t x, uint32_t y) {
    const auto* p = pixels.data() + (size_t{y} * width + x) * 4;
    return {int(p[0]), int(p[1]), int(p[2]), int(p[3])};
}
std::vector<std::byte> read(GraphicsDevice& device, TextureHandle texture) {
    auto pixels = std::vector<std::byte>{};
    REQUIRE_FALSE(device.read_texture(texture, pixels));
    return pixels;
}
// AgX brings some of the other channels into saturated colors, so a color is told by its dominant channel.
bool is_red(Pixel p) { return p[0] > 150 && p[0] > p[1] + 80 && p[0] > p[2] + 80; }
bool is_green(Pixel p) { return p[1] > 150 && p[1] > p[0] + 80 && p[1] > p[2] + 80; }
bool is_black(Pixel p) { return p[0] == 0 && p[1] == 0 && p[2] == 0; }

/// A headless Metal device, the renderer, and a project serving a cube and a slanted quad.
struct GpuFixture {
    GpuFixture() {
        REQUIRE(device.initialize(nullptr));
        auto source = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        REQUIRE_FALSE(source.empty());
        renderer = std::make_unique<Renderer>(device, std::move(source));
        project = std::make_unique<TestProject>(device,
            std::map<std::string, Geometry>{{"cube.mesh", unit_cube()}, {"quad.mesh", slanted_quad()}},
            std::map<std::string, MaterialAsset>{{"red.material", {{1, 0, 0, 1}, 0, 1}},
                                                 {"green.material", {{0, 1, 0, 1}, 0, 1}},
                                                 {"white.material", {{1, 1, 1, 1}, 0, 1}},
                                                 {"grey.material", {{0.5f, 0.5f, 0.5f, 1}, 0, 1}}});
        cube = project->add<MeshAsset>(1, "cube.mesh");
        quad = project->add<MeshAsset>(2, "quad.mesh");
        red = project->add<MaterialAsset>(3, "red.material");
        green = project->add<MaterialAsset>(4, "green.material");
        white = project->add<MaterialAsset>(5, "white.material");
        missing = project->add<MeshAsset>(6, "missing.mesh"); // registered but absent
        grey = project->add<MaterialAsset>(7, "grey.material");
    }
    ~GpuFixture() {
        project.reset();
        renderer.reset();
        CHECK(device.take_gpu_errors().empty());
        device.shutdown();
    }
    /// A light shining along -Z, so faces toward +Z receive full diffuse light.
    void add_light(WorldCommands& commands) {
        auto light = commands.create();
        commands.add(light, TransformComponent{});
        commands.add(light, LightComponent{});
    }
    void add_mesh(WorldCommands& commands, AssetRef<MeshAsset> mesh, AssetRef<MaterialAsset> material,
                  TransformComponent transform) {
        auto entity = commands.create();
        commands.add(entity, transform);
        commands.add(entity, MeshRendererComponent{mesh, material, true});
    }
    RenderView front_view(uint32_t width, uint32_t height) {
        auto view = make_render_view(CameraComponent{}, look_pose({0, 0, 5}, {0, 0, 0}), width, height);
        REQUIRE(view);
        view->clear_color = black;
        return *view;
    }
    /// One frame rendering `view` into `target`, then a synchronous readback.
    std::vector<std::byte> render(const RenderSnapshot& snapshot, const RenderView& view, RenderTarget& target) {
        REQUIRE_FALSE(target.resize(view.width, view.height));
        REQUIRE_FALSE(device.begin_frame());
        const auto error = renderer->render(snapshot, view, target);
        INFO(error.message);
        REQUIRE_FALSE(error);
        REQUIRE_FALSE(device.end_frame());
        return read(device, target.color());
    }

    MetalDevice device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<TestProject> project;
    AssetRef<MeshAsset> cube, quad, missing;
    AssetRef<MaterialAsset> red, green, white, grey;
};

/// Two instances of one cube with different materials, and a missing mesh between them.
void two_cubes(GpuFixture& fixture, World& world) {
    build_world(world, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        fixture.add_mesh(commands, fixture.cube, fixture.red, {{-1, 0, 0}, {}, {1.0f}});
        fixture.add_mesh(commands, fixture.cube, fixture.green, {{1, 0, 0}, {}, {1.0f}});
        fixture.add_mesh(commands, fixture.missing, fixture.white, {{0, 0, 0}, {}, {1.0f}});
    });
}
} // namespace

TEST_CASE("Metal renderer draws shared geometry with per-instance transforms and materials", "[rhi][renderer]") {
    GpuFixture fixture;
    World world;
    two_cubes(fixture, world);
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
    CHECK(snapshot.meshes.size() == 1);
    CHECK(snapshot.instances.size() == 2);
    CHECK(snapshot.diagnostics.size() == 1); // the missing mesh
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "fixed scene"});
    const auto pixels = fixture.render(snapshot, fixture.front_view(64, 64), target);
    // Front faces point at the light, so they show their full base color.
    CHECK(is_red(pixel(pixels, 64, 20, 32)));
    CHECK(is_green(pixel(pixels, 64, 44, 32)));
    CHECK(is_black(pixel(pixels, 64, 32, 32))); // the missing mesh is skipped, not drawn with garbage
    CHECK(is_black(pixel(pixels, 64, 20, 5)));
    CHECK(fixture.renderer->stats().draws == 2);
}

TEST_CASE("Metal renderer draws many instances of each mesh and material with one instanced draw each, at their own places", "[rhi][renderer][batches]") {
    GpuFixture fixture;
    World world;
    // A checkerboard of 7 x 5 small cubes in two materials, interleaved, and one far out of view.
    build_world(world, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 7; ++column)
                fixture.add_mesh(commands, fixture.cube, (row + column) % 2 ? fixture.green : fixture.red,
                                 {{float(column - 3) * 0.6f, float(row - 2) * 0.6f, 0}, {}, {0.3f}});
        fixture.add_mesh(commands, fixture.cube, fixture.red, {{500, 0, 0}, {}, {1.0f}});
    });
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
    REQUIRE(snapshot.instances.size() == 36);
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "checkerboard"});
    const auto view = fixture.front_view(256, 256);
    const auto pixels = fixture.render(snapshot, view, target);
    CHECK(fixture.renderer->stats().draws == 2); // one per material, for 35 cubes in view
    CHECK(fixture.renderer->last_view().drawn == 35);
    CHECK(fixture.renderer->last_view().culled == 1);
    for (int row = 0; row < 5; ++row)
        for (int column = 0; column < 7; ++column) {
            const auto clip = view.matrices.view_projection * math::Vec4{float(column - 3) * 0.6f, float(row - 2) * 0.6f, 0.15f, 1.0f};
            const auto x = uint32_t((clip.x / clip.w * 0.5f + 0.5f) * 256.0f), y = uint32_t((0.5f - clip.y / clip.w * 0.5f) * 256.0f);
            const auto shown = pixel(pixels, 256, x, y);
            INFO("cube " << column << ", " << row);
            CHECK(((row + column) % 2 ? is_green(shown) : is_red(shown)));
            // Between cubes, nothing.
            CHECK(is_black(pixel(pixels, 256, std::min(x + 14, 255u), y)));
        }
}

TEST_CASE("Metal renderer lights nonuniformly scaled surfaces by their true normals", "[rhi][renderer]") {
    GpuFixture fixture;
    World world;
    build_world(world, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        // Scaling z by 4 tilts the quad toward +Y: its true normal is (0, 0.970, 0.243), so the default light
        // along -Z gives diffuse 0.243. Transforming the normal by the model matrix would give 0.970.
        fixture.add_mesh(commands, fixture.quad, fixture.white, {{0, 0, 0}, {}, {1.0f, 1.0f, 4.0f}});
    });
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
    auto view = make_render_view(CameraComponent{}, look_pose({0, 5, 0}, {0, 0, 0}, {0, 0, -1}), 32, 32);
    REQUIRE(view);
    view->clear_color = black;
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "normals"});
    const auto pixels = fixture.render(snapshot, *view, target);
    const auto center = pixel(pixels, 32, 16, 16);
    INFO("center " << center[0] << " " << center[1] << " " << center[2]);
    // Diffuse 0.243 plus a weak 4% highlight, about 0.28 of scene light, through exposure and AgX. The
    // true normal's 0.97 would be far brighter.
    CHECK(center[0] > test::displayed_grey(0.20, view->exposure));
    CHECK(center[0] < test::displayed_grey(0.39, view->exposure));
    CHECK(center[0] < test::displayed_grey(0.90, view->exposure));
    CHECK(center[0] == center[1]);
    CHECK(center[1] == center[2]);
}

TEST_CASE("Metal view output presents identically into a player window and an editor viewport", "[rhi][renderer]") {
    GpuFixture fixture;
    World world;
    two_cubes(fixture, world);
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
    auto view_target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "view"});
    const auto view = fixture.render(snapshot, fixture.front_view(48, 32), view_target);

    const auto destination = [&](uint32_t width, uint32_t height, const char* label) {
        auto created = fixture.device.create_texture({width, height, Format::rgba8_unorm,
            TextureUsage::render_target | TextureUsage::readback, label});
        REQUIRE(created);
        return created.handle;
    };
    const auto player = destination(48, 32, "player window");
    const auto editor = destination(96, 48, "editor window");
    constexpr auto viewport = PixelRect{24, 8, 48, 32};
    REQUIRE_FALSE(fixture.device.begin_frame());
    REQUIRE_FALSE(fixture.renderer->present(view_target, player, {0, 0, 48, 32}));
    REQUIRE_FALSE(fixture.renderer->present(view_target, editor, viewport, {0.0, 0.0, 1.0, 1.0}));
    REQUIRE_FALSE(fixture.device.end_frame());
    const auto player_pixels = read(fixture.device, player);
    const auto editor_pixels = read(fixture.device, editor);

    CHECK(is_red(pixel(view, 48, 18, 16))); // the red cube spans pixels 15-20
    size_t mismatches = 0;
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 48; ++x) {
            const auto expected = pixel(view, 48, x, y);
            const auto close = [&](Pixel actual) {
                for (int c = 0; c < 4; ++c) if (std::abs(actual[c] - expected[c]) > 1) return false;
                return true;
            };
            if (!close(pixel(player_pixels, 48, x, y))) ++mismatches;
            if (!close(pixel(editor_pixels, 96, x + viewport.x, y + viewport.y))) ++mismatches;
        }
    CHECK(mismatches == 0);
    const auto outside = pixel(editor_pixels, 96, 4, 4);
    CHECK(outside == Pixel{0, 0, 255, 255});
    CHECK(pixel(editor_pixels, 96, 90, 44) == Pixel{0, 0, 255, 255});
}

TEST_CASE("Metal views render at sizes independent of any window and survive resizing", "[rhi][renderer]") {
    GpuFixture fixture;
    World world;
    build_world(world, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        fixture.add_mesh(commands, fixture.cube, fixture.red, {{0, 0, 0}, {}, {1.0f}});
    });
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "resized view"});
    const auto baseline = fixture.device.native_texture_count();
    const std::pair<uint32_t, uint32_t> sizes[] = {{64, 64}, {128, 32}, {32, 96}, {32, 96}, {32, 96}, {64, 64}};
    for (const auto& [width, height] : sizes) {
        const auto pixels = fixture.render(snapshot, fixture.front_view(width, height), target);
        CHECK(is_red(pixel(pixels, width, width / 2, height / 2)));
        CHECK(is_black(pixel(pixels, width, 0, 0)));
        CHECK(is_black(pixel(pixels, width, width - 1, height - 1)));
    }
    CHECK(target.allocations() == 4); // repeated sizes reuse the textures
    fixture.device.wait_idle();
    // Replaced targets were retired: the last target's three textures and the renderer's own (the
    // placeholder, the split-sum table, the empty cube, the cleared "no shadows" map, and the sun's shadow
    // atlas, since the light casts shadows) remain.
    CHECK(fixture.device.native_texture_count() == baseline + 8);
}

TEST_CASE("Metal frames in flight keep their meshes when entities and assets go away", "[rhi][renderer]") {
    GpuFixture fixture;
    const auto baseline = fixture.device.native_buffer_count();
    World world;
    build_world(world, [&](WorldCommands& commands) { fixture.add_light(commands); });
    auto first = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "before deletion"});
    auto second = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "after deletion"});
    REQUIRE_FALSE(first.resize(32, 32));
    REQUIRE_FALSE(second.resize(32, 32));
    for (int round = 0; round < 10; ++round) {
        build_world(world, [&](WorldCommands& commands) {
            fixture.add_mesh(commands, fixture.cube, fixture.red, {{0, 0, 0}, {}, {1.0f}});
        });
        auto snapshot = std::make_optional(extract_render_snapshot(world, *fixture.project->registry, no_ambient));
        REQUIRE(snapshot->instances.size() == 1);
        REQUIRE_FALSE(fixture.device.begin_frame());
        REQUIRE_FALSE(fixture.renderer->render(*snapshot, fixture.front_view(32, 32), first));
        REQUIRE_FALSE(fixture.device.end_frame());
        // Without waiting for the GPU: delete the entity, drop the snapshot, and evict the mesh.
        auto commands = world.commands();
        auto drawn = std::vector<EntityHandle>{};
        world.for_each<MeshRendererComponent>([&](EntityHandle entity, const auto&) { drawn.push_back(entity); });
        for (const auto entity : drawn) commands.destroy(entity);
        REQUIRE(world.commit(commands));
        snapshot.reset();
        CHECK(fixture.project->registry->evict_unused() >= 1);
        CHECK(fixture.device.stats().buffers == 0);
        const auto after = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
        CHECK(after.instances.empty());
        REQUIRE_FALSE(fixture.device.begin_frame());
        REQUIRE_FALSE(fixture.renderer->render(after, fixture.front_view(32, 32), second));
        REQUIRE_FALSE(fixture.device.end_frame());

        CHECK(is_red(pixel(read(fixture.device, first.color()), 32, 16, 16)));
        CHECK(is_black(pixel(read(fixture.device, second.color()), 32, 16, 16)));
        fixture.device.wait_idle(); // collects retirements whose frames completed
        CHECK(fixture.device.native_buffer_count() == baseline);
    }
    CHECK(*fixture.project->loads >= 20); // every round reloaded the evicted mesh and material
}

namespace {
/// Moves the cube 1 m along X and turns it 60° about Y each tick.
class CubeMover final : public SimulationSystem {
public:
    std::string_view name() const override { return "CubeMover"; }
    void fixed_update(TickContext& tick) override {
        const auto cube = *tick.world.find(EntityId{0x7465, 20});
        auto moved = TransformComponent{};
        tick.world.with<TransformComponent>(cube, [&](const TransformComponent& value) { moved = value; });
        moved.translation.x += 1.0f;
        moved.rotation = moved.rotation * math::Quat::from_axis_angle({0, 1, 0}, 3.14159265f / 3.0f);
        tick.commands.set_transform(cube, moved);
    }
};
} // namespace

TEST_CASE("Metal renders a playing scene between ticks exactly as the pose between them", "[rhi][renderer][presentation]") {
    GpuFixture fixture;
    const auto start = TransformComponent{{-1, 0, 0}, {}, math::Vec3(1.0f)};
    auto document = SceneDocument{};
    document.entities = {SceneEntity{EntityId{0x7465, 10}, {}, {TransformComponent{}, LightComponent{}}},
                         SceneEntity{EntityId{0x7465, 20}, {}, {start, MeshRendererComponent{fixture.cube, fixture.red, true}}}};
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<CubeMover>());
    auto started = PlaySession::start(document, asset_property_context(*fixture.project->registry), std::move(systems));
    INFO(started.error);
    REQUIRE(started);
    auto& session = *started.session;
    session.update(1.0 / 60.0); // tick 0: x -1 to 0, turned 60°
    session.update(1.0 / 120.0); // halfway to the next tick
    REQUIRE(session.clock().alpha() == 0.5);
    const auto poses = session.presentation();
    auto options = no_ambient;
    options.poses = &poses;
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "between ticks"});
    const auto view = fixture.front_view(64, 64);
    const auto shown = fixture.render(extract_render_snapshot(session.world(), *fixture.project->registry, options), view, target);
    // The same cube authored at the pose halfway between the ticks.
    World expected;
    build_world(expected, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        const auto end = TransformComponent{{0, 0, 0}, math::Quat::from_axis_angle({0, 1, 0}, 3.14159265f / 3.0f), math::Vec3(1.0f)};
        fixture.add_mesh(commands, fixture.cube, fixture.red, interpolate_transform(start, end, 0.5f));
    });
    const auto between = fixture.render(extract_render_snapshot(expected, *fixture.project->registry, no_ambient), view, target);
    CHECK(shown == between);
    // Without the poses, the completed tick shows instead: a different image.
    const auto completed = fixture.render(extract_render_snapshot(session.world(), *fixture.project->registry, no_ambient), view, target);
    CHECK(completed != between);
}

namespace {
/// Renders a large white cube filling a 16x16 view, lit only by `ambient`: every pixel's scene light is
/// the ambient color. Returns the centre pixel.
Pixel ambient_pixel(GpuFixture& fixture, const math::Vec3& ambient, float exposure, ToneMapping tone,
                    DebugView shown = DebugView::none) {
    World world;
    build_world(world, [&](WorldCommands& commands) { fixture.add_mesh(commands, fixture.cube, fixture.white, {{0, 0, 0}, {}, {4.0f}}); });
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, RenderExtractOptions{ambient});
    auto camera = CameraComponent{};
    camera.exposure = exposure;
    camera.tone_mapping = tone;
    auto view = make_render_view(camera, look_pose({0, 0, 5}, {0, 0, 0}), 16, 16);
    REQUIRE(view);
    view->debug_view = shown;
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "ambient"});
    return pixel(fixture.render(snapshot, *view, target), 16, 8, 8);
}
bool near(const Pixel& pixel, const std::array<int, 3>& expected, int tolerance) {
    for (size_t c = 0; c < 3; ++c)
        if (std::abs(pixel[c] - expected[c]) > tolerance) return false;
    return true;
}
} // namespace

TEST_CASE("Metal tone maps known scene light as the reference AgX and PBR Neutral do", "[rhi][renderer][tone]") {
    GpuFixture fixture;
    const auto exposure = exposure_scale(0.0f);
    for (const auto light : {0.0f, 0.02f, 0.18f, 0.5f, 1.0f, 2.0f, 4.0f, 16.0f, 100.0f})
        for (const auto tone : {ToneMapping::agx, ToneMapping::pbr_neutral}) {
            INFO("scene light " << light << (tone == ToneMapping::agx ? ", AgX" : ", PBR Neutral"));
            const auto shown = ambient_pixel(fixture, math::Vec3{light}, 0.0f, tone);
            CHECK(near(shown, test::displayed({light, light, light}, exposure, tone), 2));
            CHECK(shown[3] == 255);
        }
    // Colored light keeps its hue through both.
    for (const auto tone : {ToneMapping::agx, ToneMapping::pbr_neutral}) {
        const auto shown = ambient_pixel(fixture, {0.6f, 0.3f, 0.1f}, 0.0f, tone);
        CHECK(near(shown, test::displayed({0.6, 0.3, 0.1}, exposure, tone), 2));
        CHECK((shown[0] > shown[1] && shown[1] > shown[2]));
    }
}

TEST_CASE("Metal scales scene light by the camera's EV100, and light above one no longer clips", "[rhi][renderer][tone]") {
    GpuFixture fixture;
    // EV100 1 halves the light that EV100 0 lets through; 1.2 x 0.18 at EV100 0 is middle grey.
    for (const auto ev : {-2.0f, -1.0f, 0.0f, 1.0f, 3.0f}) {
        INFO("EV100 " << ev);
        CHECK(near(ambient_pixel(fixture, math::Vec3{0.216f}, ev, ToneMapping::agx), test::displayed({0.216, 0.216, 0.216}, exposure_scale(ev)), 2));
    }
    CHECK(std::abs(exposure_scale(1.0f) - exposure_scale(0.0f) / 2.0f) < 1e-6f);
    CHECK(std::abs(exposure_scale(std::log2(1.0f / 1.2f)) - 1.0f) < 1e-6f); // the scale that shows scene light as it is
    // Above 1 the image keeps getting brighter instead of clipping at white, up to AgX's +4 stops.
    auto previous = 0;
    for (const auto light : {0.5f, 1.0f, 1.5f, 2.0f, 2.5f}) {
        const auto shown = ambient_pixel(fixture, math::Vec3{light}, 0.0f, ToneMapping::agx)[0];
        INFO("scene light " << light << " shows as " << shown);
        CHECK(shown > previous + 3);
        CHECK(shown < 255);
        previous = shown;
    }
}

TEST_CASE("Metal keeps detail under a bright and a dim light in one view", "[rhi][renderer][tone]") {
    GpuFixture fixture;
    World world;
    build_world(world, [&](WorldCommands& commands) {
        // A bright light along -Z on the cubes' front faces, a dim one straight down on their tops.
        auto bright = commands.create();
        commands.add(bright, TransformComponent{});
        commands.add(bright, LightComponent{LightKind::directional, {1.0f}, 3.0f});
        auto dim = commands.create();
        commands.add(dim, TransformComponent{{}, math::Quat::from_axis_angle({1, 0, 0}, -math::PI / 2.0f), {1.0f}});
        commands.add(dim, LightComponent{LightKind::directional, {1.0f}, 0.06f});
        fixture.add_mesh(commands, fixture.cube, fixture.white, {{-1.2f, 0, 0}, {}, {1.6f}});
        fixture.add_mesh(commands, fixture.cube, fixture.grey, {{1.2f, 0, 0}, {}, {1.6f}});
    });
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry, no_ambient);
    auto view = make_render_view(CameraComponent{}, look_pose({0, 4, 6}, {0, 0, 0}), 128, 96);
    REQUIRE(view);
    view->clear_color = black;
    auto target = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "bright and dim"});
    const auto pixels = fixture.render(snapshot, *view, target);
    const auto at = [&](const math::Vec3& point) { // a world point's pixel
        const auto clip = view->matrices.view_projection * math::Vec4(point, 1.0f);
        const auto x = uint32_t((clip.x / clip.w * 0.5f + 0.5f) * 128.0f), y = uint32_t((0.5f - clip.y / clip.w * 0.5f) * 96.0f);
        return pixel(pixels, 128, x, y);
    };
    const auto white_front = at({-1.2f, 0.0f, 0.8f}), grey_front = at({1.2f, 0.0f, 0.8f});
    const auto white_top = at({-1.2f, 0.8f, 0.0f}), grey_top = at({1.2f, 0.8f, 0.0f});
    INFO("fronts " << white_front[0] << " " << grey_front[0] << ", tops " << white_top[0] << " " << grey_top[0]);
    // Fronts receive about 3 and 1.5 of scene light: a clamp would show both as white. Tops about 0.06
    // and 0.03: both still above black, and apart.
    CHECK(white_front[0] < 252);
    CHECK(white_front[0] > grey_front[0] + 8);
    CHECK(grey_top[0] > 3);
    CHECK(white_top[0] > grey_top[0] + 3);
    CHECK(grey_front[0] > white_top[0] + 40);
}

TEST_CASE("Metal exposure views show luminance by stops, and bands of false color", "[rhi][renderer][tone]") {
    GpuFixture fixture;
    const auto middle = 0.18f / exposure_scale(0.0f); // scene light that is middle grey once exposed
    // Luminance: grey by stops from middle grey, -8 black to +8 white, without sRGB encoding.
    CHECK(near(ambient_pixel(fixture, math::Vec3{middle}, 0.0f, ToneMapping::agx, DebugView::luminance), {128, 128, 128}, 1));
    CHECK(near(ambient_pixel(fixture, math::Vec3{middle * 16.0f}, 0.0f, ToneMapping::agx, DebugView::luminance), {191, 191, 191}, 1));
    CHECK(near(ambient_pixel(fixture, math::Vec3{middle / 16.0f}, 0.0f, ToneMapping::agx, DebugView::luminance), {64, 64, 64}, 1));
    // False color by band: middle grey, three stops over, five under, and clipped.
    const auto band = [&](float stops) {
        return ambient_pixel(fixture, math::Vec3{middle * std::exp2(stops)}, 0.0f, ToneMapping::pbr_neutral, DebugView::false_color);
    };
    CHECK(near(band(0.0f), {128, 128, 128}, 1));
    CHECK(near(band(3.0f), {255, 140, 0}, 1));
    CHECK(near(band(-5.0f), {0, 51, 230}, 1));
    CHECK(near(band(7.0f), {255, 153, 230}, 1));
    // The views ignore the tone mapper: they show exposed luminance.
    CHECK(ambient_pixel(fixture, math::Vec3{middle}, 0.0f, ToneMapping::agx, DebugView::false_color) == band(0.0f));
}

TEST_CASE("Metal renders the same image from a scene camera and from that camera's data and pose", "[rhi][renderer][tone]") {
    GpuFixture fixture;
    World world;
    auto camera_component = CameraComponent{};
    camera_component.exposure = -1.5f;
    camera_component.tone_mapping = ToneMapping::pbr_neutral;
    const auto pose = look_pose({2, 3, 6}, {0, 0, 0});
    const auto created = build_world(world, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        fixture.add_mesh(commands, fixture.cube, fixture.red, {{-1, 0, 0}, {}, {1.0f}});
        fixture.add_mesh(commands, fixture.cube, fixture.green, {{1, 0, 0}, {}, {1.0f}});
        auto camera = commands.create();
        commands.add(camera, look_transform({2, 3, 6}, {0, 0, 0}));
        commands.add(camera, camera_component);
    });
    const auto snapshot = extract_render_snapshot(world, *fixture.project->registry);
    // The player and the Game view use the camera entity; the Scene view and offscreen views, camera data and a pose.
    const auto from_entity = extract_render_view(world, created.back(), 64, 48);
    const auto from_data = make_render_view(camera_component, pose, 64, 48);
    REQUIRE(from_entity);
    REQUIRE(from_data);
    CHECK(from_entity->exposure == from_data->exposure);
    CHECK(from_entity->tone_mapping == ToneMapping::pbr_neutral);
    auto first = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "entity"});
    auto second = RenderTarget(fixture.device, {Format::rgba8_unorm, true, "data"});
    CHECK(fixture.render(snapshot, *from_entity, first) == fixture.render(snapshot, *from_data, second));
}
