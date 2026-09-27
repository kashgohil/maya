#include "maya/simulation/play_session.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace maya;
using Catch::Approx;

namespace {
constexpr auto frame = 1.0 / 60.0;

SceneEntity entity(uint64_t low, std::vector<ComponentValue> components, std::optional<uint64_t> parent = {}) {
    auto value = SceneEntity{EntityId{0x51, low}, {}, std::move(components)};
    if (parent) value.parent = EntityId{0x51, *parent};
    return value;
}
TransformComponent transform_of(const World& world, uint64_t low) {
    auto result = TransformComponent{};
    REQUIRE(world.with<TransformComponent>(*world.find(EntityId{0x51, low}), [&](const TransformComponent& value) { result = value; }));
    return result;
}
std::string text(const World& world) {
    auto out = std::ostringstream{};
    REQUIRE(write_scene(out, capture_scene(world), {}).empty());
    return out.str();
}
std::vector<InputEvent> keys(std::initializer_list<std::pair<KeyCode, bool>> changes) {
    auto events = std::vector<InputEvent>{};
    for (const auto& [key, down] : changes) events.push_back(KeyEvent{key, down, KeyModifiers::none});
    return events;
}

/// Records what it saw, and can fail on demand.
struct Journal {
    std::vector<std::string> events;
    bool fail_start = false;
    std::optional<uint64_t> fail_tick;
};
class Recorder final : public SimulationSystem {
public:
    Recorder(Journal& journal, std::string name) : m_journal(journal), m_name(std::move(name)) {}
    std::string_view name() const override { return m_name; }
    void start(const World& world) override {
        m_journal.events.push_back(m_name + ".start " + std::to_string(world.size()));
        if (m_journal.fail_start && m_name == "second") throw std::runtime_error("no luck");
    }
    void fixed_update(TickContext& tick) override {
        m_journal.events.push_back(m_name + ".tick " + std::to_string(tick.tick));
        if (m_journal.fail_tick == tick.tick) throw std::runtime_error("broke");
    }
    void stop() noexcept override { m_journal.events.push_back(m_name + ".stop"); }

private:
    Journal& m_journal;
    std::string m_name;
};
/// Moves entity 1 one metre along X each tick, reading what the previous tick committed.
class Mover final : public SimulationSystem {
public:
    std::string_view name() const override { return "Mover"; }
    void fixed_update(TickContext& tick) override {
        const auto entity = *tick.world.find(EntityId{0x51, 1});
        auto moved = transform_of(tick.world, 1);
        seen.push_back(moved.translation.x);
        moved.translation.x += 1.0f;
        tick.commands.set_transform(entity, moved);
    }
    std::vector<float> seen;
};
} // namespace

