#include "maya/simulation/play_session.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

using namespace maya;
using Catch::Approx;

namespace {
constexpr auto frame = 1.0 / 60.0;

SceneEntity entity(uint64_t low, std::vector<ComponentValue> components, std::optional<uint64_t> parent = {}) {
    auto value = SceneEntity{EntityId{0x70, low}, {}, std::move(components)};
    if (parent) value.parent = EntityId{0x70, *parent};
    return value;
}
TransformComponent at(math::Vec3 position, math::Vec3 scale = math::Vec3(1.0f)) {
    return {position, {}, scale};
}
EntityHandle handle(const World& world, uint64_t low) {
    return *world.find(EntityId{0x70, low});
}
TransformComponent transform_of(const World& world, uint64_t low) {
    auto result = TransformComponent{};
    REQUIRE(world.with<TransformComponent>(handle(world, low), [&](const TransformComponent& value) { result = value; }));
    return result;
}

BodyDesc box(MotionType motion, math::Vec3 half = math::Vec3(0.5f)) {
    auto body = BodyDesc{};
    body.motion = motion;
    body.colliders = {ColliderDesc{BoxShape{half}}};
    return body;
}
BodyDesc sphere(MotionType motion, float radius = 0.5f) {
    auto body = BodyDesc{};
    body.motion = motion;
    body.colliders = {ColliderDesc{SphereShape{radius}}};
    return body;
}

/// Runs a callback on every tick, and another after each step; the tests script what systems ask of
/// physics with them.
class Scripted final : public SimulationSystem {
public:
    explicit Scripted(std::function<void(TickContext&)> tick, std::string name = "Scripted",
                      std::function<void(TickContext&)> late = {})
        : m_tick(std::move(tick)), m_late(std::move(late)), m_name(std::move(name)) {}
    std::string_view name() const override { return m_name; }
    void fixed_update(TickContext& tick) override { m_tick(tick); }
    void late_fixed_update(TickContext& tick) override {
        if (m_late) m_late(tick);
    }

private:
    std::function<void(TickContext&)> m_tick, m_late;
    std::string m_name;
};

std::unique_ptr<PlaySession> play(const SceneDocument& document, std::function<void(TickContext&)> tick,
                                  PhysicsSettings settings = {}, std::string name = "Scripted") {
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<Scripted>(std::move(tick), std::move(name)));
    auto started = PlaySession::start(document, {}, std::move(systems), {}, settings);
    REQUIRE(started);
    return std::move(started.session);
}

/// A session whose system also runs `late` after each step, and when the session stops.
std::unique_ptr<PlaySession> play_late(const SceneDocument& document, std::function<void(TickContext&)> tick,
                                       std::function<void(TickContext&)> late, PhysicsSettings settings = {}) {
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<Scripted>(std::move(tick), "Scripted", std::move(late)));
    auto started = PlaySession::start(document, {}, std::move(systems), {}, settings);
    REQUIRE(started);
    return std::move(started.session);
}

/// On the first tick, creates the bodies listed for entities by their low ID word.
std::function<void(TickContext&)> create_on_first_tick(std::vector<std::pair<uint64_t, BodyDesc>> bodies) {
    return [bodies = std::move(bodies)](TickContext& tick) {
        if (tick.tick != 0) return;
        for (const auto& [low, body] : bodies) tick.bodies.create(handle(tick.world, low), body);
    };
}

/// Runs `ticks` host frames of one tick each.
void run(PlaySession& session, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        const auto played = session.update(frame);
        INFO(session.error());
        REQUIRE(played.ticks_run == 1);
    }
}

/// Plays until the session fails and returns its error.
std::string failure(PlaySession& session, int ticks = 3) {
    for (int i = 0; i < ticks && !session.failed(); ++i) session.update(frame);
    return session.error();
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

uint64_t pose_hash(const World& world) {
    auto hash = uint64_t{14695981039346656037ull};
    world.for_each<TransformComponent>([&](EntityHandle, const TransformComponent& value) {
        const float words[] = {value.translation.x, value.translation.y, value.translation.z,
                               value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w};
        for (const auto word : words) {
            auto bits = uint32_t{};
            std::memcpy(&bits, &word, sizeof bits);
            hash = (hash ^ bits) * 1099511628211ull;
        }
    });
    return hash;
}

SceneDocument floor_and(std::vector<SceneEntity> more) {
    auto document = SceneDocument{};
    document.entities.push_back(entity(1, {TransformComponent{at({0.0f, -0.5f, 0.0f})}}));
    for (auto& value : more) document.entities.push_back(std::move(value));
    return document;
}
BodyDesc floor_body() { return box(MotionType::static_body, {50.0f, 0.5f, 50.0f}); }

/// A floor, then `count` 1 m boxes in 10 x 10 layers (IDs 100 on) dropping onto it.
SceneDocument pile(int count) {
    auto more = std::vector<SceneEntity>{};
    for (int i = 0; i < count; ++i) {
        const auto slot = i % 100, level = i / 100;
        more.push_back(entity(100 + i, {TransformComponent{at({(float(slot % 10) - 4.5f) * 1.5f, 2.0f + float(level) * 1.5f,
                                                             (float(slot / 10) - 4.5f) * 1.5f})}}));
    }
    return floor_and(std::move(more));
}
std::function<void(TickContext&)> pile_bodies(int count) {
    auto bodies = std::vector<std::pair<uint64_t, BodyDesc>>{{1, floor_body()}};
    for (int i = 0; i < count; ++i) {
        auto body = box(MotionType::dynamic);
        body.colliders[0].rotation = math::Quat::from_axis_angle(math::Vec3(1.0f, 0.0f, 1.0f).normalized(), 0.3f);
        bodies.emplace_back(100 + i, body);
    }
    return create_on_first_tick(std::move(bodies));
}

/// Restores the default worker count when a test changes it.
struct Workers {
    explicit Workers(int count) { set_physics_worker_threads(count); }
    ~Workers() { set_physics_worker_threads(-1); }
};
} // namespace

