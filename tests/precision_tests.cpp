#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/scripting.hpp"
#include "maya/world/spatial.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <map>
#include <sstream>

// The origin-offset sweep (#1065, docs/architecture/runtime-world-contracts.md#coordinates): the same
// content far from the world's origin behaves as it does at it.
using namespace maya;
using Catch::Approx;

namespace {
constexpr auto frame = 1.0 / 60.0;
/// The distances swept: the origin, 100 m, 1 km, 10 km, W1's farthest corner (11.6 km), and 100 km.
constexpr double distances[] = {0.0, 100.0, 1000.0, 10000.0, 11600.0, 100000.0};
/// An offset `distance` from the origin, along a diagonal in x and z, as W1's cells are.
math::DVec3 offset_of(double distance) { return {distance * 0.6, 0.0, -distance * 0.8}; }

const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
SceneEntity entity(uint64_t low, std::vector<ComponentValue> components, std::optional<uint64_t> parent = {}) {
    auto value = SceneEntity{EntityId{0x65, low}, {}, std::move(components)};
    if (parent) value.parent = EntityId{0x65, *parent};
    return value;
}
TransformComponent placed(const math::DVec3& at, math::Quat rotation = {}) { return {at, rotation, math::Vec3(1.0f)}; }
EntityHandle live(const World& world, uint64_t low) { return *world.find(EntityId{0x65, low}); }
math::DVec3 world_position(const World& world, uint64_t low) { return world.world_matrix(live(world, low))->translation; }
} // namespace

TEST_CASE("World poses and scene files keep positions exact at every offset", "[precision]") {
    for (const auto distance : distances) {
        INFO("offset " << distance << " m");
        const auto offset = offset_of(distance);
        // A root far out, turned, and a child a few centimetres from it.
        const auto turn = math::Quat::from_axis_angle({0, 1, 0}, 0.7f);
        auto document = SceneDocument{};
        document.entities = {entity(1, {placed(offset + math::DVec3{1.25, 2.0, 3.0}, turn)}),
                             entity(2, {placed({0.01, 0.02, 0.03})}, 1)};
        auto built = instantiate_scene(document, any_asset);
        REQUIRE(built);
        const auto& world = *built.world;
        // The child's place relative to the root is the same as at the origin, to well under a micrometre.
        const auto relative = world_position(world, 2) - world_position(world, 1);
        const auto expected = turn.rotate({0.01f, 0.02f, 0.03f});
        CHECK(std::abs(relative.x - expected.x) < 1e-7);
        CHECK(std::abs(relative.y - expected.y) < 1e-7);
        CHECK(std::abs(relative.z - expected.z) < 1e-7);
        // A millimetre's step is a millimetre, which a float position cannot hold beyond about 8 km.
        auto moved = world_position(world, 1) + math::DVec3{0.001, 0.0, 0.0};
        CHECK(moved.x - world_position(world, 1).x == Approx(0.001).epsilon(1e-6));
        // Saved and read back, every translation is the same double.
        auto text = std::ostringstream{};
        REQUIRE(write_scene(text, capture_scene(world), any_asset).empty());
        CHECK(text.str().find("component maya.transform 2") != std::string::npos);
        auto read = read_scene(text.str(), any_asset);
        REQUIRE(read);
        auto reloaded = instantiate_scene(std::move(read.document), any_asset);
        REQUIRE(reloaded);
        CHECK(world_position(*reloaded.world, 1) == world_position(world, 1));
        CHECK(world_position(*reloaded.world, 2) == world_position(world, 2));
    }
}

