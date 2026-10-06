// Point and spot lights, and shadows, on Metal (#1034, docs/renderer.md#lights, #shadows): illuminance at
// known distances and angles against the CPU reference, shadows cast and received without acne or
// peter-panning, thin casters, spot light shadows, the limits' reports, and steady cascades.

#include <catch2/catch_test_macros.hpp>
#include "maya/core/file_system.hpp"
#include "maya/renderer/light_plan.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "support/render_scene.hpp"
#include "support/shading.hpp"
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>

using namespace maya;
using namespace maya::test;

namespace {
using Hdr = std::vector<std::array<float, 4>>;

/// A 2 x 2 quad facing +Z.
Geometry wall() {
    auto geometry = Geometry{};
    for (const auto& p : {math::Vec3{-1, -1, 0}, math::Vec3{1, -1, 0}, math::Vec3{1, 1, 0}, math::Vec3{-1, 1, 0}})
        geometry.vertices.emplace_back(p, math::Vec3{0, 0, 1}, math::Vec4{1.0f});
    geometry.indices = {0, 1, 2, 0, 2, 3};
    return geometry;
}
/// A 2 x 2 quad facing +Y: scaled up, the floor.
Geometry ground() {
    auto geometry = Geometry{};
    for (const auto& p : {math::Vec3{-1, 0, 1}, math::Vec3{1, 0, 1}, math::Vec3{1, 0, -1}, math::Vec3{-1, 0, -1}})
        geometry.vertices.emplace_back(p, math::Vec3{0, 1, 0}, math::Vec4{1.0f});
    geometry.indices = {0, 1, 2, 0, 2, 3};
    return geometry;
}

struct LightingFixture {
    LightingFixture() {
        REQUIRE(device.initialize(nullptr));
        auto source = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        REQUIRE_FALSE(source.empty());
        renderer = std::make_unique<Renderer>(device, std::move(source));
        project = std::make_unique<TestProject>(device, std::map<std::string, Geometry>{{"wall.mesh", wall()}, {"ground.mesh", ground()},
                                                                                        {"cube.mesh", unit_cube()}},
                                                std::map<std::string, MaterialAsset>{{"white.material", MaterialAsset{{1, 1, 1, 1}, 0.0f, 1.0f}}});
        wall_mesh = project->add<MeshAsset>(1, "wall.mesh");
        ground_mesh = project->add<MeshAsset>(2, "ground.mesh");
        cube_mesh = project->add<MeshAsset>(3, "cube.mesh");
        white = project->add<MaterialAsset>(10, "white.material");
    }
    ~LightingFixture() {
        target.reset();
        project.reset();
        renderer.reset();
        CHECK(device.take_gpu_errors().empty());
        device.shutdown();
    }
    void add(WorldCommands& commands, AssetRef<MeshAsset> mesh, TransformComponent transform) {
        auto entity = commands.create();
        commands.add(entity, transform);
        commands.add(entity, MeshRendererComponent{mesh, white, true});
    }
    static void light(WorldCommands& commands, LightComponent value, const math::Vec3& position, const math::Quat& rotation = {}) {
        auto entity = commands.create();
        commands.add(entity, TransformComponent{position, rotation, {1.0f}});
        commands.add(entity, value);
    }
    /// Renders the scene `build` makes from `eye` toward `look`, and reads back the HDR scene light.
    Hdr render(const std::function<void(WorldCommands&)>& build, const math::Vec3& eye, const math::Vec3& look, float fov,
               uint32_t width, uint32_t height, ShadowView shadow_view = ShadowView::none, const math::Vec3& up = {0, 1, 0}) {
        World world;
        build_world(world, build);
        const auto snapshot = extract_render_snapshot(world, *project->registry, {math::Vec3{0.0f}});
        REQUIRE(snapshot.diagnostics.empty());
        auto camera = CameraComponent{};
        camera.vertical_fov = fov;
        camera.near_clip = 0.05f;
        camera.far_clip = 300.0f;
        auto view = make_render_view(camera, look_pose(eye, look, up), width, height);
        REQUIRE(view);
        view->clear_color = {0, 0, 0, 1};
        view->shadow_view = shadow_view;
        if (!target) target = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, true, "lighting"});
        REQUIRE_FALSE(target->resize(width, height));
        REQUIRE_FALSE(device.begin_frame());
        const auto error = renderer->render(snapshot, *view, *target);
        INFO(error.message);
        REQUIRE_FALSE(error);
        REQUIRE_FALSE(device.end_frame());
        auto bytes = std::vector<std::byte>{};
        REQUIRE_FALSE(device.read_texture(target->scene_color(), bytes));
        auto image = Hdr(size_t(width) * height);
        for (size_t i = 0; i < image.size(); ++i)
            for (size_t c = 0; c < 4; ++c) {
                _Float16 half;
                std::memcpy(&half, bytes.data() + i * 8 + c * 2, 2);
                image[i][c] = float(half);
            }
        last_view = *view;
        last_plan = plan_lights(snapshot, *view);
        return image;
    }
    /// The pixel a world point falls on in the last view.
    std::pair<uint32_t, uint32_t> pixel(const math::Vec3& p) const {
        const auto clip = last_view.matrices.view_projection * math::Vec4{p, 1.0f};
        const auto x = (clip.x / clip.w * 0.5f + 0.5f) * float(last_view.width), y = (0.5f - clip.y / clip.w * 0.5f) * float(last_view.height);
        return {uint32_t(x), uint32_t(y)};
    }
    float red(const Hdr& image, const math::Vec3& p) const {
        const auto [x, y] = pixel(p);
        return image[size_t(y) * last_view.width + x][0];
    }

    MetalDevice device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<TestProject> project;
    std::unique_ptr<RenderTarget> target;
    AssetRef<MeshAsset> wall_mesh, ground_mesh, cube_mesh;
    AssetRef<MaterialAsset> white;
    RenderView last_view;
    LightPlan last_plan;
};

