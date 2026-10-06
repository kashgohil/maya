// Planning a view's lights (#1034, docs/renderer.md#lights): which point and spot lights it draws and
// shadows, and the sun's cascades, fitted to the view and steady as it moves.

#include "maya/renderer/light_plan.hpp"
#include "maya/world/spatial.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace maya;
using Catch::Approx;

namespace {
math::Mat4 pose(const math::Vec3& from, const math::Vec3& to) {
    const auto f = (to - from).normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return local_matrix(TransformComponent{from, q, {1, 1, 1}});
}
RenderView view_at(const math::Vec3& from, const math::Vec3& to, float far = 200.0f) {
    auto camera = CameraComponent{};
    camera.vertical_fov = 1.0f;
    camera.near_clip = 0.1f;
    camera.far_clip = far;
    auto view = make_render_view(camera, pose(from, to), 1600, 900);
    REQUIRE(view);
    return *view;
}
RenderLocalLight point(uint64_t id, const math::Vec3& position, float candela, float range = 5.0f) {
    auto light = RenderLocalLight{};
    light.entity = {0, id};
    light.position = position;
    light.intensity = math::Vec3{candela};
    light.range = range;
    return light;
}
RenderDirectionalLight sun(bool shadows = true) {
    auto light = RenderDirectionalLight{};
    light.entity = {0, 1};
    light.direction_to_light = math::Vec3{0.3f, 1.0f, 0.2f}.normalized();
    light.shadow.cast = shadows;
    light.shadow_distance = 50.0f;
    return light;
}
RenderInstance instance(const math::Vec3& center, float radius) {
    auto value = RenderInstance{};
    value.bounds_center = center;
    value.bounds_radius = radius;
    return value;
}
math::Vec3 apply(const math::Mat4& m, const math::Vec3& p) {
    const auto v = m * math::Vec4{p, 1.0f};
    return {v.x, v.y, v.z};
}
} // namespace

TEST_CASE("A view draws the point and spot lights that reach it, the most important first, and reports the rest", "[renderer][lights]") {
    auto snapshot = RenderSnapshot{};
    // Behind the camera and out of reach: not drawn, and not dropped either.
    snapshot.local_lights.push_back(point(1, {0, 0, 30}, 1000.0f));
    // Twenty in front, each as bright as the next but farther: the 16 nearest are drawn.
    for (uint64_t i = 0; i < 20; ++i) snapshot.local_lights.push_back(point(100 - i, {0, 0, -2.0f - float(i)}, 100.0f));
    const auto plan = plan_lights(snapshot, view_at({0, 0, 0}, {0, 0, -1}));
    REQUIRE(plan.local.size() == max_local_lights);
    REQUIRE(plan.dropped.size() == 4);
    CHECK(snapshot.local_lights[plan.local.front()].position.z == -2.0f); // the nearest
    for (const auto index : plan.dropped) CHECK(snapshot.local_lights[index].position.z < -17.0f);
    CHECK(std::ranges::count(plan.local, 0u) == 0);
    CHECK(std::ranges::count(plan.dropped, 0u) == 0);
    // A much brighter far light outranks near dim ones.
    snapshot.local_lights.push_back(point(500, {0, 0, -40}, 1e6f, 60.0f));
    const auto bright = plan_lights(snapshot, view_at({0, 0, 0}, {0, 0, -1}));
    CHECK(snapshot.local_lights[bright.local.front()].entity == EntityId{0, 500});
    // Ties go to the lower EntityId.
    auto tied = RenderSnapshot{};
    tied.local_lights = {point(7, {1, 0, -5}, 10.0f), point(3, {-1, 0, -5}, 10.0f)};
    const auto order = plan_lights(tied, view_at({0, 0, 0}, {0, 0, -1}));
    REQUIRE(order.local.size() == 2);
    CHECK(tied.local_lights[order.local[0]].entity == EntityId{0, 3});
}

TEST_CASE("Spot shadow maps go to the most important shadowed spot lights; the rest are drawn unshadowed and reported", "[renderer][lights]") {
    auto snapshot = RenderSnapshot{};
    for (uint64_t i = 0; i < 6; ++i) {
        auto spot = point(10 + i, {float(i) - 2.5f, 3, -4.0f - float(i)}, 200.0f, 12.0f);
        spot.kind = LightKind::spot;
        spot.direction = {0, -1, 0};
        spot.cos_inner = std::cos(0.3f);
        spot.cos_outer = std::cos(0.5f);
        spot.shadow.cast = i != 2; // one without shadows
        snapshot.local_lights.push_back(spot);
    }
    const auto plan = plan_lights(snapshot, view_at({0, 2, 0}, {0, 2, -1}));
    CHECK(plan.local.size() == 6);
    REQUIRE(plan.spot_shadows.size() == max_shadowed_spot_lights);
    REQUIRE(plan.unshadowed.size() == 1); // five ask, four get maps
    CHECK(snapshot.local_lights[plan.unshadowed[0]].entity == EntityId{0, 15}); // the farthest
    for (const auto& map : plan.spot_shadows) {
        const auto& light = snapshot.local_lights[map.light];
        CHECK(light.shadow.cast);
        // The light's axis projects to the map's centre, at a depth within the map.
        const auto clip = map.view_projection * math::Vec4{light.position + light.direction * 5.0f, 1.0f};
        CHECK(clip.x / clip.w == Approx(0.0f).margin(1e-4));
        CHECK(clip.y / clip.w == Approx(0.0f).margin(1e-4));
        CHECK(clip.z / clip.w > 0.0f);
        CHECK(clip.z / clip.w < 1.0f);
        CHECK(map.far == light.range);
    }
    // casts_into: within the light's range, or unbounded.
    CHECK(casts_into(snapshot.local_lights[0], instance(snapshot.local_lights[0].position + math::Vec3{0, -12.5f, 0}, 1.0f)));
    CHECK_FALSE(casts_into(snapshot.local_lights[0], instance(snapshot.local_lights[0].position + math::Vec3{0, -14.0f, 0}, 1.0f)));
    CHECK(casts_into(snapshot.local_lights[0], RenderInstance{}));
}

