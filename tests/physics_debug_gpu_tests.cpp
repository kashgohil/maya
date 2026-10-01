// Physics debug views on Metal (#1022, docs/physics.md#debug-views): reference images of each
// category over a fixed scene, compared as the #1005 references are.

#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/physics_debug.hpp"
#include "support/metal_view.hpp"
#include <chrono>
#include <cstdio>

using namespace maya;
using namespace maya::editor::testing;
using namespace maya::test;

namespace {
const auto cube_mesh = AssetId{0x6d617961, 2};
const auto amber = AssetId{0x6d617961, 0x10};
const auto ground = AssetId{0x6d617961, 0x13};

SceneEntity entity(uint64_t low, std::string name, std::vector<ComponentValue> components) {
    components.insert(components.begin(), NameComponent{std::move(name)});
    return SceneEntity{EntityId{0x44, low}, {}, std::move(components)};
}
TransformComponent placed(math::Vec3 at, math::Vec3 scale = math::Vec3(1.0f), math::Quat rotation = {}) {
    auto transform = TransformComponent{};
    transform.translation = at;
    transform.rotation = rotation;
    transform.scale = scale;
    return transform;
}
ColliderComponent shape(ColliderShape kind, bool sensor = false, int32_t group = 0) {
    auto collider = ColliderComponent{};
    collider.shape = kind;
    collider.sensor = sensor;
    collider.group = group;
    return collider;
}
/// A camera pose at `from`, looking at `to`.
math::Mat4 look_from(const math::Vec3& from, const math::Vec3& to) {
    const auto f = (to - from).normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return local_matrix(placed(from, math::Vec3(1.0f), q));
}
MeshRendererComponent cube(AssetId material = amber) { return {AssetRef<MeshAsset>{cube_mesh}, AssetRef<MaterialAsset>{material}, true}; }

/// A floor, a stack of three boxes that falls asleep, a ball rolling across the floor, a kinematic
/// capsule, a sensor zone, and a crate in group 3.
SceneDocument debug_scene() {
    auto floor = shape(ColliderShape::box);
    floor.half_extents = {0.5f, 0.5f, 0.5f};
    auto zone = shape(ColliderShape::box, true);
    zone.half_extents = {0.5f, 0.5f, 0.5f};
    auto ball = RigidBodyComponent{};
    ball.linear_velocity = {1.5f, 0.0f, 0.0f};
    auto kinematic = RigidBodyComponent{};
    kinematic.motion = BodyMotion::kinematic;
    auto capsule = shape(ColliderShape::capsule);
    capsule.radius = 0.35f;
    capsule.half_height = 0.4f;
    auto sphere = shape(ColliderShape::sphere);
    sphere.radius = 0.4f;
    auto document = SceneDocument{};
    auto light = LightComponent{};
    light.intensity = 2.0f;
    document.entities = {
        entity(1, "Sun", {placed({0, 5, 0}, math::Vec3(1.0f), math::Quat::from_axis_angle({1, 0, 0}, -1.0f)), light}),
        entity(2, "Floor", {placed({0, -0.5f, 0}, {12, 1, 8}), cube(ground), floor}),
        entity(10, "Box A", {placed({-2, 0.5f, 0}), cube(), shape(ColliderShape::box), RigidBodyComponent{}}),
        entity(11, "Box B", {placed({-2, 1.5f, 0}), cube(), shape(ColliderShape::box), RigidBodyComponent{}}),
        entity(12, "Box C", {placed({-2, 2.5f, 0}), cube(), shape(ColliderShape::box), RigidBodyComponent{}}),
        entity(20, "Ball", {placed({-4.5f, 0.4f, 2}), sphere, ball}),
        entity(30, "Pillar", {placed({2.5f, 0.75f, -1.5f}), capsule, kinematic}),
        entity(40, "Zone", {placed({1, 1, 1.5f}, {2, 2, 2}), zone}),
        entity(50, "Crate", {placed({3.5f, 0.5f, 1.5f}, {1, 1, 1}, math::Quat::from_axis_angle({0, 1, 0}, 0.6f)), cube(),
                             shape(ColliderShape::box, false, 3), RigidBodyComponent{}}),
    };
    return document;
}
} // namespace