TEST_CASE("Version 1 scene files load exactly as they did: their translations are the floats they were", "[precision][scene]") {
    const auto text = std::string(R"(maya-scene 1
entity 65 1
  component maya.transform 1
    translation 0.1 4096.37 -8191.99
    rotation 0 0 0 1
    scale 1 1 1
end
)");
    auto read = read_scene(text, any_asset);
    INFO((read.diagnostics.empty() ? std::string() : read.diagnostics.front().message));
    REQUIRE(read);
    const auto& transform = std::get<TransformComponent>(read.document.entities.front().components.front());
    CHECK(transform.translation.x == double(0.1f)); // not 0.1: the float it always was
    CHECK(transform.translation.y == double(4096.37f));
    CHECK(transform.translation.z == double(-8191.99f));
    // Saved again, it is written at version 2, and reads back the same.
    auto saved = std::ostringstream{};
    REQUIRE(write_scene(saved, read.document, any_asset).empty());
    CHECK(saved.str().find("component maya.transform 2") != std::string::npos);
    auto again = read_scene(saved.str(), any_asset);
    REQUIRE(again);
    CHECK(std::get<TransformComponent>(again.document.entities.front().components.front()).translation == transform.translation);
}

TEST_CASE("Physics at every offset matches the origin run: a resting stack and a rolling sphere", "[precision][physics]") {
    struct Result {
        std::vector<math::DVec3> positions; // relative to the offset
        std::vector<bool> sleeping;
    };
    const auto run = [](const math::DVec3& offset) {
        auto ground = ColliderComponent{};
        ground.half_extents = {30.0f, 0.5f, 30.0f};
        auto box = ColliderComponent{};
        box.half_extents = math::Vec3(0.25f);
        auto ball = ColliderComponent{};
        ball.shape = ColliderShape::sphere;
        ball.radius = 0.3f;
        auto rolling = RigidBodyComponent{};
        rolling.linear_velocity = {5.0f, 0.0f, 0.0f};
        auto document = SceneDocument{};
        document.entities = {entity(1, {placed(offset + math::DVec3{0, -0.5, 0}), ground}),
                             entity(2, {placed(offset + math::DVec3{0, 0.25, -3}), box, RigidBodyComponent{}}),
                             entity(3, {placed(offset + math::DVec3{0.05, 0.75, -3}), box, RigidBodyComponent{}}),
                             entity(4, {placed(offset + math::DVec3{-0.05, 1.25, -3}), box, RigidBodyComponent{}}),
                             entity(5, {placed(offset + math::DVec3{-10, 0.3, 4}), ball, rolling})};
        auto started = PlaySession::start(std::move(document), any_asset, builtin_systems());
        INFO(started.error);
        REQUIRE(started);
        auto& session = *started.session;
        for (int tick = 0; tick < 300; ++tick) REQUIRE(session.update(frame).error.empty()); // five seconds
        auto result = Result{};
        for (uint64_t low = 2; low <= 5; ++low) {
            result.positions.push_back(world_position(session.world(), low) - offset);
            result.sleeping.push_back(session.physics().state(live(session.world(), low))->sleeping);
        }
        return result;
    };
    const auto origin = run({});
    CHECK(origin.positions[3].x > -2.0); // the sphere rolled several metres
    for (const auto distance : distances) {
        INFO("offset " << distance << " m");
        const auto far = run(offset_of(distance));
        for (size_t i = 0; i < origin.positions.size(); ++i) {
            INFO("body " << i);
            // Declared tolerance: 0.1 mm (#1060 measured at most 0.03 mm with double-precision Jolt).
            CHECK((far.positions[i] - origin.positions[i]).length() < 1e-4);
        }
        CHECK(far.sleeping == origin.sleeping);
    }
}

