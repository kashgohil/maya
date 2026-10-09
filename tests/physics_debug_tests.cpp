#include "maya/simulation/physics_debug.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/world/spatial.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

// Physics debug views (#1022, docs/physics.md#debug-views): what authoring and play views draw, and
// what capturing contacts and queries costs the simulation (nothing).
using namespace maya;
using Catch::Approx;

namespace {
constexpr auto frame = 1.0 / 60.0;

SceneEntity entity(uint64_t low, std::vector<ComponentValue> components, std::optional<uint64_t> parent = std::nullopt) {
    auto result = SceneEntity{EntityId{0x5d, low}, {}, std::move(components)};
    if (parent) result.parent = EntityId{0x5d, *parent};
    return result;
}
TransformComponent placed(math::Vec3 at, math::Vec3 scale = math::Vec3(1.0f)) {
    auto transform = TransformComponent{};
    transform.translation = at;
    transform.scale = scale;
    return transform;
}
ColliderComponent collider(ColliderShape shape, bool sensor = false, int32_t group = 0) {
    auto value = ColliderComponent{};
    value.shape = shape;
    value.sensor = sensor;
    value.group = group;
    return value;
}
RigidBodyComponent kinematic() {
    auto body = RigidBodyComponent{};
    body.motion = BodyMotion::kinematic;
    return body;
}

std::unique_ptr<PlaySession> start(const SceneDocument& document) {
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    auto started = PlaySession::start(document, any_asset, builtin_systems());
    INFO(started.error);
    REQUIRE(started);
    return std::move(started.session);
}
std::unique_ptr<World> authored(const SceneDocument& document) {
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    auto built = instantiate_scene(document, any_asset);
    REQUIRE(built);
    return std::move(built.world);
}

math::Vec3 translation(const math::Mat4& m) { return {m.elements[12], m.elements[13], m.elements[14]}; }
bool same_color(const DebugColor& a, const DebugColor& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
size_t count_color(const DebugDraw& draw, const DebugColor& color) {
    return size_t(std::ranges::count_if(draw.shapes, [&](const DebugShape& s) { return same_color(s.color, color); }));
}

/// A static floor; a dynamic box with a child collider (one compound body); a kinematic capsule on a
/// scaled entity; a sensor; and a box in group 5.
SceneDocument scene() {
    auto floor = collider(ColliderShape::box);
    floor.half_extents = {5, 0.5f, 5};
    auto offset_box = collider(ColliderShape::box);
    offset_box.offset = {0, 0, 1};
    auto capsule = collider(ColliderShape::capsule);
    capsule.radius = 0.25f;
    capsule.half_height = 0.5f;
    auto document = SceneDocument{};
    document.entities = {
        entity(1, {NameComponent{"Floor"}, placed({0, -0.5f, 0}), floor}),
        entity(2, {NameComponent{"Body"}, placed({0, 0.5f, 0}), collider(ColliderShape::box), RigidBodyComponent{}}),
        entity(3, {NameComponent{"Part"}, placed({1, 0, 0}), offset_box}, 2),
        entity(4, {NameComponent{"Pillar"}, placed({3, 1, 0}), capsule, kinematic()}),
        entity(5, {NameComponent{"Zone"}, placed({-3, 1, 0}), collider(ColliderShape::sphere, true)}),
        entity(6, {NameComponent{"Grouped"}, placed({0, 0.5f, -3}), collider(ColliderShape::box, false, 5), RigidBodyComponent{}}),
    };
    return document;
}
} // namespace

TEST_CASE("Authoring views outline collider components by category, state, and group", "[physics-debug]") {
    const auto owned = authored(scene());
    const auto& world = *owned;
    auto draw = DebugDraw{};
    authored_physics_debug(world, {}, draw);
    CHECK(draw.empty()); // no category: nothing
    authored_physics_debug(world, {all_physics_debug, 0}, draw);
    CHECK(draw.empty()); // no group: nothing

    authored_physics_debug(world, {uint8_t(PhysicsDebugCategory::colliders)}, draw);
    REQUIRE(draw.shapes.size() == 5); // every solid collider; not the sensor
    CHECK(draw.lines.empty());
    CHECK(count_color(draw, physics_debug_color::collider) == 5);
    // The child's collider sits at its own entity, with its offset: (1, 0.5, 0) + (0, 0, 1).
    const auto part = std::ranges::find_if(draw.shapes, [](const DebugShape& s) { return translation(s.world).x == Approx(1.0f); });
    REQUIRE(part != draw.shapes.end());
    CHECK(translation(part->world).y == Approx(0.5f));
    CHECK(translation(part->world).z == Approx(1.0f));
    // The capsule's matrix is rigid; its size carries the radius and half height.
    const auto pillar = std::ranges::find(draw.shapes, DebugShapeKind::capsule, &DebugShape::kind);
    REQUIRE(pillar != draw.shapes.end());
    CHECK(pillar->size.x == 0.25f);
    CHECK(pillar->size.y == 0.5f);

    draw.clear();
    authored_physics_debug(world, {uint8_t(PhysicsDebugCategory::body_state)}, draw);
    CHECK(count_color(draw, physics_debug_color::static_body) == 1); // the floor
    CHECK(count_color(draw, physics_debug_color::active) == 3); // the body, its part, and the grouped box
    CHECK(count_color(draw, physics_debug_color::kinematic) == 1);

    draw.clear();
    authored_physics_debug(world, {uint8_t(PhysicsDebugCategory::triggers)}, draw);
    REQUIRE(draw.shapes.size() == 1);
    CHECK(draw.shapes[0].kind == DebugShapeKind::sphere);
    CHECK(same_color(draw.shapes[0].color, physics_debug_color::trigger));

    draw.clear();
    authored_physics_debug(world, {uint8_t(PhysicsDebugCategory::colliders), uint16_t(1u << 5)}, draw);
    REQUIRE(draw.shapes.size() == 1);
    CHECK(translation(draw.shapes[0].world).z == Approx(-3.0f));

    // Contacts and queries need a physics world: authoring has neither.
    draw.clear();
    authored_physics_debug(world, {uint8_t(PhysicsDebugCategory::contacts) | uint8_t(PhysicsDebugCategory::queries)}, draw);
    CHECK(draw.empty());
}

TEST_CASE("A capsule on a scaled entity keeps round caps: the scale goes into its radius and length", "[physics-debug]") {
    auto capsule = collider(ColliderShape::capsule);
    capsule.radius = 0.5f;
    capsule.half_height = 1.0f;
    auto document = SceneDocument{};
    document.entities = {entity(1, {placed({0, 0, 0}, {2, 3, 2}), capsule})};
    auto draw = DebugDraw{};
    authored_physics_debug(*authored(document), {uint8_t(PhysicsDebugCategory::colliders)}, draw);
    REQUIRE(draw.shapes.size() == 1);
    CHECK(draw.shapes[0].size.x == Approx(1.0f));
    CHECK(draw.shapes[0].size.y == Approx(3.0f));
    CHECK(draw.shapes[0].world.elements[0] == Approx(1.0f)); // no scale left in the matrix
    CHECK(draw.shapes[0].world.elements[5] == Approx(1.0f));
}

TEST_CASE("Play views draw the physics world's bodies at their shown poses, with sleep and kinematic state", "[physics-debug]") {
    const auto session = start(scene());
    auto draw = DebugDraw{};
    play_physics_debug(session->world(), session->physics(), {uint8_t(PhysicsDebugCategory::colliders)}, nullptr, draw);
    CHECK(draw.shapes.size() == 5); // the body's two colliders come from the physics world as one body
    draw.clear();

    // At a shown pose between ticks, not the World's.
    const auto body = *session->world().find(EntityId{0x5d, 2});
    auto poses = PresentationPoses{};
    poses.set(body, math::Affine::from_matrix(math::Mat4::translate({7, 8, 9})));
    play_physics_debug(session->world(), session->physics(), {uint8_t(PhysicsDebugCategory::colliders)}, &poses, draw);
    CHECK(std::ranges::count_if(draw.shapes, [](const DebugShape& s) { return translation(s.world).x == Approx(7.0f); }) == 1);
    CHECK(std::ranges::count_if(draw.shapes, [](const DebugShape& s) { return translation(s.world).x == Approx(8.0f); }) == 1); // the part

    // Resting bodies fall asleep; the kinematic body keeps its own color.
    for (int i = 0; i < 120; ++i) REQUIRE(session->update(frame).error.empty());
    draw.clear();
    play_physics_debug(session->world(), session->physics(), {uint8_t(PhysicsDebugCategory::body_state)}, nullptr, draw);
    CHECK(count_color(draw, physics_debug_color::static_body) == 1);
    CHECK(count_color(draw, physics_debug_color::sleeping) == 3);
    CHECK(count_color(draw, physics_debug_color::kinematic_sleeping) == 1);
    const auto stats = session->physics().stats();
    CHECK(stats.active_bodies == 0);
    CHECK(stats.contacts >= 2); // both boxes rest on the floor
    CHECK(stats.overlaps == 0);
}

TEST_CASE("Contacts and queries are captured only when asked, each tick's in turn, without changing the simulation", "[physics-debug]") {
    auto document = scene();
    document.entities.push_back(entity(7, {NameComponent{"Drop"}, placed({0, 3, 2}), collider(ColliderShape::box), RigidBodyComponent{}}));
    const auto contacts_and_queries = PhysicsDebugOptions{uint8_t(PhysicsDebugCategory::contacts) | uint8_t(PhysicsDebugCategory::queries)};
    CHECK(contacts_and_queries.needs_capture());
    CHECK_FALSE(PhysicsDebugOptions{uint8_t(PhysicsDebugCategory::colliders)}.needs_capture());

    const auto off = start(document);
    const auto on = start(document);
    on->set_physics_debug_capture(true);
    CHECK(on->physics().debug_capture());
    CHECK_FALSE(off->physics().debug_capture());
    for (int i = 0; i < 5; ++i) {
        REQUIRE(off->update(frame).error.empty());
        REQUIRE(on->update(frame).error.empty());
    }
    // Off: nothing kept, though queries are still counted.
    CHECK(off->physics().debug_contacts().empty());
    CHECK(off->physics().raycast({0, 5, 0}, {0, -1, 0}, 10.0f).size() == 2); // the body and the floor
    CHECK(off->physics().debug_queries().empty());
    CHECK(off->physics().stats().queries == 1);

    // On: the last step's contact points, sorted, with their normals.
    const auto& contacts = on->physics().debug_contacts();
    REQUIRE_FALSE(contacts.empty());
    CHECK(std::ranges::is_sorted(contacts, {}, [](const PhysicsDebugContact& c) { return std::tuple(c.point.x, c.point.y, c.point.z); }));
    for (const auto& contact : contacts) CHECK(contact.normal.length() == Approx(1.0f).margin(1e-4));
    const auto hits = on->physics().raycast({0, 5, 0}, {0, -1, 0}, 10.0f);
    on->physics().overlap(SphereShape{0.5f}, {-3, 1, 0}, {}, QueryFilter{all_collision_groups, true, std::nullopt});
    REQUIRE(on->physics().debug_queries().size() == 2);
    const auto& ray = on->physics().debug_queries()[0];
    CHECK(ray.kind == PhysicsQueryKind::raycast);
    CHECK(ray.distance == 10.0f);
    REQUIRE(ray.hits.size() == hits.size());
    CHECK(ray.hits[0].y == Approx(hits[0].point.y));
    CHECK(on->physics().debug_queries()[1].kind == PhysicsQueryKind::overlap);
    CHECK(on->physics().debug_queries()[1].hits.size() == 1); // the sensor, asked for

    auto draw = DebugDraw{};
    play_physics_debug(on->world(), on->physics(), contacts_and_queries, nullptr, draw);
    CHECK(draw.shapes.size() == 1); // the overlap's sphere
    CHECK(draw.lines.size() == contacts.size() * 8 + 1 + 3 * (hits.size() + 1)); // crosses and arrows; the ray; hit crosses
    // Contacts in no shown group are left out.
    draw.clear();
    play_physics_debug(on->world(), on->physics(), {uint8_t(PhysicsDebugCategory::contacts), uint16_t(1u << 9)}, nullptr, draw);
    CHECK(draw.empty());

    // The next tick starts a new list of queries.
    REQUIRE(on->update(frame).error.empty());
    CHECK(on->physics().debug_queries().empty());
    // Turning capture off drops what was kept.
    on->set_physics_debug_capture(false);
    CHECK(on->physics().debug_contacts().empty());

    // Capturing changes nothing the simulation does.
    const auto plain = start(document);
    const auto captured = start(document);
    captured->set_physics_debug_capture(true);
    for (int i = 0; i < 300; ++i) {
        REQUIRE(plain->update(frame).error.empty());
        REQUIRE(captured->update(frame).error.empty());
        if (i % 30 == 0) REQUIRE(plain->state_hash() == captured->state_hash());
    }
    CHECK(plain->full_state_hash() == captured->full_state_hash());
}

TEST_CASE("Physics debug cost at 1,000 and 10,000 colliders", "[.][physics-debug][cost]") {
    for (const auto count : {1000, 10000}) {
        auto document = SceneDocument{};
        auto floor = collider(ColliderShape::box);
        floor.half_extents = {200, 0.5f, 200};
        document.entities.push_back(entity(1, {placed({0, -0.5f, 0}), floor}));
        for (int i = 0; i < count; ++i) {
            const auto kind = i % 3 == 0 ? ColliderShape::box : i % 3 == 1 ? ColliderShape::sphere : ColliderShape::capsule;
            auto shape = collider(kind);
            shape.radius = 0.3f;
            shape.half_height = 0.2f;
            document.entities.push_back(entity(uint64_t(10 + i), {placed({float(i % 100) * 1.5f - 75, 0.6f, float(i / 100) * 1.5f - 75}), shape,
                                                                RigidBodyComponent{}}));
        }
        auto session = start(document);
        session->set_physics_debug_capture(true);
        for (int i = 0; i < 30; ++i) REQUIRE(session->update(frame).error.empty());
        const auto poses = session->presentation();
        auto draw = DebugDraw{};
        constexpr int rounds = 30;
        auto outline_ms = 0.0, all_ms = 0.0;
        for (int i = 0; i < rounds; ++i) {
            draw.clear();
            auto clock = std::chrono::steady_clock::now();
            play_physics_debug(session->world(), session->physics(), {uint8_t(PhysicsDebugCategory::colliders)}, &poses, draw);
            outline_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock).count();
            draw.clear();
            clock = std::chrono::steady_clock::now();
            play_physics_debug(session->world(), session->physics(), {all_physics_debug}, &poses, draw);
            all_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock).count();
        }
        std::printf("%6d colliders: outlines %.3f ms, every category %.3f ms (%zu outlines, %zu lines)\n", count, outline_ms / rounds,
                    all_ms / rounds, draw.shapes.size(), draw.lines.size());
    }
}
