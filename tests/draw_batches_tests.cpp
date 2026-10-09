// Culling and batching (#1025, docs/renderer.md#culling-and-batching): which instances each pass draws,
// and how they are grouped into instanced draws of one mesh and one material.

#include "maya/renderer/draw_batches.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/world/spatial.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

using namespace maya;
using namespace maya::test;

namespace {
RenderView view_from(const math::Vec3& eye, const math::Vec3& target) {
    auto camera = CameraComponent{};
    camera.vertical_fov = 1.0f;
    camera.near_clip = 0.1f;
    camera.far_clip = 100.0f;
    const auto view = make_render_view(camera, look_pose(eye, target), 200, 100);
    REQUIRE(view);
    return *view;
}
RenderInstance at(const math::Vec3& center, float radius, uint32_t mesh = 0, uint32_t material = 0) {
    auto instance = RenderInstance{};
    instance.world = math::Mat4::translate(center);
    instance.bounds_center = center;
    instance.bounds_radius = radius;
    instance.mesh = mesh;
    instance.material = material;
    return instance;
}
/// Every instance a batch draws, in order.
std::vector<uint32_t> drawn(const DrawList& list, const DrawBatch& batch) {
    return {list.order.begin() + batch.first, list.order.begin() + batch.first + batch.count};
}
} // namespace

TEST_CASE("A view's frustum keeps spheres that reach inside it, at every edge, and drops the rest", "[renderer][batches]") {
    const auto view = view_from({0, 0, 10}, {0, 0, 0});
    const auto frustum = Frustum::from(view_frame(RenderSnapshot{}, view).matrices.view_projection); // about the world's origin
    // The view's half extents at 10 m: tan(0.5) x 10 up and down, twice that left and right (2:1).
    const auto half_y = std::tan(0.5f) * 10.0f, half_x = half_y * 2.0f;
    for (const auto& [direction, extent] : {std::pair{math::Vec3{1, 0, 0}, half_x}, std::pair{math::Vec3{-1, 0, 0}, half_x},
                                             std::pair{math::Vec3{0, 1, 0}, half_y}, std::pair{math::Vec3{0, -1, 0}, half_y}}) {
        INFO("toward " << direction.x << " " << direction.y);
        // A unit sphere whose centre is just outside the edge still reaches in; one a little farther does not.
        // (The planes are slanted, so a sphere touches them nearer than the edge plus its radius.)
        CHECK(frustum.reaches(direction * (extent + 0.5f), 1.0f));
        CHECK_FALSE(frustum.reaches(direction * (extent + 1.5f), 1.0f));
        CHECK(frustum.reaches(direction * (extent - 0.1f), 0.0f));
        CHECK_FALSE(frustum.reaches(direction * (extent + 0.1f), 0.0f));
    }
    // Near and far planes: 0.1 and 100 m from the eye.
    CHECK_FALSE(frustum.reaches({0, 0, 10.5f}, 0.2f)); // behind the eye
    CHECK(frustum.reaches({0, 0, 9.95f}, 0.1f));
    CHECK(frustum.reaches({0, 0, -89.0f}, 0.5f));
    CHECK_FALSE(frustum.reaches({0, 0, -91.0f}, 0.5f));
    // Unbounded instances are always drawn.
    CHECK(frustum.reaches({0, 0, 500}, std::numeric_limits<float>::infinity()));
}