const auto rough_white = Surface{{1, 1, 1}, 0, 1};
/// Turns +Z onto `to`, so a light's local -Z shines away from it.
math::Quat rotation_to(const math::Vec3& to) {
    const auto axis = math::Vec3{-to.y, to.x, 0.0f}; // z x to
    const auto length = axis.length();
    if (length < 1e-6f) return to.z > 0 ? math::Quat{} : math::Quat::from_axis_angle({0, 1, 0}, math::PI);
    return math::Quat::from_axis_angle(axis * (1.0f / length), std::acos(std::clamp(to.z, -1.0f, 1.0f)));
}
Direction direction(const math::Vec3& v) { return {v.x, v.y, v.z}; }
} // namespace

TEST_CASE("Point lights give candela / d^2 of illuminance, faded to nothing at their range", "[rhi][lights]") {
    LightingFixture fixture;
    // The wall faces +Z; the camera looks at its centre from far along +Z, so V = N = +Z there.
    for (const auto d : {0.5, 1.0, 2.0, 4.0, 7.5}) {
        INFO("distance " << d);
        const auto image = fixture.render([&](WorldCommands& commands) {
            fixture.add(commands, fixture.wall_mesh, {});
            LightingFixture::light(commands, {LightKind::point, {1.0f}, 10.0f, 8.0f}, {0, 0, float(d)});
        }, {0, 0, 50}, {0, 0, 0}, 0.02f, 17, 17); // an odd size, so the middle pixel is the wall's centre
        const auto expected = reflected(rough_white, {0, 0, 1}, {0, 0, 1}, {0, 0, 1})[0] * 10.0 * point_falloff(d, 8.0);
        const auto shown = image[8 * 17 + 8][0];
        INFO("shown " << shown << ", expected " << expected);
        CHECK(std::abs(shown - expected) <= 0.01 * expected + 1e-4);
    }
    // At its range and beyond: nothing.
    const auto beyond = fixture.render([&](WorldCommands& commands) {
        fixture.add(commands, fixture.wall_mesh, {});
        LightingFixture::light(commands, {LightKind::point, {1.0f}, 1000.0f, 3.0f}, {0, 0, 3.0f});
    }, {0, 0, 50}, {0, 0, 0}, 0.02f, 17, 17);
    CHECK(beyond[8 * 17 + 8][0] == 0.0f);
}