TEST_CASE("The fixed clock admits wall time as whole ticks with bounded catch-up", "[simulation][clock]") {
    auto clock = FixedClock{};
    CHECK(clock.interval() == Approx(1.0 / 60.0));
    // Exact multiples never lose a tick to rounding.
    uint64_t ticks = 0;
    for (int i = 0; i < 600; ++i) ticks += clock.advance(frame).ticks;
    CHECK(ticks == 600);
    // Fractions accumulate: 120 Hz frames give a tick every other frame.
    auto half = FixedClock{};
    auto pattern = std::vector<uint32_t>{};
    for (int i = 0; i < 6; ++i) pattern.push_back(half.advance(frame / 2).ticks);
    CHECK(pattern == std::vector<uint32_t>{0, 1, 0, 1, 0, 1});
    // A long frame is clamped to 0.25 s, then capped at four ticks; the rest is reported.
    auto slow = FixedClock{};
    const auto stalled = slow.advance(1.0);
    CHECK(stalled.ticks == 4);
    CHECK(stalled.rejected_time == Approx(0.75));
    CHECK(stalled.discarded_ticks == 11); // 0.25 s is 15 ticks
    CHECK(slow.total_rejected_time() == Approx(0.75));
    CHECK(slow.total_discarded_ticks() == 11);
    CHECK(slow.alpha() == Approx(0.0).margin(1e-6));
    // Invalid deltas are ignored and flagged.
    for (const auto bad : {-1.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        const auto result = slow.advance(bad);
        CHECK(result.invalid_delta);
        CHECK(result.ticks == 0);
    }
    CHECK_THROWS_AS(FixedClock(ClockSettings{0}), std::invalid_argument);
    CHECK_THROWS_AS(FixedClock(ClockSettings{60, 0}), std::invalid_argument);
    CHECK_THROWS_AS(FixedClock(ClockSettings{60, 4, 0.0}), std::invalid_argument);
}

TEST_CASE("Pausing stops time, stepping runs one tick, and resuming does not catch up", "[simulation][clock]") {
    auto clock = FixedClock{};
    CHECK(clock.advance(frame * 0.5).ticks == 0);
    CHECK(clock.alpha() == Approx(0.5f));
    clock.pause();
    CHECK(clock.alpha() == 1.0f);
    CHECK(clock.advance(10.0).ticks == 0); // paused time is neither simulated nor reported as rejected
    CHECK(clock.total_rejected_time() == 0.0);
    clock.step();
    CHECK(clock.advance(0.0).ticks == 1);
    CHECK(clock.advance(frame).ticks == 0); // one step is one tick
    clock.step();
    clock.step();
    CHECK(clock.advance(0.0).ticks == 1); // steps do not queue
    clock.resume();
    CHECK(clock.advance(frame * 0.5).ticks == 0); // the half tick from before the pause is gone
    CHECK(clock.advance(frame * 0.5).ticks == 1);
    clock.step(); // does nothing while running
    CHECK(clock.advance(0.0).ticks == 0);
}

TEST_CASE("Game input gives each edge to exactly one tick and keeps held state", "[simulation][input]") {
    auto input = GameInput{};
    input.feed(keys({{KeyCode::W, true}}));
    auto first = input.latch();
    CHECK(first.down(KeyCode::W));
    CHECK(first.went_down(KeyCode::W));
    auto catch_up = input.latch(); // a second tick in the same frame
    CHECK(catch_up.down(KeyCode::W));
    CHECK_FALSE(catch_up.went_down(KeyCode::W));
    // A press and release before the next tick report both, and the key is not held.
    input.feed(keys({{KeyCode::Space, true}, {KeyCode::Space, false}}));
    const auto tap = input.latch();
    CHECK(tap.went_down(KeyCode::Space));
    CHECK(tap.went_up(KeyCode::Space));
    CHECK_FALSE(tap.down(KeyCode::Space));
    // A repeated press of a held key is not a new press.
    input.feed(keys({{KeyCode::W, true}}));
    CHECK_FALSE(input.latch().went_down(KeyCode::W));
    // Frames without ticks keep edges and movement for the next tick that runs.
    input.feed({MouseMoveEvent{10, 10}});
    input.feed({MouseMoveEvent{14, 7}, MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}, ScrollEvent{0, 2}});
    input.feed({MouseMoveEvent{20, 7}});
    const auto moved = input.latch();
    CHECK(moved.look.x == 10.0f); // from the first position, not from the origin
    CHECK(moved.look.y == -3.0f);
    CHECK(moved.buttons_pressed.test(0));
    CHECK(moved.scroll == 2.0f);
    CHECK(input.latch().look.x == 0.0f);
    // Losing focus releases everything held, as a release edge.
    input.feed({FocusEvent{false}});
    const auto lost = input.latch();
    CHECK_FALSE(lost.down(KeyCode::W));
    CHECK(lost.went_up(KeyCode::W));
    CHECK_FALSE(lost.buttons_held.test(0));
    input.feed({MouseMoveEvent{500, 500}}); // a new baseline after release, not a jump
    CHECK(input.latch().look.x == 0.0f);
    // Keys outside the table are ignored.
    input.feed(keys({{KeyCode::Unknown, true}}));
    CHECK_FALSE(input.latch().down(KeyCode::Unknown));
}