TEST_CASE("Body descriptions are validated before a body is made", "[physics]") {
    CHECK(validate_body(box(MotionType::dynamic)).empty());
    CHECK(validate_body(sphere(MotionType::static_body)).empty());
    auto body = box(MotionType::dynamic);
    body.colliders.clear();
    CHECK(validate_body(body) == "a body needs at least one collider");
    CHECK(validate_body(box(MotionType::dynamic, {0.5f, 0.0f, 0.5f})) == "a box needs finite, positive half extents");
    CHECK(validate_body(sphere(MotionType::dynamic, -1.0f)) == "a sphere needs a finite, positive radius");
    body = box(MotionType::dynamic);
    body.colliders[0].shape = CapsuleShape{0.3f, 0.0f};
    CHECK(validate_body(body) == "a capsule needs a finite, positive radius and half height");
    body = box(MotionType::dynamic);
    body.colliders[0].rotation = {0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(validate_body(body) == "a collider rotation is not a finite, nonzero quaternion");
    body = box(MotionType::dynamic);
    body.restitution = 1.5f;
    CHECK(validate_body(body) == "the restitution must be between 0 and 1");
    body = box(MotionType::dynamic);
    body.density = 0.0f;
    CHECK(validate_body(body) == "the density must be finite and positive");
    body = box(MotionType::dynamic);
    body.mass = std::nanf("");
    CHECK(validate_body(body) == "the mass must be finite and not negative");
    body = box(MotionType::static_body);
    body.linear_velocity = {1.0f, 0.0f, 0.0f};
    CHECK(validate_body(body) == "a static body has no initial velocity");
    body = box(MotionType::dynamic);
    body.group = 16;
    CHECK(validate_body(body) == "the collision group must be 0 to 15");
}

TEST_CASE("A dynamic body falls as the fixed-step integration predicts", "[physics]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 100.0f, 0.0f})}})};
    auto body = sphere(MotionType::dynamic);
    body.linear_damping = 0.0f;
    auto session = play(document, create_on_first_tick({{1, body}}));
    run(*session, 61); // the body arrives at the end of tick 0, then falls for 60 steps
    // Jolt integrates velocity, then position: after n steps y = y0 + g dt² n(n+1)/2.
    constexpr auto dt = 1.0 / 60.0;
    constexpr auto steps = 60.0;
    const auto expected = 100.0 - 9.81 * dt * dt * steps * (steps + 1.0) / 2.0;
    const auto fallen = transform_of(session->world(), 1);
    CHECK(fallen.translation.y == Approx(expected).margin(1e-3));
    CHECK(fallen.translation.x == 0.0f);
    const auto state = session->physics().state(handle(session->world(), 1));
    REQUIRE(state);
    CHECK(state->linear_velocity.y == Approx(-9.81 * steps * dt).margin(1e-3));
    CHECK(state->position.y == fallen.translation.y); // the World shows exactly the body's pose
    CHECK(state->mass == Approx(1000.0 * 4.0 / 3.0 * math::PI * 0.125).epsilon(1e-3));
    CHECK(session->physics().stats().steps == 61);
}

TEST_CASE("A dropped box comes to rest on a static floor and sleeps", "[physics]") {
    auto session = play(pile(1), pile_bodies(1));
    run(*session, 240);
    const auto rested = transform_of(session->world(), 100);
    CHECK(rested.translation.y == Approx(0.5f).margin(0.01f));
    const auto state = session->physics().state(handle(session->world(), 100));
    REQUIRE(state);
    CHECK(state->sleeping);
    const auto stats = session->physics().stats();
    CHECK(stats.bodies == 2);
    CHECK(stats.static_bodies == 1);
    CHECK(stats.dynamic_bodies == 1);
    CHECK(stats.active_bodies == 0);
    CHECK(stats.steps_with_errors == 0);
    // Static bodies never move their entity: the floor's transform is as authored.
    CHECK(transform_of(session->world(), 1).translation.y == -0.5f);
}

TEST_CASE("Kinematic bodies follow their targets and stop without one", "[physics]") {
    auto document = floor_and({entity(2, {TransformComponent{at({0.0f, 0.5f, 0.0f})}}),
                               entity(3, {TransformComponent{at({3.0f, 0.5f, 0.0f})}})});
    auto targets = 30;
    auto session = play(document, [&](TickContext& tick) {
        if (tick.tick == 0) {
            tick.bodies.create(handle(tick.world, 1), floor_body());
            tick.bodies.create(handle(tick.world, 2), box(MotionType::kinematic));
            tick.bodies.create(handle(tick.world, 3), box(MotionType::dynamic));
            return;
        }
        if (tick.tick <= uint64_t(targets)) // 1 m/s along +X, one target per tick
            tick.bodies.set_kinematic_target(handle(tick.world, 2), {float(tick.tick) / 60.0f, 0.5f, 0.0f}, {});
    });
    run(*session, 31);
    CHECK(transform_of(session->world(), 2).translation.x == Approx(0.5f).margin(1e-4));
    const auto moving = session->physics().state(handle(session->world(), 2));
    REQUIRE(moving);
    CHECK(moving->motion == MotionType::kinematic);
    CHECK(moving->linear_velocity.x == Approx(1.0f).margin(1e-3));
    CHECK(moving->mass == 0.0f);
    run(*session, 30); // no more targets: it stops at the last one
    CHECK(transform_of(session->world(), 2).translation.x == Approx(0.5f).margin(1e-4));
    CHECK(session->physics().state(handle(session->world(), 2))->linear_velocity.x == 0.0f);

    SECTION("A kinematic body pushes dynamic ones") {
        targets = 400;
        run(*session, 200);
        CHECK(transform_of(session->world(), 3).translation.x > 3.5f);
    }
}

TEST_CASE("Teleports move a body directly and keep its velocity", "[physics]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 10.0f, 0.0f})}})};
    auto body = sphere(MotionType::dynamic);
    body.gravity_factor = 0.0f;
    body.linear_velocity = {2.0f, 0.0f, 0.0f};
    body.linear_damping = 0.0f;
    const auto turned = math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, 1.0f);
    auto session = play(document, [&](TickContext& tick) {
        if (tick.tick == 0) tick.bodies.create(handle(tick.world, 1), body);
        if (tick.tick == 10) tick.bodies.teleport(handle(tick.world, 1), {-5.0f, 3.0f, 1.0f}, turned);
    });
    run(*session, 10);
    CHECK(transform_of(session->world(), 1).translation.x == Approx(2.0f * 9.0f / 60.0f).margin(1e-4));
    run(*session, 1); // tick 10: teleported, then one step at 2 m/s
    const auto moved = transform_of(session->world(), 1);
    CHECK(moved.translation.x == Approx(-5.0f + 2.0f / 60.0f).margin(1e-4));
    CHECK(moved.translation.y == Approx(3.0f).margin(1e-5));
    CHECK(moved.translation.z == Approx(1.0f).margin(1e-5));
    CHECK(moved.rotation.y == Approx(turned.y).margin(1e-5));
    CHECK(moved.rotation.w == Approx(turned.w).margin(1e-5));
}

TEST_CASE("Forces, impulses, and velocities push dynamic bodies", "[physics]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 0.0f, 0.0f})}}),
                         entity(2, {TransformComponent{at({10.0f, 0.0f, 0.0f})}}),
                         entity(3, {TransformComponent{at({20.0f, 0.0f, 0.0f})}})};
    auto body = sphere(MotionType::dynamic);
    body.gravity_factor = 0.0f;
    body.linear_damping = 0.0f;
    body.mass = 2.0f;
    auto session = play(document, [&](TickContext& tick) {
        if (tick.tick == 0) {
            for (uint64_t low = 1; low <= 3; ++low) tick.bodies.create(handle(tick.world, low), body);
            return;
        }
        if (tick.tick != 1) return;
        tick.bodies.add_force(handle(tick.world, 1), {120.0f, 0.0f, 0.0f}); // 60 m/s² for one step
        tick.bodies.add_impulse(handle(tick.world, 2), {4.0f, 0.0f, 0.0f}); // 2 m/s
        tick.bodies.set_linear_velocity(handle(tick.world, 3), {0.0f, 0.0f, -3.0f});
    });
    run(*session, 2);
    const auto velocity = [&](uint64_t low) { return session->physics().state(handle(session->world(), low))->linear_velocity; };
    CHECK(velocity(1).x == Approx(1.0f).margin(1e-4));
    CHECK(velocity(2).x == Approx(2.0f).margin(1e-4));
    CHECK(velocity(3).z == Approx(-3.0f).margin(1e-5));
    run(*session, 1); // a force lasts one step
    CHECK(velocity(1).x == Approx(1.0f).margin(1e-4));
    CHECK(session->physics().state(handle(session->world(), 1))->mass == Approx(2.0f));
}