TEST_CASE("Spot lights are full inside their inner cone, fade to their outer cone, and give nothing beyond it", "[rhi][lights]") {
    LightingFixture fixture;
    // A spot 2 m in front of the wall's centre, moved sideways so the centre is at a known angle off its axis.
    const auto inner = 0.5f, outer = 1.0f; // full angles
    for (const auto degrees : {5.0, 18.0, 25.0, 35.0}) {
        INFO("angle " << degrees);
        const auto angle = degrees * std::numbers::pi / 180.0;
        const auto side = 2.0 * std::tan(angle);
        const auto image = fixture.render([&](WorldCommands& commands) {
            fixture.add(commands, fixture.wall_mesh, {});
            auto spot = LightComponent{LightKind::spot, {1.0f}, 50.0f, 20.0f, inner, outer};
            spot.cast_shadows = false;
            LightingFixture::light(commands, spot, {float(side), 0, 2}); // shining along -Z
        }, {0, 0, 50}, {0, 0, 0}, 0.02f, 17, 17);
        const auto d = std::hypot(side, 2.0);
        const auto L = normalize({side, 0, 2});
        const auto expected = reflected(rough_white, {0, 0, 1}, {0, 0, 1}, L)[0] * 50.0 * point_falloff(d, 20.0) *
                              spot_falloff(std::cos(angle), std::cos(inner / 2.0), std::cos(outer / 2.0));
        const auto shown = image[8 * 17 + 8][0];
        INFO("shown " << shown << ", expected " << expected);
        CHECK(std::abs(shown - expected) <= 0.01 * expected + 1e-4);
        if (degrees > 30) CHECK(shown == 0.0f);
    }
}

TEST_CASE("The sun's shadows fall where its light is blocked, with no acne on lit surfaces and no gap at contact", "[rhi][shadows]") {
    LightingFixture fixture;
    const auto to_light = math::Vec3{1.0f, 0.7f, 0.15f}.normalized();
    const auto scene = [&](bool thin) {
        return [&, thin](WorldCommands& commands) {
            fixture.add(commands, fixture.ground_mesh, {{0, 0, 0}, {}, {20, 1, 20}});
            if (thin) {
                // A floating quad with no thickness, its back to the sun: it still casts.
                fixture.add(commands, fixture.ground_mesh, {{3, 1, 3}, math::Quat::from_axis_angle({1, 0, 0}, math::PI), {0.5f, 1, 0.5f}});
                // A wall with no thickness standing on the floor, facing the sun. Too much bias lets light leak
                // under it: a solid cube hides that, since a biased sample behind it moves into it.
                fixture.add(commands, fixture.wall_mesh, {{2, 0.5f, 0}, math::Quat::from_axis_angle({0, 1, 0}, math::PI / 2), {0.5f, 0.5f, 1}});
            } else {
                fixture.add(commands, fixture.cube_mesh, {{0, 0.5f, 0}, {}, {1.0f}}); // resting on the floor
            }
            LightingFixture::light(commands, LightComponent{}, {}, rotation_to(to_light));
        };
    };
    // Seen from straight above, so the floor right beside the cube is in view.
    const auto eye = math::Vec3{0, 12, 0};
    const auto image = fixture.render(scene(false), eye, {0, 0, 0}, 0.9f, 255, 255, ShadowView::none, {0, 0, -1});
    const auto lit_at = [&](const math::Vec3& p, const Direction& N) {
        return reflected(rough_white, N, normalize({eye.x - p.x, eye.y - p.y, eye.z - p.z}), direction(to_light))[0] * math::PI;
    };
    // Lit floor away from the cube: as the reference (no acne).
    for (const auto& p : {math::Vec3{3, 0, -3}, math::Vec3{-3, 0, -2}, math::Vec3{2.5f, 0, 2}, math::Vec3{0.6f, 0, 0}}) {
        const auto shown = fixture.red(image, p);
        INFO("lit floor at " << p.x << " " << p.z << ": " << shown << ", expected " << lit_at(p, {0, 1, 0}));
        CHECK(std::abs(shown - lit_at(p, {0, 1, 0})) < 0.02 * lit_at(p, {0, 1, 0}));
    }
    // The cube's top, facing the sun, is fully lit: no self-shadowing.
    CHECK(std::abs(fixture.red(image, {0.1f, 1.0f, 0.1f}) - lit_at({0.1f, 1.0f, 0.1f}, {0, 1, 0})) < 0.02 * lit_at({0.1f, 1.0f, 0.1f}, {0, 1, 0}));
    // The floor on the cube's far side from the sun: in its shadow, from 8 cm of its base...
    CHECK(fixture.red(image, {-0.58f, 0, -0.05f}) < 0.05 * lit_at({-0.58f, 0, -0.05f}, {0, 1, 0}));
    // ...to where its top's shadow falls.
    const auto under_top = math::Vec3{0, 0.9f, 0} - to_light * (0.9f / to_light.y);
    CHECK(fixture.red(image, under_top) < 0.05 * lit_at(under_top, {0, 1, 0}));

    // Thin quads cast too, and a standing one's shadow starts at its foot (no peter-panning).
    const auto thin = fixture.render(scene(true), eye, {0, 0, 0}, 0.9f, 1023, 1023, ShadowView::none, {0, 0, -1}); // 1.1 cm pixels
    const auto under = math::Vec3{3, 1, 3} - to_light * (1.0f / to_light.y);
    CHECK(fixture.red(thin, under) < 0.05 * lit_at(under, {0, 1, 0}));
    CHECK(fixture.red(thin, {1.95f, 0, 0}) < 0.05 * lit_at({1.95f, 0, 0}, {0, 1, 0})); // 5 cm behind the wall's foot
    CHECK(fixture.red(thin, {1.5f, 0, 0}) < 0.05 * lit_at({1.5f, 0, 0}, {0, 1, 0}));
    CHECK(std::abs(fixture.red(thin, {-3, 0, -2}) - lit_at({-3, 0, -2}, {0, 1, 0})) < 0.02 * lit_at({-3, 0, -2}, {0, 1, 0}));
}