TEST_CASE("A play session runs systems in order on its own World and commits each tick", "[simulation][session]") {
    auto authored = SceneDocument{};
    authored.entities = {entity(1, {TransformComponent{}}), entity(2, {TransformComponent{}, CameraComponent{}}),
                         entity(3, {TransformComponent{}, CameraComponent{}}, 2)};
    auto journal = Journal{};
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<Recorder>(journal, "first"));
    auto mover = std::make_unique<Mover>();
    auto* moves = mover.get();
    systems.push_back(std::move(mover));
    systems.push_back(std::make_unique<Recorder>(journal, "second"));
    auto started = PlaySession::start(authored, {}, std::move(systems));
    REQUIRE(started);
    auto& session = *started.session;
    CHECK(session.camera() == EntityId{0x51, 2}); // the first camera in document order
    CHECK(journal.events == std::vector<std::string>{"first.start 3", "second.start 3"});
    const auto played = session.update(frame * 3);
    CHECK(played.ticks_run == 3);
    CHECK(session.clock().tick() == 3);
    // Each tick sees what the previous one committed.
    CHECK(moves->seen == std::vector<float>{0.0f, 1.0f, 2.0f});
    CHECK(transform_of(session.world(), 1).translation.x == 3.0f);
    CHECK(journal.events.size() == 8);
    CHECK(journal.events[2] == "first.tick 0");
    CHECK(journal.events[3] == "second.tick 0");
    CHECK(journal.events[7] == "second.tick 2");
    // The document the session came from is untouched.
    CHECK(std::get<TransformComponent>(authored.entities[0].components[0]).translation.x == 0.0f);
    started.session.reset();
    CHECK(journal.events.back() == "first.stop");
    CHECK(journal.events[journal.events.size() - 2] == "second.stop");
}

TEST_CASE("Play session failures report why and leave nothing running", "[simulation][session]") {
    auto authored = SceneDocument{};
    authored.entities = {entity(1, {TransformComponent{}})};
    const auto systems = [](Journal& journal) {
        auto list = std::vector<std::unique_ptr<SimulationSystem>>{};
        list.push_back(std::make_unique<Recorder>(journal, "first"));
        list.push_back(std::make_unique<Recorder>(journal, "second"));
        list.push_back(std::make_unique<Recorder>(journal, "third"));
        return list;
    };
    SECTION("A system that fails to start: those entered are stopped in reverse, the rest never start") {
        auto journal = Journal{.fail_start = true};
        const auto started = PlaySession::start(authored, {}, systems(journal));
        CHECK_FALSE(started);
        CHECK(started.error == "second could not start: no luck");
        CHECK(journal.events == std::vector<std::string>{"first.start 1", "second.start 1", "second.stop", "first.stop"});
    }
    SECTION("A scene that cannot be built") {
        auto invalid = authored;
        invalid.entities.push_back(entity(1, {})); // a duplicate ID
        auto journal = Journal{};
        const auto started = PlaySession::start(invalid, {}, systems(journal));
        CHECK_FALSE(started);
        REQUIRE_FALSE(started.diagnostics.empty());
        CHECK(started.diagnostics.front().code == SceneError::duplicate_entity);
        CHECK(journal.events.empty());
        auto empty = std::vector<std::unique_ptr<SimulationSystem>>{};
        empty.push_back(nullptr);
        CHECK(PlaySession::start(authored, {}, std::move(empty)).error == "A play session cannot run an empty system");
    }
    SECTION("A system that fails during a tick stops the simulation at the last completed tick") {
        auto journal = Journal{.fail_tick = 2};
        auto list = systems(journal);
        list.insert(list.begin(), std::make_unique<Mover>());
        auto started = PlaySession::start(authored, {}, std::move(list));
        REQUIRE(started);
        const auto played = started.session->update(frame * 4);
        CHECK(played.ticks_run == 2);
        CHECK(played.error == "Tick 2: first failed: broke");
        CHECK(started.session->failed());
        CHECK(started.session->clock().tick() == 2);
        CHECK(transform_of(started.session->world(), 1).translation.x == 2.0f); // tick 2's move was not committed
        CHECK(started.session->update(frame).error == played.error); // nothing runs any more
        CHECK(transform_of(started.session->world(), 1).translation.x == 2.0f);
    }
}