TEST_CASE("Requests that break the motion-authority rules are refused with a reason", "[physics]") {
    auto document = floor_and({entity(2, {TransformComponent{at({0.0f, 2.0f, 0.0f})}}),
                               entity(3, {TransformComponent{at({0.0f, 1.0f, 0.0f})}}, 2),
                               entity(4, {TransformComponent{at({5.0f, 2.0f, 0.0f}, {1.0f, 2.0f, 1.0f})}}),
                               entity(5, {TransformComponent{at({9.0f, 2.0f, 0.0f})}})});
    const auto with_bodies = [&](std::function<void(TickContext&)> then) {
        return [then = std::move(then)](TickContext& tick) {
            if (tick.tick == 0) {
                tick.bodies.create(handle(tick.world, 1), floor_body());
                tick.bodies.create(handle(tick.world, 2), box(MotionType::dynamic));
                tick.bodies.create(handle(tick.world, 5), box(MotionType::kinematic));
                return;
            }
            then(tick);
        };
    };
    SECTION("Physics writes a dynamic body's pose; a system may not") {
        auto session = play(document, with_bodies([](TickContext& tick) {
            tick.commands.set_transform(handle(tick.world, 2), at({0.0f, 5.0f, 0.0f}));
        }), {}, "Lifter");
        const auto error = failure(*session);
        CHECK(contains(error, "Tick 1: Lifter failed: cannot set the transform of entity 70 2: its dynamic body's pose is "
                              "written by physics; use a force, a velocity, a kinematic target, or a teleport"));
    }
    SECTION("A kinematic body moves by targets, not transform writes") {
        auto session = play(document, with_bodies([](TickContext& tick) {
            tick.commands.set_transform(handle(tick.world, 5), at({9.0f, 3.0f, 0.0f}));
        }));
        CHECK(contains(failure(*session), "its kinematic body's pose is written by physics"));
    }
    SECTION("A moving body stays a root entity") {
        auto session = play(document, with_bodies([](TickContext& tick) {
            tick.commands.reparent(handle(tick.world, 2), handle(tick.world, 1), ReparentPolicy::keep_world);
        }));
        CHECK(contains(failure(*session), "cannot reparent entity 70 2: its dynamic body must stay a root entity"));
    }
    SECTION("Nothing with a body goes under a moving body") {
        auto session = play(document, with_bodies([](TickContext& tick) {
            tick.commands.reparent(handle(tick.world, 1), handle(tick.world, 3), ReparentPolicy::keep_world);
        }));
        CHECK(contains(failure(*session), "cannot reparent entity 70 1 under a moving body"));
    }
    SECTION("The transform of a body stays") {
        auto session = play(document, with_bodies([](TickContext& tick) {
            tick.commands.remove<TransformComponent>(handle(tick.world, 1));
        }));
        CHECK(contains(failure(*session), "cannot remove the transform of entity 70 1: it has a body"));
    }
    SECTION("Pushes need a dynamic body; targets need a kinematic one") {
        const auto refused = [&](std::function<void(TickContext&)> request) {
            auto session = play(document, with_bodies(std::move(request)));
            return failure(*session);
        };
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.add_force(handle(tick.world, 1), {1.0f, 0.0f, 0.0f}); }),
                       "cannot push entity 70 1: its body is static; move a static collider by writing its transform"));
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.set_linear_velocity(handle(tick.world, 5), {1.0f, 0.0f, 0.0f}); }),
                       "its body is kinematic; move it with a kinematic target"));
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.set_kinematic_target(handle(tick.world, 2), {}, {}); }),
                       "cannot set a kinematic target for entity 70 2: its body is dynamic"));
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.teleport(handle(tick.world, 1), {}, {}); }),
                       "cannot teleport entity 70 1: its body is static"));
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.add_impulse(handle(tick.world, 4), {1.0f, 0.0f, 0.0f}); }),
                       "cannot push entity 70 4: it has no body"));
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.add_force(handle(tick.world, 2), {NAN, 0.0f, 0.0f}); }),
                       "is not finite"));
        CHECK(contains(refused([](TickContext& tick) { tick.bodies.create(handle(tick.world, 2), box(MotionType::dynamic)); }),
                       "cannot create a body for entity 70 2: it already has one"));
    }
}

TEST_CASE("Bodies are refused where the contract's scale and hierarchy rules forbid them", "[physics]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 1.0f})}}),
                         entity(2, {TransformComponent{at({0.0f, 1.0f, 0.0f})}}, 1),
                         entity(3, {TransformComponent{at({4.0f, 0.0f, 0.0f})}}),
                         entity(4, {TransformComponent{at({0.0f, 1.0f, 0.0f})}}, 3),
                         entity(5, {NameComponent{"no transform"}})};
    const auto refusal = [&](uint64_t low, BodyDesc body, std::function<void(TickContext&)> before = {}) {
        auto session = play(document, [&](TickContext& tick) {
            if (tick.tick == 0 && before) before(tick);
            if (tick.tick == (before ? 1u : 0u)) tick.bodies.create(handle(tick.world, low), body);
        }, {}, "Builder");
        return failure(*session);
    };
    CHECK(contains(refusal(2, box(MotionType::dynamic)),
                   "Builder: cannot create a dynamic body for entity 70 2: a dynamic body must be a root entity"));
    CHECK(contains(refusal(1, box(MotionType::kinematic)), "a kinematic body needs unit scale"));
    CHECK(contains(refusal(1, sphere(MotionType::static_body)), "a sphere needs uniform scale"));
    auto capsule = box(MotionType::static_body);
    capsule.colliders[0].shape = CapsuleShape{};
    CHECK(refusal(1, capsule).empty()); // X and Z match: the straight section stretches along Y
    auto rotated = box(MotionType::static_body);
    rotated.colliders[0].rotation = math::Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, 0.5f);
    CHECK(contains(refusal(2, rotated), "a rotated collider cannot take its entity's nonuniform scale"));
    CHECK(contains(refusal(5, box(MotionType::static_body)), "a body needs a transform"));
    const auto moving_parent = [](TickContext& tick) { tick.bodies.create(handle(tick.world, 3), box(MotionType::dynamic)); };
    CHECK(contains(refusal(4, box(MotionType::static_body), moving_parent), "it is below a moving body"));
    const auto static_child = [](TickContext& tick) { tick.bodies.create(handle(tick.world, 4), box(MotionType::static_body)); };
    CHECK(contains(refusal(3, box(MotionType::dynamic), static_child), "an entity below it has a body"));
}

TEST_CASE("Static colliders bake their world scale and follow their committed transforms", "[physics]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 3.0f})}}),
                         entity(2, {TransformComponent{at({1.0f, 0.0f, 0.0f})}}, 1)};
    auto session = play(document, [](TickContext& tick) {
        if (tick.tick == 0) tick.bodies.create(handle(tick.world, 2), box(MotionType::static_body));
        if (tick.tick == 5) tick.commands.set_transform(handle(tick.world, 1), at({0.0f, 4.0f, 0.0f}, {2.0f, 1.0f, 3.0f}));
        if (tick.tick == 8) tick.commands.set_transform(handle(tick.world, 1), at({0.0f, 4.0f, 0.0f}, {1.0f, 1.0f, 1.0f}));
    });
    run(*session, 3);
    const auto state = [&] { return *session->physics().state(handle(session->world(), 2)); };
    CHECK(state().position.x == Approx(2.0f)); // the child's offset scaled by its parent
    CHECK(state().motion == MotionType::static_body);
    run(*session, 4); // tick 5's transform commits at its end and applies before tick 6's step
    CHECK(state().position.y == Approx(4.0f));
    run(*session, 3); // the new scale rebuilds the shape
    CHECK(state().position.x == Approx(1.0f));
    CHECK(session->physics().stats().static_bodies == 1);
}

