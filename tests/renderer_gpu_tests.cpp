#include <catch2/catch_test_macros.hpp>
#include "maya/core/file_system.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/rhi/metal/metal_device.hpp"
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
bool is_red(Pixel p) { return p[0] > 200 && p[1] < 40 && p[2] < 40; }
bool is_green(Pixel p) { return p[1] > 200 && p[0] < 40 && p[2] < 40; }
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
                                                 {"white.material", {{1, 1, 1, 1}, 0, 1}}});
        cube = project->add<MeshAsset>(1, "cube.mesh");
        quad = project->add<MeshAsset>(2, "quad.mesh");
        red = project->add<MaterialAsset>(3, "red.material");
        green = project->add<MaterialAsset>(4, "green.material");
        white = project->add<MaterialAsset>(5, "white.material");
        missing = project->add<MeshAsset>(6, "missing.mesh"); // registered but absent
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
    AssetRef<MaterialAsset> red, green, white;
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

TEST_CASE("Metal renderer lights nonuniformly scaled surfaces by their true normals", "[rhi][renderer]") {
    GpuFixture fixture;
    World world;
    build_world(world, [&](WorldCommands& commands) {
        fixture.add_light(commands);
        // Scaling z by 4 tilts the quad toward +Y: its true normal is (0, 0.970, 0.243), so a light
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
    // Diffuse 0.243 plus a weak 4% highlight: about 71 of 255.
    CHECK(center[0] > 50);
    CHECK(center[0] < 100);
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
    CHECK(fixture.device.native_texture_count() == baseline + 2); // replaced targets were retired
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