TEST_CASE("A floor lit by the sun shows no shadow acne in any cascade, near or far", "[rhi][shadows]") {
    LightingFixture fixture;
    const auto to_light = math::Vec3{0.2f, 0.4f, -0.9f}.normalized(); // low: acne shows first at grazing angles
    const auto image = fixture.render([&](WorldCommands& commands) {
        fixture.add(commands, fixture.ground_mesh, {{0, 0, 0}, {}, {100, 1, 100}});
        LightingFixture::light(commands, LightComponent{}, {}, rotation_to(to_light));
    }, {0, 1.7f, 0}, {0, 0.5f, -10}, 1.0f, 320, 180);
    // Every pixel showing the floor, out to past the shadow distance, as the reference.
    auto worst = 1.0f;
    const auto L = direction(to_light);
    for (uint32_t y = 100; y < 180; y += 2)
        for (uint32_t x = 0; x < 320; x += 4) {
            // The floor point under this pixel, and its expected light.
            const auto& m = fixture.last_view.matrices;
            const auto inverse = *inverse_affine(m.view);
            const auto tan_y = std::tan(0.5f), tan_x = tan_y * 320.0f / 180.0f;
            const auto ndc_x = (float(x) + 0.5f) / 320.0f * 2.0f - 1.0f, ndc_y = 1.0f - (float(y) + 0.5f) / 180.0f * 2.0f;
            const auto ray_view = math::Vec3{ndc_x * tan_x, ndc_y * tan_y, -1.0f};
            const auto origin = math::Vec3{0, 1.7f, 0};
            const auto through = math::Vec3{inverse.at(0, 0) * ray_view.x + inverse.at(0, 1) * ray_view.y + inverse.at(0, 2) * ray_view.z,
                                            inverse.at(1, 0) * ray_view.x + inverse.at(1, 1) * ray_view.y + inverse.at(1, 2) * ray_view.z,
                                            inverse.at(2, 0) * ray_view.x + inverse.at(2, 1) * ray_view.y + inverse.at(2, 2) * ray_view.z};
            if (through.y >= -1e-3f) continue;
            const auto point = origin + through * (-origin.y / through.y);
            const auto V = normalize({origin.x - point.x, origin.y - point.y, origin.z - point.z});
            const auto expected = reflected(rough_white, {0, 1, 0}, V, L)[0] * math::PI;
            worst = std::min(worst, float(image[size_t(y) * 320 + x][0] / expected));
        }
    INFO("the darkest floor pixel relative to its light: " << worst);
    CHECK(worst > 0.97f);
}