TEST_CASE("Compound bodies combine their colliders", "[physics]") {
    auto document = floor_and({entity(2, {TransformComponent{at({0.0f, 3.0f, 0.0f})}})});
    auto dumbbell = BodyDesc{};
    dumbbell.colliders = {ColliderDesc{SphereShape{0.5f}, {-1.0f, 0.0f, 0.0f}}, ColliderDesc{SphereShape{0.5f}, {1.0f, 0.0f, 0.0f}},
                          ColliderDesc{CapsuleShape{0.1f, 0.5f}, {}, math::Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, math::PI / 2.0f)}};
    auto session = play(document, create_on_first_tick({{1, floor_body()}, {2, dumbbell}}));
    run(*session, 300);
    const auto rested = transform_of(session->world(), 2);
    // Resting on both spheres; Jolt lets resting contacts sink by up to its 2 cm penetration slop.
    CHECK(rested.translation.y == Approx(0.49f).margin(0.015f));
    CHECK(rested.translation.x == Approx(0.0f).margin(0.02f));
    const auto mass = session->physics().state(handle(session->world(), 2))->mass;
    CHECK(mass > 2.0f * 1000.0f * 4.0f / 3.0f * math::PI * 0.125f);
}

TEST_CASE("Bodies arrive and leave with their entities at the tick boundary", "[physics]") {
    auto document = floor_and({entity(2, {TransformComponent{at({0.0f, 5.0f, 0.0f})}})});
    auto spawned = std::optional<EntityHandle>{};
    auto session = play(document, [&](TickContext& tick) {
        if (tick.tick == 0) {
            tick.bodies.create(handle(tick.world, 1), floor_body());
            tick.bodies.create(handle(tick.world, 2), sphere(MotionType::dynamic));
            // A new entity and its body in the same tick.
            const auto pending = tick.commands.create(EntityId{0x70, 3});
            tick.commands.add(pending, TransformComponent{at({3.0f, 5.0f, 0.0f})});
            tick.bodies.create(pending, sphere(MotionType::dynamic));
        }
        if (tick.tick == 1) spawned = handle(tick.world, 3);
        if (tick.tick == 10) {
            REQUIRE(tick.physics.has_body(handle(tick.world, 2)));
            tick.commands.destroy(handle(tick.world, 2));
        }
    });
    run(*session, 2);
    REQUIRE(spawned);
    CHECK(session->physics().has_body(*spawned));
    CHECK(transform_of(session->world(), 3).translation.y < 5.0f);
    const auto doomed = handle(session->world(), 2);
    run(*session, 9);
    CHECK_FALSE(session->world().alive(doomed));
    CHECK_FALSE(session->physics().has_body(doomed));
    CHECK_FALSE(session->physics().state(doomed));
    const auto stats = session->physics().stats();
    CHECK(stats.bodies == 2);
    CHECK(stats.bodies_created == 3);
    CHECK(stats.bodies_removed == 1);

    SECTION("A stale handle never reaches the physics library") {
        auto stale = play(document, [&](TickContext& tick) {
            if (tick.tick == 0) {
                tick.bodies.create(handle(tick.world, 2), sphere(MotionType::dynamic));
                tick.commands.destroy(handle(tick.world, 2));
            }
        });
        CHECK(contains(failure(*stale), "cannot create a dynamic body for entity (slot"));
        auto old = std::optional<EntityHandle>{};
        auto later = play(document, [&](TickContext& tick) {
            if (tick.tick == 0) tick.bodies.create(handle(tick.world, 2), sphere(MotionType::dynamic));
            if (tick.tick == 1) {
                old = handle(tick.world, 2);
                tick.commands.destroy(*old);
            }
            if (tick.tick == 2) tick.bodies.add_force(*old, {1.0f, 0.0f, 0.0f});
        });
        CHECK(contains(failure(*later), "the entity does not exist"));
    }
    SECTION("An explicit remove keeps the entity") {
        auto removing = play(document, [&](TickContext& tick) {
            if (tick.tick == 0) tick.bodies.create(handle(tick.world, 2), sphere(MotionType::dynamic));
            if (tick.tick == 3) tick.bodies.remove(handle(tick.world, 2));
        });
        run(*removing, 5);
        CHECK(removing->world().alive(handle(removing->world(), 2)));
        CHECK_FALSE(removing->physics().has_body(handle(removing->world(), 2)));
        const auto kept = transform_of(removing->world(), 2).translation.y;
        run(*removing, 5);
        CHECK(transform_of(removing->world(), 2).translation.y == kept); // nothing moves it any more
    }
}

TEST_CASE("Pause and single step advance physics exactly one interval", "[physics]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 50.0f, 0.0f})}})};
    auto session = play(document, create_on_first_tick({{1, sphere(MotionType::dynamic)}}));
    run(*session, 5);
    session->clock().pause();
    session->update(frame * 10);
    CHECK(session->physics().stats().steps == 5);
    const auto paused = transform_of(session->world(), 1).translation.y;
    session->clock().step();
    session->update(frame * 10);
    CHECK(session->physics().stats().steps == 6);
    CHECK(transform_of(session->world(), 1).translation.y < paused);
}

TEST_CASE("The same scene and inputs give identical poses whatever the worker count", "[physics]") {
    const auto trace = [](int workers) {
        auto guard = Workers(workers);
        auto session = play(pile(300), pile_bodies(300));
        auto hashes = std::vector<uint64_t>{};
        for (int i = 0; i < 12; ++i) {
            run(*session, 20);
            hashes.push_back(pose_hash(session->world()));
        }
        CHECK(session->physics().stats().steps_with_errors == 0);
        return hashes;
    };
    const auto serial = trace(0);
    CHECK(serial == trace(4));
    CHECK(serial == trace(4));
    CHECK(serial == trace(1));
}

TEST_CASE("Physics limits fail a body's creation, not the world", "[physics]") {
    auto settings = PhysicsSettings{};
    settings.max_bodies = 3;
    auto session = play(pile(4), pile_bodies(4), settings, "Filler");
    CHECK(contains(failure(*session), "Filler: cannot create a dynamic body for entity 70 66: the physics world is full (3 bodies)"));
    CHECK(session->physics().stats().bodies == 3); // the bodies made before it stay
    auto document = SceneDocument{};
    settings = PhysicsSettings{};
    settings.collision_steps = 0;
    CHECK(PlaySession::start(document, {}, {}, {}, settings).error == "Physics could not start: Physics settings are out of range");
}

TEST_CASE("Repeated play sessions return physics memory to the empty-session baseline", "[physics]") {
    const auto cycle = [] {
        auto session = play(pile(50), pile_bodies(50));
        run(*session, 10);
    };
    cycle(); // the first session creates the job pool and Jolt's type registry, kept for the process
    const auto baseline = physics_memory().live_bytes;
    for (int i = 0; i < 100; ++i) cycle();
    CHECK(physics_memory().live_bytes == baseline);
    CHECK(physics_memory().allocations > 0);
}

TEST_CASE("An empty physics world's cost", "[.][physics][cost]") {
    { auto warm = PhysicsWorld{}; } // Jolt's process setup
    const auto before = physics_memory().live_bytes;
    reset_physics_peak();
    const auto started = std::chrono::steady_clock::now();
    auto world = std::make_unique<PhysicsWorld>();
    const auto created = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const auto held = physics_memory().live_bytes - before;
    auto destroyed = std::chrono::steady_clock::now();
    world.reset();
    std::printf("empty world: created in %.3f ms, holds %.2f MiB, destroyed in %.3f ms\n", created, double(held) / (1 << 20),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - destroyed).count());
}