TEST_CASE("Physics debug views match their reference images, one per category", "[visual][gpu][physics-debug]") {
    Gpu gpu;
    const auto project = open_project(sample_project());
    REQUIRE(project);
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto started = PlaySession::start(debug_scene(), context, builtin_systems());
    INFO(started.error);
    REQUIRE(started);
    auto& session = *started.session;
    session.set_physics_debug_capture(true);
    constexpr uint32_t width = 400, height = 225;
    auto view = make_render_view(CameraComponent{}, look_from({0.0f, 4.5f, 7.5f}, {0.0f, 0.6f, 0.0f}), width, height);
    REQUIRE(view);
    view->debug_line_width = 2.0f;

    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    const auto render = [&](const char* name, uint8_t categories, uint16_t groups = all_collision_groups) {
        auto debug = DebugDraw{};
        play_physics_debug(session.world(), session.physics(), {categories, groups}, nullptr, debug);
        REQUIRE_FALSE(debug.empty());
        auto options = RenderExtractOptions{};
        options.debug = &debug;
        images.emplace_back(name, gpu.render(session.world(), *assets.registry, *view, options));
    };
    const auto run_to = [&](uint64_t tick) {
        while (session.clock().tick() < tick) REQUIRE(session.update(1.0 / 60.0).error.empty());
    };

    // Early, while everything is awake and the stack presses on the floor.
    run_to(12);
    render("contacts", uint8_t(PhysicsDebugCategory::contacts));
    // Queries asked after the tick, as a system in phase 7 would.
    const auto& physics = session.physics();
    CHECK_FALSE(physics.raycast({-5, 3, 0}, {1, -0.3f, 0}, 8.0f).empty());
    CHECK_FALSE(physics.shape_cast(SphereShape{0.3f}, {0, 3, -2}, {}, {0, -1, 0}, 4.0f).empty());
    CHECK_FALSE(physics.overlap(BoxShape{{0.6f, 0.6f, 0.6f}}, {-2, 1.5f, 0}, {}).empty());
    render("queries", uint8_t(PhysicsDebugCategory::queries));
    render("colliders", uint8_t(PhysicsDebugCategory::colliders));
    render("triggers", uint8_t(PhysicsDebugCategory::triggers));
    // Later: the stack sleeps while the ball still rolls.
    run_to(120);
    render("body-state", uint8_t(PhysicsDebugCategory::body_state));
    render("group-3", uint8_t(PhysicsDebugCategory::colliders), uint16_t(1u << 3));

    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/physics-debug",
                            fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs", images);
}

TEST_CASE("Physics debug pass cost at 1,000 and 10,000 colliders", "[.][gpu][physics-debug][cost]") {
    Gpu gpu;
    const auto project = open_project(sample_project());
    REQUIRE(project);
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    for (const auto count : {1000, 10000}) {
        // Boxes, spheres, and capsules in equal numbers on a floor, as in the simulation's cost case.
        auto document = SceneDocument{};
        auto floor = shape(ColliderShape::box);
        floor.half_extents = {0.5f, 0.5f, 0.5f};
        document.entities.push_back(entity(1, "Floor", {placed({0, -0.5f, 0}, {200, 1, 200}), cube(ground), floor}));
        for (int i = 0; i < count; ++i) {
            auto collider = shape(i % 3 == 0 ? ColliderShape::box : i % 3 == 1 ? ColliderShape::sphere : ColliderShape::capsule);
            collider.radius = 0.3f;
            collider.half_height = 0.2f;
            document.entities.push_back(entity(uint64_t(10 + i), "Body", {placed({float(i % 100) * 1.5f - 75, 0.6f, float(i / 100) * 1.5f - 75}),
                                                                          collider, RigidBodyComponent{}}));
        }
        auto started = PlaySession::start(document, context, builtin_systems());
        REQUIRE(started);
        auto& session = *started.session;
        REQUIRE(session.update(1.0 / 60.0).error.empty());
        constexpr uint32_t width = 1920, height = 1080;
        auto view = make_render_view(CameraComponent{}, look_from({0, 60, 90}, {0, 0, 0}), width, height);
        REQUIRE(view);
        view->debug_line_width = 3.0f;
        REQUIRE_FALSE(gpu.target->resize(width, height));
        auto debug = DebugDraw{};
        play_physics_debug(session.world(), session.physics(), {uint8_t(PhysicsDebugCategory::colliders)}, nullptr, debug);
        // GPU time per frame, from the device's own timestamps, with and without the pass.
        const auto gpu_ms = [&](const DebugDraw* lines, double& encode_ms) {
            auto options = RenderExtractOptions{};
            options.debug = lines;
            const auto snapshot = extract_render_snapshot(session.world(), *assets.registry, options);
            gpu.device.wait_idle();
            gpu.device.take_gpu_timings();
            constexpr int frames = 60;
            encode_ms = 0.0;
            for (int i = 0; i < frames; ++i) {
                REQUIRE_FALSE(gpu.device.begin_frame());
                const auto clock = std::chrono::steady_clock::now();
                REQUIRE_FALSE(gpu.renderer->render(snapshot, *view, *gpu.target));
                encode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock).count();
                REQUIRE_FALSE(gpu.device.end_frame());
            }
            gpu.device.wait_idle();
            encode_ms /= frames;
            auto total = 0.0;
            const auto timings = gpu.device.take_gpu_timings();
            for (const auto& timing : timings) total += timing.milliseconds;
            REQUIRE_FALSE(timings.empty());
            return total / double(timings.size());
        };
        auto plain_encode = 0.0, debug_encode = 0.0;
        const auto plain = gpu_ms(nullptr, plain_encode);
        const auto with = gpu_ms(&debug, debug_encode);
        std::printf("%6d colliders at %ux%u: GPU %.3f ms without, %.3f ms with the pass (+%.3f); encode +%.3f ms\n", count, width, height,
                    plain, with, with - plain, debug_encode - plain_encode);
    }
}