TEST_CASE("A shadow running away from the camera through every cascade keeps its edge where it belongs", "[rhi][shadows]") {
    LightingFixture fixture;
    // A wall 2 m high and 60 m long along -Z, lit from +X: its shadow is the floor from x = 0 to x = -2 / tan(elevation).
    const auto to_light = math::Vec3{1.0f, 0.6f, 0.0f}.normalized();
    const auto edge = -2.0f / 0.6f;
    const auto eye = math::Vec3{-4.5f, 1.7f, 0};
    constexpr uint32_t height = 360;
    const auto image = fixture.render([&](WorldCommands& commands) {
        fixture.add(commands, fixture.ground_mesh, {{0, 0, -40}, {}, {50, 1, 50}});
        fixture.add(commands, fixture.wall_mesh, {{0, 1, -31}, math::Quat::from_axis_angle({0, 1, 0}, math::PI / 2), {30, 1, 1}});
        LightingFixture::light(commands, LightComponent{}, {}, rotation_to(to_light));
    }, eye, {-4.5f, 0.5f, -10}, 1.0f, 640, height);
    const auto lit_at = [&](const math::Vec3& p) {
        return reflected(rough_white, {0, 1, 0}, normalize({eye.x - p.x, eye.y - p.y, eye.z - p.z}), direction(to_light))[0] * math::PI;
    };
    REQUIRE(fixture.last_plan.sun);
    // From the first cascade to the last (the shadow distance is 60 m): dark just inside the edge, lit just outside.
    // The edge is soft over the filter's reach and the biases' shift: a few of the cascade's texels, which land
    // 1 / sin(elevation) times as wide on the floor.
    for (const auto z : {2.5f, 4.0f, 6.0f, 9.0f, 13.0f, 18.0f, 25.0f, 33.0f, 42.0f, 50.0f}) {
        const auto& cascades = fixture.last_plan.cascades;
        const auto cascade = std::ranges::find_if(cascades, [&](const ShadowCascade& c) { return z <= c.far; });
        REQUIRE(cascade != cascades.end());
        // Where a cascade blends into the next, the next's bigger texels count.
        const auto next = cascade + 1;
        const auto texel = next != cascades.end() && z > next->near ? next->texel : cascade->texel;
        const auto margin = 4.0f * texel / to_light.y + 2.0f * z * std::tan(0.5f) / float(height);
        INFO("at " << z << " m, in cascade " << (cascade - cascades.begin()) << ", " << margin << " m from the edge");
        const auto inside = math::Vec3{edge + margin, 0, -z}, outside = math::Vec3{edge - margin, 0, -z};
        CHECK(fixture.red(image, inside) < 0.05 * lit_at(inside));
        CHECK(std::abs(fixture.red(image, outside) - lit_at(outside)) < 0.02 * lit_at(outside));
    }
}

TEST_CASE("Spot lights cast shadows from their own maps", "[rhi][shadows]") {
    LightingFixture fixture;
    // A 1.5 m cube floating 0.75 m above the floor, under a spot light shining straight down: its shadow reaches
    // 1.2 m from the middle, and the camera straight above sees the floor from 0.9 m out.
    const auto image = fixture.render([&](WorldCommands& commands) {
        fixture.add(commands, fixture.ground_mesh, {{0, 0, 0}, {}, {20, 1, 20}});
        fixture.add(commands, fixture.cube_mesh, {{0, 1.5f, 0}, {}, {1.5f}});
        auto spot = LightComponent{LightKind::spot, {1.0f}, 200.0f, 30.0f, 1.0f, 1.6f};
        LightingFixture::light(commands, spot, {0, 6, 0}, rotation_to({0, 1, 0}));
    }, {0, 14, 0}, {0, 0, 0}, 0.8f, 255, 255, ShadowView::none, {0, 0, -1});
    const auto lit = fixture.red(image, {2.0f, 0, 0}); // in the inner cone, beside the shadow
    CHECK(lit > 0.0f);
    CHECK(fixture.red(image, {1.05f, 0, 0}) < 0.05f * lit);
    CHECK(fixture.red(image, {0, 0, -1.05f}) < 0.05f * lit);
    // Outside the shadow, the floor is lit as the reference shows, with no acne.
    for (const auto& p : {math::Vec3{1.4f, 0, 0}, math::Vec3{2.0f, 0, 0}, math::Vec3{-2.5f, 0, 1}, math::Vec3{0.3f, 0, 3}, math::Vec3{-1.5f, 0, -2}}) {
        const auto to_light = math::Vec3{0, 6, 0} - p;
        const auto d = double(to_light.length());
        const auto expected = reflected(rough_white, {0, 1, 0}, normalize({-p.x, 14.0 - p.y, -p.z}), direction(to_light * (1.0f / float(d))))[0] *
                              200.0 * point_falloff(d, 30.0) * spot_falloff(6.0 / d, std::cos(0.5), std::cos(0.8));
        const auto shown = fixture.red(image, p);
        INFO("floor at " << p.x << " " << p.z << ": " << shown << ", expected " << expected);
        CHECK(std::abs(shown - expected) < 0.02 * expected);
    }
    CHECK(fixture.renderer->last_lights().local == 1);
    CHECK(fixture.renderer->last_lights().unshadowed.empty());
}