TEST_CASE("Step cost at 1,000 and 10,000 bodies", "[.][physics][cost]") {
    for (const auto count : {1000, 10000}) {
        auto session = play(pile(count), pile_bodies(count), [&] {
            auto settings = PhysicsSettings{};
            settings.max_bodies = uint32_t(count + 16);
            return settings;
        }());
        run(*session, 1);
        reset_physics_peak();
        auto samples = std::vector<double>{};
        auto phases = std::array<double, 5>{};
        for (int i = 0; i < 300; ++i) {
            const auto before = std::chrono::steady_clock::now();
            run(*session, 1);
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
            const auto stats = session->physics().stats();
            phases[0] += stats.prepare_ms / 300.0;
            phases[1] += stats.step_ms / 300.0;
            phases[2] += stats.synchronize_ms / 300.0;
            phases[3] += stats.commit_ms / 300.0;
            phases[4] += stats.events_ms / 300.0;
        }
        std::sort(samples.begin(), samples.end());
        auto mean = 0.0;
        for (const auto sample : samples) mean += sample / double(samples.size());
        const auto stats = session->physics().stats();
        std::printf("%5d bodies, %d workers: tick mean %.3f ms, P95 %.3f ms, max %.3f ms; %zu awake at the end; "
                    "peak Jolt heap %.1f MiB; %llu steps with errors\n",
                    count, physics_worker_threads(), mean, samples[samples.size() * 95 / 100], samples.back(),
                    stats.active_bodies, double(physics_memory().peak_bytes) / (1 << 20),
                    static_cast<unsigned long long>(stats.steps_with_errors));
        std::printf("       means: prepare %.3f ms, step %.3f ms, synchronize %.3f ms, events %.3f ms (%.0f per tick), body commit %.3f ms\n",
                    phases[0], phases[1], phases[2], phases[4], double(stats.events) / double(stats.steps), phases[3]);
    }
}

// --- Authored bodies (#1019): maya.collider, maya.rigid_body, and maya.physics_settings -----------

namespace {
ColliderComponent box_collider(math::Vec3 half = math::Vec3(0.5f)) {
    auto collider = ColliderComponent{};
    collider.half_extents = half;
    return collider;
}
ColliderComponent sphere_collider(float radius = 0.5f) {
    auto collider = ColliderComponent{};
    collider.shape = ColliderShape::sphere;
    collider.radius = radius;
    return collider;
}
SceneEntity floor_entity() {
    return entity(1, {TransformComponent{at({0.0f, -0.1f, 0.0f}, {20.0f, 0.2f, 20.0f})}, box_collider()});
}
/// Starts a session of `document` with no systems.
PlayStartResult start(const SceneDocument& document) {
    return PlaySession::start(document, {}, {});
}
std::unique_ptr<PlaySession> authored(const SceneDocument& document) {
    auto started = start(document);
    INFO(started.error);
    REQUIRE(started);
    return std::move(started.session);
}
std::string refusal(const SceneDocument& document) {
    auto started = start(document);
    CHECK_FALSE(started);
    return started.error;
}
} // namespace

TEST_CASE("An authored stack of boxes settles and stays at rest", "[physics][authored]") {
    auto document = SceneDocument{};
    document.entities.push_back(floor_entity());
    for (int i = 0; i < 6; ++i)
        document.entities.push_back(entity(10 + i, {TransformComponent{at({0.0f, 0.5f + float(i), 0.0f})}, box_collider(),
                                                    RigidBodyComponent{}}));
    auto session = authored(document);
    const auto& physics = session->physics();
    CHECK(physics.stats().bodies == 7);
    CHECK(physics.stats().static_bodies == 1);
    CHECK(physics.motion_type(handle(session->world(), 1)) == MotionType::static_body);
    run(*session, 300);
    for (int i = 0; i < 6; ++i) {
        INFO("box " << i);
        const auto rested = transform_of(session->world(), 10 + i);
        CHECK(rested.translation.y == Approx(0.5f + float(i)).margin(0.03f));
        CHECK(std::abs(rested.translation.x) < 0.02f); // the solver settles a tall stack by about 1 cm
        CHECK(std::abs(rested.translation.z) < 0.02f);
        CHECK(physics.state(handle(session->world(), 10 + i))->sleeping);
    }
    const auto settled = pose_hash(session->world());
    run(*session, 300);
    CHECK(pose_hash(session->world()) == settled); // asleep: nothing moves at all
}

TEST_CASE("Authored bodies take their settings from the components", "[physics][authored]") {
    auto body = RigidBodyComponent{};
    body.mass = 5.0f;
    body.linear_damping = 0.0f;
    body.gravity_factor = 0.5f;
    body.linear_velocity = {2.0f, 0.0f, 0.0f};
    auto kinematic = RigidBodyComponent{};
    kinematic.motion = BodyMotion::kinematic;
    auto settings = PhysicsSettingsComponent{};
    settings.gravity = {0.0f, -2.0f, 0.0f};
    auto document = SceneDocument{};
    document.entities = {entity(1, {TransformComponent{at({0.0f, 50.0f, 0.0f})}, sphere_collider(), body}),
                         entity(2, {TransformComponent{at({5.0f, 50.0f, 0.0f})}, sphere_collider(), kinematic}),
                         entity(3, {settings})};
    auto session = authored(document);
    run(*session, 61); // bodies exist from the start: 61 steps
    const auto state = *session->physics().state(handle(session->world(), 1));
    CHECK(state.motion == MotionType::dynamic);
    CHECK(state.mass == Approx(5.0f));
    CHECK(state.linear_velocity.x == Approx(2.0f).margin(1e-4));
    CHECK(state.linear_velocity.y == Approx(-2.0f * 0.5f * 61.0f / 60.0f).margin(1e-3)); // scene gravity x factor
    CHECK(session->physics().motion_type(handle(session->world(), 2)) == MotionType::kinematic);
    CHECK(transform_of(session->world(), 2).translation.y == 50.0f); // kinematic: no target, no motion
}

TEST_CASE("Colliders below a rigid body form its compound shape", "[physics][authored]") {
    // A dumbbell: the body entity has no collider itself; two scaled children carry spheres.
    auto document = SceneDocument{};
    document.entities = {floor_entity(),
                         entity(2, {TransformComponent{at({0.0f, 3.0f, 0.0f})}, RigidBodyComponent{}}),
                         entity(3, {TransformComponent{at({-1.0f, 0.0f, 0.0f}, math::Vec3(0.5f))}, sphere_collider(1.0f)}, 2),
                         entity(4, {TransformComponent{at({1.0f, 0.0f, 0.0f}, math::Vec3(0.5f))}, sphere_collider(1.0f)}, 2),
                         entity(5, {TransformComponent{at({0.0f, 0.0f, 0.0f})}}, 2)}; // no collider: ignored
    auto session = authored(document);
    CHECK(session->physics().stats().bodies == 2);
    CHECK_FALSE(session->physics().has_body(handle(session->world(), 3)));
    run(*session, 300);
    // Both spheres (radius 1 x scale 0.5) rest on the floor, so the body's origin rests at 0.5.
    CHECK(transform_of(session->world(), 2).translation.y == Approx(0.49f).margin(0.015f));
    CHECK(session->physics().state(handle(session->world(), 2))->mass ==
          Approx(2.0 * 1000.0 * 4.0 / 3.0 * math::PI * 0.125).epsilon(1e-3));
}