TEST_CASE("Spin turns entities about their local axis at their speed", "[simulation][systems]") {
    auto authored = SceneDocument{};
    authored.entities = {entity(1, {TransformComponent{}, SpinComponent{{0, 2, 0}, math::PI}}),
                         entity(2, {TransformComponent{}, SpinComponent{{0, 0, 0}, 1.0f}}),
                         entity(3, {TransformComponent{{0, 0, 0}, math::Quat::from_axis_angle({1, 0, 0}, math::PI / 2), {1, 1, 1}},
                                    SpinComponent{{0, 1, 0}, -math::PI}})};
    auto started = PlaySession::start(authored, {}, builtin_systems());
    REQUIRE(started);
    auto& session = *started.session;
    for (int i = 0; i < 30; ++i) session.update(frame); // half a second
    // Half a turn at pi rad/s about +Y (the axis length does not matter).
    const auto spun = transform_of(session.world(), 1).rotation.rotate({1, 0, 0});
    CHECK(spun.x == Approx(0.0f).margin(1e-4));
    CHECK(spun.z == Approx(-1.0f).margin(1e-4));
    CHECK(transform_of(session.world(), 2).rotation.w == 1.0f); // a zero axis does not turn
    // The axis is local: after the authored X turn, local +Y points along world +Z.
    const auto local = transform_of(session.world(), 3).rotation.rotate({1, 0, 0});
    CHECK(local.x == Approx(0.0f).margin(1e-4));
    CHECK(local.y == Approx(-1.0f).margin(1e-4)); // negative speed turns the other way
    const auto rotation = transform_of(session.world(), 1).rotation;
    CHECK(std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w) ==
          Approx(1.0f).margin(1e-5));
}

TEST_CASE("Fly control moves and turns from gameplay input only", "[simulation][systems]") {
    auto authored = SceneDocument{};
    authored.entities = {entity(1, {TransformComponent{}, FlyControlComponent{2.0f, 0.01f}}),
                         entity(2, {TransformComponent{{5, 0, 0}, {}, {1, 1, 1}}})};
    auto started = PlaySession::start(authored, {}, builtin_systems());
    REQUIRE(started);
    auto& session = *started.session;
    session.update(frame * 10);
    CHECK(transform_of(session.world(), 1).translation.z == 0.0f); // no input, no movement
    session.input().feed(keys({{KeyCode::W, true}}));
    for (int i = 0; i < 30; ++i) session.update(frame);
    CHECK(transform_of(session.world(), 1).translation.z == Approx(-1.0f).margin(1e-4)); // 2 m/s for half a second
    session.input().feed(keys({{KeyCode::W, false}, {KeyCode::LeftShift, true}, {KeyCode::E, true}}));
    for (int i = 0; i < 15; ++i) session.update(frame);
    CHECK(transform_of(session.world(), 1).translation.y == Approx(2.0f).margin(1e-4)); // up, four times faster
    session.input().feed(keys({{KeyCode::LeftShift, false}, {KeyCode::E, false}}));
    // Mouse movement turns: yaw left by 0.5 rad, and pitch stops short of straight up.
    session.input().feed({MouseMoveEvent{100, 100}, MouseMoveEvent{50, -500}});
    session.update(frame);
    const auto look = transform_of(session.world(), 1).rotation.rotate({0, 0, -1});
    CHECK(std::atan2(-look.x, -look.z) == Approx(0.5f).margin(1e-4));
    CHECK(look.y > 0.999f);
    CHECK(transform_of(session.world(), 2).translation.x == 5.0f); // entities without the component stay
}

TEST_CASE("The same scene, inputs, and frame times give the same result", "[simulation][session]") {
    auto authored = SceneDocument{};
    authored.entities = {entity(1, {TransformComponent{}, SpinComponent{{1, 1, 0}, 0.7f}}),
                         entity(2, {TransformComponent{}, CameraComponent{}, FlyControlComponent{}}),
                         entity(3, {TransformComponent{{0, 1, 0}, {}, {1, 1, 1}}, SpinComponent{}}, 1)};
    const auto run = [&] {
        auto started = PlaySession::start(authored, {}, builtin_systems());
        REQUIRE(started);
        auto& session = *started.session;
        for (int i = 0; i < 240; ++i) {
            if (i == 10) session.input().feed(keys({{KeyCode::D, true}}));
            if (i == 50) session.input().feed({MouseMoveEvent{0, 0}, MouseMoveEvent{30, 12}});
            if (i == 90) session.input().feed(keys({{KeyCode::D, false}}));
            session.update(i % 7 == 0 ? frame * 2.5 : frame * 0.8); // uneven frames
        }
        return std::pair{text(session.world()), session.clock().tick()};
    };
    const auto first = run(), second = run();
    CHECK(first.first == second.first);
    CHECK(first.second == second.second);
}
