#include "editor_math.hpp"
#include "picking.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <random>

using namespace maya;
using namespace maya::editor;
using namespace maya::test;
using Catch::Approx;

TEST_CASE("Rays enter boxes and triangles at the right distance, or miss", "[editor][picking]") {
    const auto box = [](math::Vec3 origin, math::Vec3 direction) {
        return ray_box(origin, direction, {-1, -1, -1}, {1, 1, 1});
    };
    CHECK(box({0, 0, 5}, {0, 0, -1}) == Approx(4.0f));
    CHECK(box({0, 0, 0}, {0, 0, -1}) == Approx(0.0f)); // starting inside
    CHECK_FALSE(box({0, 0, 5}, {0, 0, 1})); // pointing away
    CHECK_FALSE(box({3, 0, 5}, {0, 0, -1})); // passing beside it
    CHECK_FALSE(box({0, 2, 5}, {1, 0, 0})); // parallel, outside the slab
    CHECK(box({-5, 0.5f, 0}, {1, 0, 0}) == Approx(4.0f)); // parallel to two slabs, inside them

    const auto a = math::Vec3{-1, -1, 0}, b = math::Vec3{1, -1, 0}, c = math::Vec3{0, 1, 0};
    CHECK(ray_triangle({0, 0, 3}, {0, 0, -1}, a, b, c) == Approx(3.0f));
    CHECK(ray_triangle({0, 0, -3}, {0, 0, 1}, a, b, c) == Approx(3.0f)); // two-sided
    CHECK_FALSE(ray_triangle({0.9f, 0.9f, 3}, {0, 0, -1}, a, b, c)); // outside the triangle
    CHECK_FALSE(ray_triangle({0, 0, 3}, {1, 0, 0}, a, b, c)); // parallel
    CHECK_FALSE(ray_triangle({0, 0, 3}, {0, 0, 1}, a, b, c)); // behind the origin
}

TEST_CASE("Picking returns every hit mesh nearest first, through nested transforms", "[editor][picking]") {
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    TestProject project(device, {{"cube.mesh", unit_cube()}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    World world;
    const auto ids = build_world(world, [&](WorldCommands& commands) {
        const auto add = [&](EntityId id, TransformComponent transform) {
            auto entity = commands.create(id);
            commands.add(entity, transform);
            commands.add(entity, MeshRendererComponent{cube, {}, true});
            return entity;
        };
        add({1, 1}, {{0, 0, 0}, {}, {1.0f}}); // near
        add({1, 2}, {{0, 0, -4}, {}, {2.0f}}); // behind it, larger
        add({1, 3}, {{5, 0, 0}, {}, {1.0f}}); // off the ray
        // A child of a rotated, scaled parent: parent at x = -5 rotated 90 degrees about Y, scale 2;
        // the child sits 1 m along the parent's local X, so in world space at (-5, 0, -2).
        auto parent = add({1, 4}, {{-5, 0, 0}, math::Quat::from_axis_angle({0, 1, 0}, math::PI / 2), {2.0f}});
        auto child = add({1, 5}, {{1, 0, 0}, {}, {0.25f}});
        commands.reparent(child, parent, ReparentPolicy::keep_local);
    });
    (void)ids;
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.instances.size() == 5);

    auto hits = pick_meshes(snapshot, {{0, 0, 10}, {0, 0, -1}});
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].entity == EntityId{1, 1});
    CHECK(hits[0].distance == Approx(9.5f)); // the near cube's front face at z = 0.5
    CHECK(hits[1].entity == EntityId{1, 2});
    CHECK(hits[1].distance == Approx(13.0f)); // the larger cube's front face at z = -3

    // The nested child is hit where its world transform puts it: a 0.5 m cube centered at (-5, 0, -2).
    hits = pick_meshes(snapshot, {{-5, 0.2f, 10}, {0, 0, -1}});
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].entity == EntityId{1, 4}); // the parent's 2 m cube, front face at z = 1
    CHECK(hits[0].distance == Approx(9.0f));
    CHECK(hits[1].entity == EntityId{1, 5});
    CHECK(hits[1].distance == Approx(10.0f + 2.0f - 0.25f));
    CHECK(pick_meshes(snapshot, {{0, 10, 10}, {0, 0, -1}}).empty());
}

TEST_CASE("View rays pass through the pixels they were made for", "[editor][picking]") {
    const auto camera = CameraComponent{};
    const auto pose = look_pose({2, 3, 6}, {0, 0, 0});
    const auto view = make_render_view(camera, pose, 1600, 900);
    REQUIRE(view);
    for (const auto [x, y] : {std::pair{0.0f, 0.0f}, {0.8f, -0.5f}, {-0.95f, 0.9f}}) {
        const auto ray = view_ray(*view, math::Affine::from_matrix(pose), camera.vertical_fov, x, y);
        CHECK(ray.direction.length() == Approx(1.0f));
        const auto clip = clip_of(*view, ray.at(7.0f));
        CHECK(clip.x / clip.w == Approx(x).margin(1e-4));
        CHECK(clip.y / clip.w == Approx(y).margin(1e-4));
    }
}

TEST_CASE("Euler angles round-trip through quaternions, including gimbal lock", "[editor][picking]") {
    auto random = std::mt19937(5);
    auto angle = std::uniform_real_distribution<float>(-179.0f, 179.0f);
    auto pitch = std::uniform_real_distribution<float>(-89.0f, 89.0f);
    for (int i = 0; i < 200; ++i) {
        const auto degrees = math::Vec3{angle(random), pitch(random), angle(random)};
        const auto back = euler_degrees(from_euler_degrees(degrees));
        CHECK(back.x == Approx(degrees.x).margin(0.02));
        CHECK(back.y == Approx(degrees.y).margin(0.02));
        CHECK(back.z == Approx(degrees.z).margin(0.02));
    }
    // At 90 degrees of Y, X and Z describe the same rotation; the result must still rebuild it.
    const auto locked = from_euler_degrees({30, 90, 0});
    const auto again = from_euler_degrees(euler_degrees(locked));
    const auto a = locked.to_mat4(), b = again.to_mat4();
    for (int i = 0; i < 16; ++i) CHECK(a.elements[i] == Approx(b.elements[i]).margin(1e-4));
    const auto identity = euler_degrees(math::Quat{});
    CHECK(!std::signbit(identity.x));
    CHECK(!std::signbit(identity.y));
    CHECK(!std::signbit(identity.z));
}