TEST_CASE("Collision groups and masks filter authored colliders", "[physics][authored]") {
    auto ghost = box_collider();
    ghost.group = 3;
    ghost.mask = 1u << 3; // collides only with group 3: not the floor (group 0)
    auto picky = box_collider();
    picky.mask = 0xFFFFu & ~(1u << 3); // everything but group 3
    auto sensor = box_collider({2.0f, 0.1f, 2.0f});
    sensor.sensor = true;
    auto document = SceneDocument{};
    document.entities = {floor_entity(),
                         entity(2, {TransformComponent{at({0.0f, 2.0f, 0.0f})}, ghost, RigidBodyComponent{}}),
                         entity(3, {TransformComponent{at({3.0f, 2.0f, 0.0f})}, picky, RigidBodyComponent{}}),
                         entity(4, {TransformComponent{at({-3.0f, 1.0f, 0.0f})}, sensor}), // a static sensor slab
                         entity(5, {TransformComponent{at({-3.0f, 3.0f, 0.0f})}, box_collider(), RigidBodyComponent{}})};
    auto session = authored(document);
    run(*session, 180);
    CHECK(transform_of(session->world(), 2).translation.y < -5.0f); // fell through the floor
    CHECK(transform_of(session->world(), 3).translation.y == Approx(0.5f).margin(0.03f));
    CHECK(transform_of(session->world(), 5).translation.y == Approx(0.5f).margin(0.03f)); // through the sensor
}

TEST_CASE("Play refuses authored physics it cannot build, and starts nothing", "[physics][authored]") {
    const auto document = [](std::vector<SceneEntity> entities) {
        auto value = SceneDocument{};
        value.entities = std::move(entities);
        return value;
    };
    auto named = [](uint64_t low, std::string name, std::vector<ComponentValue> components, std::optional<uint64_t> parent = {}) {
        components.insert(components.begin(), NameComponent{std::move(name)});
        return entity(low, std::move(components), parent);
    };
    CHECK(refusal(document({named(2, "Crate", {TransformComponent{}, RigidBodyComponent{}})})) ==
          "Physics: entity 70 2 \"Crate\": a rigid body needs a collider on its entity or on an entity below it");
    CHECK(refusal(document({entity(2, {TransformComponent{}}), entity(3, {TransformComponent{}, box_collider(), RigidBodyComponent{}}, 2)})) ==
          "Physics: cannot create a dynamic body for entity 70 3: a dynamic body must be a root entity");
    CHECK(refusal(document({entity(2, {TransformComponent{at({}, {1.0f, 2.0f, 1.0f})}, sphere_collider()})})) ==
          "Physics: cannot create a static body for entity 70 2: a sphere needs uniform scale");
    CHECK(refusal(document({entity(2, {TransformComponent{at({}, {2.0f, 2.0f, 2.0f})}, box_collider(), RigidBodyComponent{}})})) ==
          "Physics: cannot create a dynamic body for entity 70 2: a dynamic body needs unit scale");
    auto grouped = box_collider();
    grouped.group = 2;
    CHECK(contains(refusal(document({entity(2, {TransformComponent{}, box_collider(), RigidBodyComponent{}}),
                                     entity(3, {TransformComponent{}, grouped}, 2)})),
                   "entity 70 3: its collider must have the same collision group, mask, and sensor setting"));
    auto rotated = box_collider();
    rotated.rotation = math::Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, 0.4f);
    CHECK(contains(refusal(document({entity(2, {TransformComponent{}, RigidBodyComponent{}}),
                                     entity(3, {TransformComponent{at({}, {1.0f, 3.0f, 1.0f})}, rotated}, 2)})),
                   "entity 70 3: a rotated collider cannot take its entity's nonuniform scale"));
    CHECK(contains(refusal(document({entity(2, {PhysicsSettingsComponent{}}), entity(3, {PhysicsSettingsComponent{}})})),
                   "entity 70 3: a scene has at most one physics settings component; entity 70 2 has one too"));
    // A refusal after other bodies were made leaves none behind: the second body fails, the first is undone.
    const auto before = physics_memory().live_bytes;
    CHECK(contains(refusal(document({entity(2, {TransformComponent{}, box_collider(), RigidBodyComponent{}}),
                                     entity(3, {TransformComponent{at({}, {1.0f, 2.0f, 1.0f})}, sphere_collider()})})),
                   "a sphere needs uniform scale"));
    CHECK(physics_memory().live_bytes == before);
}

TEST_CASE("The same authored scene plays identically every time", "[physics][authored]") {
    auto document = SceneDocument{};
    document.entities.push_back(floor_entity());
    for (int i = 0; i < 40; ++i) {
        auto body = RigidBodyComponent{};
        body.linear_velocity = {float(i % 5) - 2.0f, 0.0f, float(i % 3) - 1.0f};
        document.entities.push_back(entity(10 + i, {TransformComponent{at({float(i % 4) * 0.3f, 1.0f + float(i) * 1.2f, 0.0f})},
                                                    i % 2 ? sphere_collider() : box_collider(), body}));
    }
    const auto trace = [&](int workers) {
        auto guard = Workers(workers);
        auto session = authored(document);
        run(*session, 240);
        return pose_hash(session->world());
    };
    CHECK(trace(0) == trace(4));
    CHECK(trace(4) == trace(4));
}

// --- Queries and events (#1021) ----------------------------------------------------------------------

namespace {
BodyDesc in_group(BodyDesc body, uint8_t group) {
    body.group = group;
    return body;
}
BodyDesc as_sensor(BodyDesc body) {
    body.sensor = true;
    return body;
}
EntityId id(uint64_t low) { return EntityId{0x70, low}; }

/// A floor, and three boxes in a row along -Z: in groups 1 and 2, then a sensor.
SceneDocument corridor() {
    auto document = SceneDocument{};
    document.entities = {entity(1, {at({0, -0.5f, 0})}), entity(2, {at({0, 0.5f, -3})}), entity(3, {at({0, 0.5f, -6})}),
                         entity(4, {at({0, 0.5f, -9})})};
    return document;
}
std::function<void(TickContext&)> corridor_bodies() {
    return create_on_first_tick({{1, box(MotionType::static_body, {10, 0.5f, 10})},
                                 {2, in_group(box(MotionType::static_body), 1)},
                                 {3, in_group(box(MotionType::static_body), 2)},
                                 {4, as_sensor(box(MotionType::static_body))}});
}
std::vector<EntityId> ids(const std::vector<QueryHit>& hits) {
    auto result = std::vector<EntityId>{};
    for (const auto& hit : hits) result.push_back(hit.id);
    return result;
}
} // namespace