TEST_CASE("The sun's four cascades cover the view to its shadow distance, each holding its slice of the frustum", "[renderer][shadows]") {
    auto snapshot = RenderSnapshot{};
    snapshot.lights = {sun()};
    snapshot.instances = {instance({0, 0, -10}, 1.0f), instance({0, 40, -10}, 1.0f)}; // one high above everything
    const auto view = view_at({0, 2, 0}, {0, 1, -10});
    const auto plan = plan_lights(snapshot, view);
    REQUIRE(plan.sun == 0u);
    auto previous_far = 0.1f;
    for (uint32_t c = 0; c < sun_cascades; ++c) {
        const auto& cascade = plan.cascades[c];
        INFO("cascade " << c);
        CHECK(cascade.far > previous_far);
        CHECK(cascade.near < previous_far + 1e-4f); // it overlaps the band where the one before blends into it
        CHECK(cascade.texel == Approx(2.0f * cascade.radius / float(sun_cascade_size)));
        // Every corner of its slice of the view's frustum lies inside its sphere, and inside its map.
        const auto camera = *inverse_affine(view.matrices.view);
        const auto tan_y = std::tan(0.5f), tan_x = tan_y * 1600.0f / 900.0f;
        for (const auto depth : {cascade.near, cascade.far})
            for (const auto sx : {-1.0f, 1.0f})
                for (const auto sy : {-1.0f, 1.0f}) {
                    const auto corner = apply(camera, {sx * tan_x * depth, sy * tan_y * depth, -depth});
                    CHECK((corner - cascade.center).length() <= cascade.radius * 1.0001f);
                    const auto clip = cascade.view_projection * math::Vec4{corner, 1.0f};
                    CHECK(std::abs(clip.x) <= 1.0f);
                    CHECK(std::abs(clip.y) <= 1.0f);
                    CHECK(clip.z >= 0.0f);
                    CHECK(clip.z <= 1.0f);
                }
        // The caster high above casts into the cascades below it: depth reaches up to it.
        const auto high = cascade.view_projection * math::Vec4{math::Vec3{0, 41, -10}, 1.0f};
        CHECK(high.z >= -1e-4f);
        previous_far = cascade.far;
    }
    CHECK(plan.cascades.back().far == Approx(50.0f)); // the shadow distance, nearer than the view's far plane
    // Cascades get bigger farther out.
    CHECK(plan.cascades[0].radius < plan.cascades[1].radius);
    CHECK(plan.cascades[2].radius < plan.cascades[3].radius);
    // casts_into: the instance below the view is in the first cascade's box; one far to the side is not.
    CHECK(casts_into(plan.cascades[3], snapshot.instances[0]));
    CHECK_FALSE(casts_into(plan.cascades[0], instance({500, 0, -10}, 1.0f)));
    // Without a shadowed directional light there are no cascades.
    snapshot.lights = {sun(false)};
    CHECK_FALSE(plan_lights(snapshot, view).sun);
}

TEST_CASE("Cascades keep their size as the view turns and move in whole texels as it moves, so shadows do not shimmer", "[renderer][shadows]") {
    auto snapshot = RenderSnapshot{};
    snapshot.lights = {sun()};
    snapshot.instances = {instance({0, 0, 0}, 1.0f)};
    const auto at = [&](const math::Vec3& from, float yaw) {
        return plan_lights(snapshot, view_at(from, from + math::Vec3{std::sin(yaw), -0.3f, -std::cos(yaw)}));
    };
    const auto first = at({0, 3, 0}, 0.0f);
    const auto light_view = sun_view(snapshot.lights[0].direction_to_light);
    for (const auto yaw : {0.4f, 1.3f, 2.9f}) {
        const auto turned = at({0, 3, 0}, yaw);
        for (uint32_t c = 0; c < sun_cascades; ++c) CHECK(turned.cascades[c].radius == first.cascades[c].radius);
    }
    // Sliding the view a little at a time: each cascade's centre, seen from the light, stays on whole texels.
    for (int step = 0; step < 50; ++step) {
        const auto moved = at({0.013f * float(step), 3, 0.007f * float(step)}, 0.0f);
        for (uint32_t c = 0; c < sun_cascades; ++c) {
            const auto centre = apply(light_view, moved.cascades[c].center);
            const auto x = centre.x / moved.cascades[c].texel, y = centre.y / moved.cascades[c].texel;
            CHECK(std::abs(x - std::round(x)) < 1e-2);
            CHECK(std::abs(y - std::round(y)) < 1e-2);
        }
    }
}