TEST_CASE("Scripts read and move positions exactly at every offset", "[precision][scripting]") {
    const auto source = R"(
local Mover = {}
function Mover:start()
    self.start = self.entity:world_position()
end
function Mover:fixed_update(dt: number)
    -- A millimetre a tick: position + vector is a position, in double.
    self.entity:set_position(self.entity:position() + vector.create(0.001, 0, 0))
    local moved = self.entity:world_position() - self.start -- an offset: a (float) vector
    local hit = maya.raycast(self.entity:world_position(), vector.create(0, -1, 0), 10)
    if hit then
        maya.log(string.format("%.6f %.6f %.6f", moved.x, (self.entity:world_position() - hit.point).y, hit.point.x - self.entity:world_position().x))
    end
end
return Mover
)";
    for (const auto distance : distances) {
        INFO("offset " << distance << " m");
        const auto offset = offset_of(distance);
        auto ground = ColliderComponent{};
        ground.half_extents = {5.0f, 0.5f, 5.0f};
        auto document = SceneDocument{};
        document.entities = {entity(1, {placed(offset + math::DVec3{0, -0.5, 0}), ground}),
                             entity(2, {placed(offset + math::DVec3{0.25, 2.0, 0.25}),
                                        ScriptComponent{AssetRef<ScriptAsset>{AssetId{0x65, 0x99}}, {}}})};
        auto sources = std::map<AssetId, ScriptSource>{{AssetId{0x65, 0x99}, {"mover.luau", source}}};
        auto systems = builtin_systems();
        systems.push_back(script_system([&](AssetId id) -> ScriptSourceResult {
            const auto found = sources.find(id);
            return found == sources.end() ? ScriptSourceResult{std::nullopt, "missing"} : ScriptSourceResult{found->second, {}};
        }));
        auto started = PlaySession::start(std::move(document), any_asset, std::move(systems));
        INFO(started.error);
        REQUIRE(started);
        auto& session = *started.session;
        auto logs = std::vector<std::string>{};
        for (int tick = 0; tick < 10; ++tick) {
            auto result = session.update(frame);
            REQUIRE(result.error.empty());
            for (const auto& message : result.messages) logs.push_back(message.text);
        }
        // Ten millimetres, exactly as at the origin.
        const auto moved = world_position(session.world(), 2) - (offset + math::DVec3{0.25, 2.0, 0.25});
        CHECK(moved.x == Approx(0.010).margin(1e-9));
        REQUIRE_FALSE(logs.empty());
        INFO(logs.back());
        // The script saw the same: its offsets are small vectors, and the ray hit the ground 2 m below.
        CHECK(logs.back().ends_with("(mover.luau): 0.009000 2.000000 0.000000"));
    }
}

TEST_CASE("The Luau position type: arithmetic, comparison, text, and vectors accepted where positions go", "[precision][scripting]") {
    const auto source = R"(
local P = {}
function P:start()
    local p = maya.position.new(11600.000001, 2, -3)
    local q = p + vector.create(1, 0, 0)
    maya.log(tostring(q - p)) -- a vector
    maya.log(tostring(q))
    maya.log(tostring(p == maya.position.new(11600.000001, 2, -3)))
    maya.log(string.format("%.6f", (vector.create(1, 0, 0) + p).x))
    maya.log(string.format("%.6f", (p - vector.create(0.000001, 0, 0)).x))
    maya.log(tostring(p:to_vector()))
    self.entity:set_position(vector.create(1, 2, 3)) -- a vector is still accepted
end
return P
)";
    auto sources = std::map<AssetId, ScriptSource>{{AssetId{0x65, 0x98}, {"p.luau", source}}};
    auto document = SceneDocument{};
    document.entities = {entity(1, {placed({}), ScriptComponent{AssetRef<ScriptAsset>{AssetId{0x65, 0x98}}, {}}})};
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(script_system([&](AssetId id) -> ScriptSourceResult { return {sources.at(id), {}}; }));
    auto started = PlaySession::start(std::move(document), any_asset, std::move(systems));
    INFO(started.error);
    REQUIRE(started);
    auto result = started.session->update(frame);
    REQUIRE(result.error.empty());
    auto logs = std::vector<std::string>{};
    for (const auto& message : result.messages) logs.push_back(message.text);
    REQUIRE(logs.size() == 6);
    const auto said = [&](size_t i) { return logs[i].substr(logs[i].find("): ") + 3); }; // after "entity ... (p.luau): "
    CHECK(said(0) == "1, 0, 0");
    CHECK(said(1) == "position(11601.000001, 2, -3)");
    CHECK(said(2) == "true");
    CHECK(said(3) == "11601.000001");
    CHECK(said(4) == "11600.000000");
    CHECK(said(5).starts_with("11600"));
    started.session->update(frame);
    CHECK(world_position(started.session->world(), 1) == math::DVec3{1, 2, 3});
}