TEST_CASE("Raycasts, shape casts, and overlaps hit the chosen groups, nearest first", "[physics][query]") {
    auto session = play(corridor(), corridor_bodies());
    run(*session, 1);
    const auto& physics = session->physics();
    // Along -Z from the origin: the two boxes, not the sensor; the floor is below the ray.
    auto hits = physics.raycast({0, 0.5f, 0}, {0, 0, -2}, 20.0f);
    REQUIRE(ids(hits) == std::vector{id(2), id(3)});
    CHECK(hits[0].distance == Approx(2.5f).margin(1e-4));
    CHECK(hits[0].point.z == Approx(-2.5f).margin(1e-4));
    CHECK(hits[0].normal.z == Approx(1.0f).margin(1e-4));
    CHECK(hits[0].entity == handle(session->world(), 2));
    CHECK(hits[1].distance == Approx(5.5f).margin(1e-4));
    CHECK(ids(physics.raycast({0, 0.5f, 0}, {0, 0, -1}, 20.0f)) == ids(hits)); // the same results again
    // Filters: groups, sensors, and an ignored entity.
    auto group_2 = QueryFilter{};
    group_2.groups = uint16_t(1u << 2);
    CHECK(ids(physics.raycast({0, 0.5f, 0}, {0, 0, -1}, 20.0f, group_2)) == std::vector{id(3)});
    auto with_sensors = QueryFilter{};
    with_sensors.sensors = true;
    CHECK(ids(physics.raycast({0, 0.5f, 0}, {0, 0, -1}, 20.0f, with_sensors)) == std::vector{id(2), id(3), id(4)});
    auto ignoring = QueryFilter{};
    ignoring.ignore = handle(session->world(), 2);
    CHECK(ids(physics.raycast({0, 0.5f, 0}, {0, 0, -1}, 20.0f, ignoring)) == std::vector{id(3)});
    CHECK(physics.raycast({0, 0.5f, 0}, {0, 0, -1}, 2.0f).empty()); // too short
    // Down onto box 2: its top, then the floor below it, one hit per body.
    hits = physics.raycast({0, 5, -3}, {0, -1, 0}, 10.0f);
    REQUIRE(ids(hits) == std::vector{id(2), id(1)});
    CHECK(hits[0].distance == Approx(4.0f).margin(1e-4));
    CHECK(hits[0].normal.y == Approx(1.0f).margin(1e-4));

    // A sphere swept along the ray stops a radius short.
    hits = physics.shape_cast(SphereShape{0.25f}, {0, 0.5f, 0}, {}, {0, 0, -1}, 20.0f);
    REQUIRE(ids(hits) == std::vector{id(2), id(3)});
    CHECK(hits[0].distance == Approx(2.25f).margin(1e-3));
    CHECK(hits[0].normal.z == Approx(1.0f).margin(1e-3));
    // Overlaps list what a shape touches, by EntityId; a cast that starts touching has distance 0.
    CHECK(ids(physics.overlap(BoxShape{{0.5f, 0.5f, 0.5f}}, {0, 1.2f, -3}, {})).empty() == false);
    CHECK(ids(physics.overlap(BoxShape{{0.5f, 0.5f, 0.5f}}, {0, 1.2f, -3}, {})) == std::vector{id(2)});
    CHECK(ids(physics.overlap(BoxShape{{0.5f, 2.0f, 5.0f}}, {0, 0.5f, -5}, {})) == std::vector{id(1), id(2), id(3)});
    hits = physics.shape_cast(SphereShape{0.25f}, {0, 0.5f, -3}, {}, {0, 0, -1}, 1.0f);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].id == id(2));
    CHECK(hits[0].distance == 0.0f);
    // Bad queries are refused.
    CHECK_THROWS_AS(physics.raycast({0, 0, 0}, {0, 0, 0}, 1.0f), std::invalid_argument);
    CHECK_THROWS_AS(physics.raycast({0, 0, 0}, {0, 0, 1}, -1.0f), std::invalid_argument);
    CHECK_THROWS_AS(physics.shape_cast(SphereShape{-1.0f}, {0, 0, 0}, {}, {0, 0, 1}, 1.0f), std::invalid_argument);
}

namespace {
struct Recorded {
    PhysicsEventKind kind;
    uint64_t tick;
    EntityId first, second;
    bool removed;
    bool operator==(const Recorded&) const = default;
};
/// A floor; boxes 2 and 3 dropped onto it; sphere 5 dropped through the sensor band 4 onto the floor.
SceneDocument drops() {
    auto document = SceneDocument{};
    document.entities = {entity(1, {at({0, -0.5f, 0})}), entity(2, {at({0, 2, 0})}), entity(3, {at({3, 3, 0})}),
                         entity(4, {at({6, 2.5f, 0})}), entity(5, {at({6, 6, 0})})};
    return document;
}
std::function<void(TickContext&)> drop_bodies(std::function<void(TickContext&)> then = {}) {
    return [then](TickContext& tick) {
        if (tick.tick == 0) {
            tick.bodies.create(handle(tick.world, 1), box(MotionType::static_body, {20, 0.5f, 20}));
            tick.bodies.create(handle(tick.world, 2), box(MotionType::dynamic));
            tick.bodies.create(handle(tick.world, 3), box(MotionType::dynamic));
            tick.bodies.create(handle(tick.world, 4), as_sensor(box(MotionType::static_body, {1, 0.5f, 1})));
            tick.bodies.create(handle(tick.world, 5), sphere(MotionType::dynamic, 0.5f));
        }
        if (then) then(tick);
    };
}
std::vector<Recorded> record_drops(int workers, int ticks, std::function<void(TickContext&)> then = {},
                                   std::vector<PhysicsEvent>* all = nullptr) {
    set_physics_worker_threads(workers);
    auto recorded = std::vector<Recorded>{};
    {
        auto session = play_late(drops(), drop_bodies(then), [&](TickContext& tick) {
            if (tick.stopping) return; // only the step's events
            for (const auto& e : tick.events) {
                recorded.push_back({e.kind, e.tick, e.first, e.second, e.removed});
                if (all) all->push_back(e);
            }
        });
        run(*session, ticks);
    }
    set_physics_worker_threads(-1);
    return recorded;
}
size_t count(const std::vector<Recorded>& events, PhysicsEventKind kind, uint64_t first, uint64_t second) {
    return size_t(std::ranges::count_if(events, [&](const Recorded& e) { return e.kind == kind && e.first == id(first) && e.second == id(second); }));
}
} // namespace

TEST_CASE("Contacts and triggers begin and end once per pair, in the same order for any worker count", "[physics][events]") {
    const auto events = record_drops(0, 240);
    using Kind = PhysicsEventKind;
    // Each body lands once and stays in contact, also after it falls asleep.
    for (const uint64_t body : {2, 3, 5}) {
        INFO(body);
        CHECK(count(events, Kind::contact_begin, 1, body) == count(events, Kind::contact_end, 1, body) + 1);
    }
    CHECK(count(events, Kind::contact_end, 1, 2) == 0);
    // The sphere passes through the sensor band: in once, out once, and never a contact with it.
    CHECK(count(events, Kind::trigger_enter, 4, 5) == 1);
    CHECK(count(events, Kind::trigger_exit, 4, 5) == 1);
    CHECK(count(events, Kind::contact_begin, 4, 5) == 0);
    // Within a tick, events come by kind, then by the two EntityIds.
    for (size_t i = 1; i < events.size(); ++i)
        if (events[i].tick == events[i - 1].tick)
            CHECK(std::tuple(events[i - 1].kind, events[i - 1].first, events[i - 1].second) <=
                  std::tuple(events[i].kind, events[i].first, events[i].second));
    CHECK(std::ranges::none_of(events, [](const Recorded& e) { return e.removed || e.first >= e.second; }));
    // The same events, at the same ticks, with several worker threads.
    CHECK(record_drops(4, 240) == events);
}