TEST_CASE("Views draw what reaches them, grouped by material and mesh, with blended surfaces back to front", "[renderer][batches]") {
    auto snapshot = RenderSnapshot{};
    auto opaque = RenderMaterial{}, two_sided = RenderMaterial{}, glass = RenderMaterial{};
    two_sided.double_sided = true;
    glass.alpha_mode = AlphaMode::blend;
    snapshot.materials = {opaque, two_sided, glass, opaque};
    // Interleaved meshes and materials, two out of view, and three blended at different distances.
    snapshot.instances = {at({0, 0, 0}, 0.5f, 0, 0), at({1, 0, 0}, 0.5f, 1, 0), at({-1, 0, 0}, 0.5f, 0, 3),
                          at({0, 1, 0}, 0.5f, 0, 0), at({0, 0, 0}, 0.5f, 0, 1), at({100, 0, 0}, 0.5f, 0, 0),
                          at({0, -1, 0}, 0.5f, 1, 0), at({0, 0, -5}, 0.5f, 0, 2), at({0, 0, 5}, 0.5f, 0, 2),
                          at({0, 0, 2}, 0.5f, 1, 2), at({0, 0, 1}, 0.5f, 1, 2), at({0, 500, 0}, 0.5f, 1, 1)};
    auto list = DrawList{};
    auto scratch = BatchScratch{};
    const auto batches = plan_view_batches(snapshot, view_from({0, 0, 10}, {0, 0, 0}), list, scratch);
    CHECK(batches.drawn == 10);
    CHECK(batches.culled == 2);
    CHECK(list.order.size() == 10);
    // Single-sided first, by material then mesh; instances keep their order within a batch.
    REQUIRE(batches.opaque.size() == 4);
    CHECK(drawn(list, batches.opaque[0]) == std::vector<uint32_t>{0, 3});
    CHECK(drawn(list, batches.opaque[1]) == std::vector<uint32_t>{1, 6});
    CHECK(drawn(list, batches.opaque[2]) == std::vector<uint32_t>{2});
    CHECK(drawn(list, batches.opaque[3]) == std::vector<uint32_t>{4}); // double-sided last
    CHECK(batches.opaque[1].mesh == 1);
    CHECK(batches.opaque[1].material == 0);
    // Blended back to front from the eye at z = 10: z = -5, 0... only neighbours of one mesh and material share.
    REQUIRE(batches.blended.size() == 3);
    CHECK(drawn(list, batches.blended[0]) == std::vector<uint32_t>{7}); // farthest
    CHECK(drawn(list, batches.blended[1]) == std::vector<uint32_t>{10, 9}); // mesh 1 at z = 1, then z = 2
    CHECK(drawn(list, batches.blended[2]) == std::vector<uint32_t>{8}); // nearest
    // A second pass appends to the same order.
    const auto again = plan_view_batches(snapshot, view_from({0, 0, 10}, {0, 0, 0}), list, scratch);
    CHECK(list.order.size() == 20);
    CHECK(again.opaque[0].first == 10);
}

TEST_CASE("Parented instances are culled by their world bounds, as extraction computes them", "[renderer][batches]") {
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    TestProject project(device, {{"cube.mesh", unit_cube()}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        auto parent = commands.create();
        commands.add(parent, TransformComponent{{50, 0, 0}, {}, {1.0f}}); // far to the side
        for (const auto x : {-50.0f, 0.0f}) { // back at the origin, and with the parent
            auto child = commands.create();
            commands.add(child, TransformComponent{{x, 0, 0}, {}, {1.0f}});
            commands.add(child, MeshRendererComponent{cube, {}, true});
            commands.reparent(child, parent, ReparentPolicy::keep_local);
        }
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.instances.size() == 2);
    auto list = DrawList{};
    auto scratch = BatchScratch{};
    const auto batches = plan_view_batches(snapshot, view_from({0, 0, 10}, {0, 0, 0}), list, scratch);
    CHECK(batches.drawn == 1);
    CHECK(batches.culled == 1);
    REQUIRE(list.order.size() == 1);
    CHECK(snapshot.instances[list.order[0]].entity == *world.persistent_id(created[1]));
    // Bigger than a unit cube by its scale, the bounds still reach: a child scaled up near the edge is kept.
    CHECK(snapshot.instances[list.order[0]].bounds_radius > 0.8f);
}

TEST_CASE("Shadow maps draw their casters by mesh, masked ones by material too, and nothing blended", "[renderer][batches]") {
    auto snapshot = RenderSnapshot{};
    auto red = RenderMaterial{}, blue = RenderMaterial{}, leaves = RenderMaterial{}, glass = RenderMaterial{};
    red.base_color = {1, 0, 0, 1};
    blue.base_color = {0, 0, 1, 1};
    leaves.alpha_mode = AlphaMode::mask;
    glass.alpha_mode = AlphaMode::blend;
    snapshot.materials = {red, blue, leaves, glass};
    snapshot.instances = {at({0, 0, 0}, 1, 0, 0), at({1, 0, 0}, 1, 0, 1), at({2, 0, 0}, 1, 1, 0), at({3, 0, 0}, 1, 0, 2),
                          at({4, 0, 0}, 1, 0, 3), at({5, 0, 0}, 1, 0, 2), at({60, 0, 0}, 1, 0, 0)};
    auto list = DrawList{};
    auto scratch = BatchScratch{};
    const auto batches = plan_shadow_batches(snapshot, [](const RenderInstance& instance) { return instance.bounds_center.x < 50; },
                                             list, scratch);
    REQUIRE(batches.size() == 3);
    CHECK(drawn(list, batches[0]) == std::vector<uint32_t>{0, 1}); // mesh 0, red and blue alike
    CHECK(drawn(list, batches[1]) == std::vector<uint32_t>{2}); // mesh 1
    CHECK(drawn(list, batches[2]) == std::vector<uint32_t>{3, 5}); // masked: its material's cutout matters
    CHECK(batches[2].material == 2);
    CHECK(list.order.size() == 5); // not the glass, not the one the map does not reach
}