TEST_CASE("Lights beyond the limits are drawn without shadows or not at all, and reported", "[rhi][lights]") {
    LightingFixture fixture;
    fixture.render([&](WorldCommands& commands) {
        fixture.add(commands, fixture.ground_mesh, {{0, 0, 0}, {}, {20, 1, 20}});
        for (int i = 0; i < 20; ++i) LightingFixture::light(commands, {LightKind::point, {1.0f}, 5.0f, 3.0f}, {float(i % 5) - 2.0f, 1, float(i / 5) - 2.0f});
        for (int i = 0; i < 6; ++i) {
            auto spot = LightComponent{LightKind::spot, {1.0f}, 500.0f, 10.0f};
            LightingFixture::light(commands, spot, {float(i) - 2.5f, 4, 0}, rotation_to({0, 1, 0}));
        }
    }, {0, 10, 6}, {0, 0, 0}, 1.0f, 64, 64);
    const auto& report = fixture.renderer->last_lights();
    CHECK(report.local == max_local_lights);
    CHECK(report.dropped.size() == 26 - max_local_lights);
    // The six bright spots outrank the dim points: all drawn, four with shadow maps.
    CHECK(report.unshadowed.size() == 2);
}

TEST_CASE("A camera sliding over the floor sees the sun's shadows hold still: no shimmer", "[rhi][shadows]") {
    LightingFixture fixture;
    // Poles out of view to the north cast long shadows across the floor the camera looks straight down on.
    const auto to_light = math::Vec3{0.15f, 0.35f, -1.0f}.normalized();
    const auto scene = [&](WorldCommands& commands) {
        fixture.add(commands, fixture.ground_mesh, {{0, 0, 0}, {}, {60, 1, 60}});
        for (int i = -8; i <= 8; ++i) fixture.add(commands, fixture.cube_mesh, {{float(i) * 0.83f, 3, -12}, {}, {0.13f, 6, 0.13f}});
        LightingFixture::light(commands, LightComponent{}, {}, rotation_to(to_light));
    };
    constexpr uint32_t width = 192, height = 192;
    const auto height_above = 8.5f, fov = 0.6f; // in the band where the second cascade blends into the third
    const auto metres_per_pixel = 2.0f * height_above * std::tan(fov / 2.0f) / float(height);
    auto previous = Hdr{};
    auto worst = 0.0f;
    size_t changed = 0;
    for (int frame = 0; frame < 12; ++frame) {
        // One pixel further east each frame, which is not a whole number of any cascade's texels: the floor
        // moves exactly one pixel west in the image, and its shadows should move with it.
        const auto x = float(frame) * metres_per_pixel;
        const auto image = fixture.render(scene, {x, height_above, 0}, {x, 0, 0}, fov, width, height, ShadowView::none, {0, 0, -1});
        if (!previous.empty()) {
            // Compare with the previous frame moved one pixel, away from the image's edges.
            for (uint32_t py = 2; py < height - 2; ++py)
                for (uint32_t px = 2; px < width - 2; ++px) {
                    const auto difference = std::abs(image[size_t(py) * width + px][0] - previous[size_t(py) * width + px + 1][0]);
                    worst = std::max(worst, difference);
                    changed += difference > 0.01f;
                }
        }
        previous = image;
    }
    INFO("largest change " << worst << ", pixels changed by more than 0.01: " << changed);
    CHECK(worst < 0.01f); // the floor and its shadows move together: nothing flickers
}