TEST_CASE("Recipients being destroyed are skipped and counted; removed bodies end their contacts as removals", "[physics][events]") {
    using Kind = PhysicsEventKind;
    // Find the tick box 2 lands, then destroy it in that tick: its recipient is left out and counted.
    const auto first = record_drops(0, 120);
    const auto landing = std::ranges::find_if(first, [](const Recorded& e) { return e.kind == Kind::contact_begin && e.second == id(2); });
    REQUIRE(landing != first.end());
    const auto tick = landing->tick;
    const auto box_3 = std::ranges::find_if(first, [](const Recorded& e) { return e.kind == Kind::contact_begin && e.second == id(3); });
    REQUIRE(box_3 != first.end());
    const auto remove_at = std::max(tick, box_3->tick) + 5;
    auto all = std::vector<PhysicsEvent>{};
    auto skipped = uint64_t{0};
    set_physics_worker_threads(0);
    {
        auto session = play_late(drops(), drop_bodies([&](TickContext& t) {
            if (t.tick == tick) t.commands.destroy(handle(t.world, 2));
            if (t.tick == remove_at) t.bodies.remove(handle(t.world, 3)); // its entity stays
        }), [&](TickContext& t) {
            if (!t.stopping) all.insert(all.end(), t.events.begin(), t.events.end());
        });
        const auto before = session->physics().stats().event_recipients_skipped;
        run(*session, int(remove_at) + 5);
        skipped = session->physics().stats().event_recipients_skipped - before;
    }
    set_physics_worker_threads(-1);
    const auto landed = std::ranges::find_if(all, [&](const PhysicsEvent& e) { return e.kind == Kind::contact_begin && e.second == id(2); });
    REQUIRE(landed != all.end());
    CHECK(landed->tick == tick);
    CHECK(landed->first_entity.has_value()); // the floor gets it
    CHECK_FALSE(landed->second_entity.has_value()); // box 2 is being destroyed in this tick
    // Its body goes at the commit: the floor's contact with it ends as a removal, the next tick.
    const auto gone = std::ranges::find_if(all, [&](const PhysicsEvent& e) { return e.kind == Kind::contact_end && e.second == id(2); });
    REQUIRE(gone != all.end());
    CHECK(gone->removed);
    CHECK(gone->tick == tick + 1);
    CHECK_FALSE(gone->second_entity.has_value());
    CHECK(skipped >= 2);
    // Box 3's body removed, its entity kept: both hear the contact end, as a removal.
    const auto removed = std::ranges::find_if(all, [&](const PhysicsEvent& e) { return e.kind == Kind::contact_end && e.second == id(3); });
    REQUIRE(removed != all.end());
    CHECK(removed->removed);
    CHECK(removed->first_entity.has_value());
    CHECK(removed->second_entity.has_value());
}

TEST_CASE("Stopping the session ends every contact and trigger in progress, as removals", "[physics][events]") {
    auto at_stop = std::vector<PhysicsEvent>{};
    auto stopping = false;
    {
        auto document = drops();
        document.entities[4] = entity(5, {at({6, 2.5f, 0})}); // the sphere starts inside the sensor band
        auto session = play_late(document, drop_bodies([](TickContext& t) {
            if (t.tick > 0) t.bodies.set_linear_velocity(handle(t.world, 5), {0, 0, 0}); // hold it there
        }), [&](TickContext& t) {
            if (t.stopping) {
                stopping = true;
                at_stop.assign(t.events.begin(), t.events.end());
            }
        });
        run(*session, 120);
    }
    REQUIRE(stopping);
    using Kind = PhysicsEventKind;
    CHECK(std::ranges::all_of(at_stop, [](const PhysicsEvent& e) { return e.removed; }));
    const auto has = [&](Kind kind, uint64_t a, uint64_t b) {
        return std::ranges::any_of(at_stop, [&](const PhysicsEvent& e) { return e.kind == kind && e.first == id(a) && e.second == id(b); });
    };
    CHECK(has(Kind::contact_end, 1, 2));
    CHECK(has(Kind::contact_end, 1, 3));
    CHECK(has(Kind::trigger_exit, 4, 5));
}

TEST_CASE("After the step, systems read its results and their requests wait for the next step", "[physics][events]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {at({0, 10, 0})})};
    auto late_velocity = std::vector<float>{};
    auto before_step = 0.0f; // tick 4, before its step
    auto session = play_late(document, [&, create = create_on_first_tick({{1, sphere(MotionType::dynamic)}})](TickContext& t) {
        create(t);
        if (t.tick == 4) before_step = t.physics.state(handle(t.world, 1))->linear_velocity.y;
    }, [&](TickContext& t) {
        if (!t.physics.has_body(handle(t.world, 1))) return;
        late_velocity.push_back(t.physics.state(handle(t.world, 1))->linear_velocity.y); // this step's result
        if (t.tick == 3) {
            t.bodies.set_linear_velocity(handle(t.world, 1), {0, 5, 0});
            late_velocity.push_back(t.physics.state(handle(t.world, 1))->linear_velocity.y); // unchanged: the step is done
        }
    });
    run(*session, 5);
    // Ticks 1 to 3 fall; the request after tick 3's step applies before tick 4's.
    REQUIRE(late_velocity.size() == 5);
    CHECK(late_velocity[0] < 0.0f);
    CHECK(late_velocity[3] == late_velocity[2]);
    CHECK(before_step == late_velocity[2]); // the next tick starts from the completed step, untouched
    CHECK(late_velocity[4] == Approx(5.0f - 9.81f / 60.0f).epsilon(0.02));
    // Writing a dynamic body's transform after the step is refused like before it.
    auto refused = play_late(document, create_on_first_tick({{1, sphere(MotionType::dynamic)}}), [](TickContext& t) {
        if (t.tick == 2) t.commands.set_transform(handle(t.world, 1), at({0, 0, 0}));
    });
    const auto error = failure(*refused);
    CHECK(contains(error, "failed after the step"));
    CHECK(contains(error, "written by physics"));
}

TEST_CASE("Events in one tick are ordered by kind and EntityId, not by when bodies were made", "[physics][events]") {
    auto document = SceneDocument{};
    document.entities = {entity(1, {at({0, -0.5f, 0})}), entity(7, {at({-2, 1, 0})}), entity(8, {at({2, 1, 0})}), entity(9, {at({-2, 0.5f, 0})})};
    auto events = std::vector<PhysicsEvent>{};
    auto session = play_late(document, [](TickContext& t) {
        if (t.tick != 0) return;
        // Made in reverse: 9, 8, 7, then the floor, so Jolt's body order is the opposite of the EntityIds'.
        t.bodies.create(handle(t.world, 9), as_sensor(box(MotionType::static_body, {1, 0.5f, 1})));
        t.bodies.create(handle(t.world, 8), box(MotionType::dynamic));
        t.bodies.create(handle(t.world, 7), box(MotionType::dynamic));
        t.bodies.create(handle(t.world, 1), box(MotionType::static_body, {20, 0.5f, 20}));
    }, [&](TickContext& t) {
        if (!t.stopping) events.insert(events.end(), t.events.begin(), t.events.end());
    });
    run(*session, 60);
    // Box 7 starts inside the sensor, and both boxes land in the same tick.
    auto landing = std::vector<std::pair<PhysicsEventKind, EntityId>>{};
    auto first_tick = std::optional<uint64_t>{};
    for (const auto& e : events) {
        if (e.kind == PhysicsEventKind::trigger_enter) continue;
        if (!first_tick) first_tick = e.tick;
        if (e.tick == *first_tick) landing.emplace_back(e.kind, e.second);
    }
    CHECK(landing == std::vector<std::pair<PhysicsEventKind, EntityId>>{{PhysicsEventKind::contact_begin, id(7)}, {PhysicsEventKind::contact_begin, id(8)}});
    const auto entered = std::ranges::find_if(events, [](const PhysicsEvent& e) { return e.kind == PhysicsEventKind::trigger_enter; });
    REQUIRE(entered != events.end());
    CHECK(entered->first == id(7));
    CHECK(entered->second == id(9));
    // In the tick box 7 met the sensor, the contacts (if any) come first, then the trigger.
    for (size_t i = 1; i < events.size(); ++i)
        if (events[i].tick == events[i - 1].tick) CHECK(events[i - 1].kind <= events[i].kind);
}
