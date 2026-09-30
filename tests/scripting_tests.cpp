#include "maya/simulation/play_session.hpp"
#include "maya/simulation/scripting.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <map>

// Luau scripts in play sessions (#1018, docs/scripting.md).
using namespace maya;
using Catch::Approx;

namespace {
constexpr auto frame = 1.0 / 60.0;

AssetId script_id(uint64_t low) { return {0x5c, low}; }

/// Script sources by asset ID, named "<name>.luau".
struct Scripts {
    std::map<AssetId, ScriptSource> sources;
    AssetId add(uint64_t low, const std::string& name, std::string text) {
        sources[script_id(low)] = {name + ".luau", std::move(text)};
        return script_id(low);
    }
    ScriptSources lookup() const {
        return [this](AssetId id) -> ScriptSourceResult {
            const auto found = sources.find(id);
            if (found == sources.end()) return {std::nullopt, "not in the test's sources"};
            return {found->second, {}};
        };
    }
};

SceneEntity entity(uint64_t low, std::vector<ComponentValue> components, std::optional<uint64_t> parent = {}) {
    auto value = SceneEntity{EntityId{0x70, low}, {}, std::move(components)};
    if (parent) value.parent = EntityId{0x70, *parent};
    return value;
}
ScriptComponent script(AssetId id, std::vector<ScriptValue> values = {}) {
    return ScriptComponent{AssetRef<ScriptAsset>{id}, std::move(values)};
}
EntityHandle handle(const World& world, uint64_t low) {
    return *world.find(EntityId{0x70, low});
}
TransformComponent transform_of(const World& world, uint64_t low) {
    auto result = TransformComponent{};
    REQUIRE(world.with<TransformComponent>(handle(world, low), [&](const TransformComponent& value) { result = value; }));
    return result;
}
const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};

struct Session {
    std::unique_ptr<PlaySession> play;
    std::vector<SimulationMessage> messages;
    /// Runs `ticks` frames of one tick each, collecting what the systems report.
    void run(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            auto result = play->update(frame);
            INFO(play->error());
            REQUIRE(result.error.empty());
            for (auto& message : result.messages) messages.push_back(std::move(message));
        }
    }
    std::vector<std::string> texts(SimulationMessage::Level level) const {
        auto result = std::vector<std::string>{};
        for (const auto& message : messages)
            if (message.level == level) result.push_back(message.text);
        return result;
    }
    bool said(const std::string& part) const {
        for (const auto& message : messages)
            if (message.text.find(part) != std::string::npos) return true;
        return false;
    }
    World& world() { return play->world(); }
};

Session play(const Scripts& scripts, std::vector<SceneEntity> entities, ScriptSettings settings = {},
             std::vector<std::unique_ptr<SimulationSystem>> before = {}) {
    auto document = SceneDocument{};
    document.entities = std::move(entities);
    auto systems = std::move(before);
    systems.push_back(script_system(scripts.lookup(), settings));
    auto started = PlaySession::start(std::move(document), any_asset, std::move(systems));
    INFO(started.error);
    REQUIRE(started);
    return Session{std::move(started.session), {}};
}

/// A native system for tests: runs a callback each tick.
class Native final : public SimulationSystem {
public:
    explicit Native(std::function<void(TickContext&)> tick) : m_tick(std::move(tick)) {}
    std::string_view name() const override { return "Native"; }
    void fixed_update(TickContext& tick) override { m_tick(tick); }

private:
    std::function<void(TickContext&)> m_tick;
};
} // namespace

TEST_CASE("Scripts declare their properties and hooks", "[scripting]") {
    const auto described = describe_script("mover.luau", R"(
local Mover = {}
Mover.properties = {
    speed = { type = "number", default = 3, min = 0, max = 10, unit = "m/s", label = "Speed" },
    target = { type = "entity" },
    tint = { type = "color" },
    jumps = { type = "integer", default = 2 },
    enabled = { type = "boolean", default = true },
    greeting = { type = "string", default = "hi" },
    direction = { type = "vector", default = vector.create(0, 0, -1) },
}
function Mover:start() end
function Mover:fixed_update(dt: number) end
return Mover
)");
    INFO(described.error);
    REQUIRE(described);
    CHECK(described.hooks == std::vector<std::string>{"start", "fixed_update"});
    REQUIRE(described.properties.size() == 7);
    const auto& names = described.properties;
    CHECK(names[0].name == "direction"); // sorted by name
    CHECK(names[6].name == "tint");
    const auto& speed = names[4];
    CHECK(speed.name == "speed");
    CHECK(speed.type == ScriptValueType::number);
    CHECK(std::get<float>(speed.default_value) == 3.0f);
    CHECK(speed.minimum == 0.0f);
    CHECK(speed.maximum == 10.0f);
    CHECK(speed.unit == "m/s");
    CHECK(speed.label == "Speed");
    CHECK(std::get<EntityId>(names[5].default_value) == EntityId{}); // target: none
    CHECK(std::get<math::Vec3>(names[6].default_value).x == 1.0f); // a color defaults to white
    CHECK(std::get<math::Vec3>(names[0].default_value).z == -1.0f);

    const auto error = [](std::string source) { return describe_script("bad.luau", source).error; };
    CHECK(error("local x = 1\nfunction broken(\n").starts_with("bad.luau:3:"));
    CHECK(error("return 5") == "bad.luau: a script must return a table of hooks and properties");
    CHECK(error("return { properties = { speed = { default = 1 } } }").find("needs a type") != std::string::npos);
    CHECK(error("return { properties = { speed = { type = \"number\", default = \"fast\" } } }").find("not a number") != std::string::npos);
    CHECK(error("return { properties = { on = { type = \"boolean\", min = 0 } } }").find("for number and integer") != std::string::npos);
    CHECK(error("return { properties = { speed = { type = \"number\", default = 5, max = 1 } } }").find("outside its range") != std::string::npos);
    CHECK(error("return { properties = { [\"two words\"] = { type = \"number\" } } }").find("identifiers") != std::string::npos);
    CHECK(error("maya.log('loading')\nreturn {}").find("not available while a script loads") != std::string::npos);
    CHECK(error("while true do end").find("work budget exceeded") != std::string::npos);
}

TEST_CASE("Scripts cannot reach the system, load code, or change shared tables", "[scripting]") {
    for (const auto* name : {"io", "debug", "os", "print", "getfenv", "setfenv", "load", "loadstring", "loadfile", "dofile",
                             "require", "collectgarbage"}) {
        INFO(name);
        CHECK(describe_script("probe.luau", std::string("assert(") + name + " == nil)\nreturn {}").error.empty());
    }
    CHECK(describe_script("vandal.luau", "math.pi = 3\nreturn {}").error.find("readonly") != std::string::npos);
    CHECK(describe_script("vandal.luau", "maya.log = nil\nreturn {}").error.find("readonly") != std::string::npos);
    CHECK(describe_script("vandal.luau", "string.rep = nil\nreturn {}").error.find("readonly") != std::string::npos);
    // Each script has its own globals.
    auto scripts = Scripts{};
    const auto first = scripts.add(1, "first", "shared = 42\nreturn {}");
    const auto second = scripts.add(2, "second", R"(
local S = {}
function S:start() maya.log(tostring(shared)) end
return S
)");
    auto session = play(scripts, {entity(1, {script(first)}), entity(2, {script(second)})});
    session.run();
    CHECK(session.texts(SimulationMessage::Level::info) == std::vector<std::string>{"entity 70 2 (second.luau): nil"});
}

TEST_CASE("Script hooks run once per activation, each tick, and at the end, in activation order", "[scripting]") {
    auto scripts = Scripts{};
    const auto logger = scripts.add(1, "logger", R"(
local L = {}
function L:start()
    self.name = self.entity:name() -- stop may run after the entity is gone
    maya.log("start " .. self.name .. " at " .. maya.tick())
end
function L:fixed_update(dt) if maya.tick() < 2 then maya.log("tick " .. self.name .. " " .. maya.tick()) end end
function L:stop() maya.log("stop " .. self.name .. ", entity alive: " .. tostring(self.entity:alive())) end
return L
)");
    auto added = false;
    auto session = play(scripts, {entity(1, {NameComponent{"A"}, script(logger)}), entity(2, {NameComponent{"B"}, script(logger)}),
                                  entity(3, {NameComponent{"C"}})},
                        {}, [&] {
                            auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
                            systems.push_back(std::make_unique<Native>([&](TickContext& tick) {
                                // A script attached during play starts at the next tick; destroying an entity stops it.
                                if (tick.tick == 1 && !added) {
                                    tick.commands.add(handle(tick.world, 3), script(logger));
                                    added = true;
                                }
                                if (tick.tick == 3) tick.commands.destroy(handle(tick.world, 1));
                            }));
                            return systems;
                        }());
    session.run(5);
    const auto logs = session.texts(SimulationMessage::Level::info);
    const auto expected = std::vector<std::string>{
        "A (logger.luau): start A at 0", "B (logger.luau): start B at 0", "A (logger.luau): tick A 0", "B (logger.luau): tick B 0",
        "A (logger.luau): tick A 1", "B (logger.luau): tick B 1", "C (logger.luau): start C at 2", "A (logger.luau): stop A, entity alive: false"};
    CHECK(logs == expected);
    session.messages.clear();
    session.play.reset(); // the end of play: stop hooks, newest first
    // (Stop messages at the end of play have no frame to go to; their effects are what matter.)
}

TEST_CASE("Authored values reach the instance; values that do not fit are reported and replaced", "[scripting]") {
    auto scripts = Scripts{};
    const auto reader = scripts.add(1, "reader", R"(
local R = {}
R.properties = {
    speed = { type = "number", default = 3, min = 0, max = 10 },
    label = { type = "string", default = "none" },
    target = { type = "entity" },
    count = { type = "integer", default = 1 },
}
function R:start()
    maya.log(self.speed, self.label, self.count, self.target and self.target:name() or "no target")
end
return R
)");
    auto session = play(scripts, {entity(1, {NameComponent{"Reader"}, script(reader, {{"speed", ScriptValueType::number, 7.5f},
                                                                                   {"label", ScriptValueType::string, std::string("hello")},
                                                                                   {"target", ScriptValueType::entity, EntityId{0x70, 2}}})}),
                                  entity(2, {NameComponent{"Goal"}}),
                                  entity(3, {NameComponent{"Odd"}, script(reader, {{"speed", ScriptValueType::number, 20.0f},
                                                                                {"count", ScriptValueType::string, std::string("x")},
                                                                                {"colour", ScriptValueType::color, math::Vec3(1.0f)}})})});
    session.run();
    CHECK(session.texts(SimulationMessage::Level::info) ==
          std::vector<std::string>{"Reader (reader.luau): 7.5 hello 1 Goal", "Odd (reader.luau): 3 none 1 no target"});
    const auto warnings = session.texts(SimulationMessage::Level::warning);
    REQUIRE(warnings.size() == 3);
    CHECK(warnings[0] == "Odd (reader.luau): 'speed' is outside its range (0 to 10); the default is used");
    CHECK(warnings[1] == "Odd (reader.luau): 'count' holds a string but the script declares a integer; the default is used");
    CHECK(warnings[2] == "Odd (reader.luau): 'colour' is not a property of the script; its value is kept but unused");
}

TEST_CASE("Scripts move entities, edit properties, and create and destroy entities through commands", "[scripting]") {
    auto scripts = Scripts{};
    const auto worker = scripts.add(1, "worker", R"(
local W = {}
function W:fixed_update(dt)
    local t = maya.tick()
    if t == 0 then
        self.entity:set_position(self.entity:position() + vector.create(1, 0, 0))
        self.entity:set_rotation(maya.quaternion.from_axis_angle(vector.create(0, 1, 0), math.pi / 2))
        maya.find("70 2"):set("maya.light", "intensity", 5)
        maya.find("70 2"):set("light", "kind", "spot")
        self.spawned = maya.create("Spawned", vector.create(0, 2, 0), self.entity)
        maya.log("spawned alive now: " .. tostring(self.spawned:alive()))
    elseif t == 1 then
        maya.log("spawned alive next tick: " .. tostring(self.spawned:alive()) .. " " .. self.spawned:name() .. " " .. tostring(self.spawned:parent() == self.entity))
        maya.find("70 3"):destroy()
    elseif t == 2 then
        maya.log("gone: " .. tostring(maya.find("70 3") == nil))
    end
end
return W
)");
    auto session = play(scripts, {entity(1, {TransformComponent{}, script(worker)}),
                                  entity(2, {TransformComponent{}, LightComponent{}}),
                                  entity(3, {TransformComponent{}})});
    session.run();
    CHECK(transform_of(session.world(), 1).translation.x == 1.0f);
    CHECK(transform_of(session.world(), 1).rotation.y == Approx(std::sqrt(0.5f)));
    auto light = LightComponent{};
    session.world().with<LightComponent>(handle(session.world(), 2), [&](const LightComponent& value) { light = value; });
    CHECK(light.intensity == 5.0f);
    CHECK(light.kind == LightKind::spot);
    session.run(2);
    CHECK(session.texts(SimulationMessage::Level::info) ==
          std::vector<std::string>{"entity 70 1 (worker.luau): spawned alive now: false",
                                   "entity 70 1 (worker.luau): spawned alive next tick: true Spawned true",
                                   "entity 70 1 (worker.luau): gone: true"});
    CHECK(session.world().size() == 3); // one spawned, one destroyed
}

TEST_CASE("Script errors name the script and line, discard the call's changes, and stop only that instance", "[scripting]") {
    auto scripts = Scripts{};
    const auto faulty = scripts.add(1, "faulty", R"(
local F = {}
function F:fixed_update(dt)
    self.entity:set_position(vector.create(9, 9, 9))
    maya.create("Never", vector.zero)
    if maya.tick() == 1 then
        local missing = nil
        missing()
    end
end
function F:stop() maya.log("stopping") end
return F
)");
    const auto steady = scripts.add(2, "steady", R"(
local S = {}
function S:fixed_update(dt) self.entity:set_position(self.entity:position() + vector.create(0, 1, 0)) end
return S
)");
    const auto broken = scripts.add(3, "broken", "local x = \nreturn {}");
    auto session = play(scripts, {entity(1, {NameComponent{"Faulty"}, TransformComponent{}, script(faulty)}),
                                  entity(2, {TransformComponent{}, script(steady)}),
                                  entity(3, {NameComponent{"Broken"}, script(broken)}),
                                  entity(4, {NameComponent{"Broken too"}, script(broken)})});
    session.run(); // tick 0 works
    CHECK(session.world().size() == 5);
    session.run(); // tick 1 fails after moving and creating: both are discarded
    CHECK(transform_of(session.world(), 1).translation.x == 9.0f); // from tick 0
    CHECK(session.world().size() == 5);
    session.run(3);
    CHECK(transform_of(session.world(), 2).translation.y == 5.0f); // the other script kept going
    const auto errors = session.texts(SimulationMessage::Level::error);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0] == "Broken (broken.luau): broken.luau:2: Expected identifier when parsing expression, got 'return'");
    CHECK(errors[1].starts_with("Faulty (faulty.luau): faulty.luau:8: attempt to call a nil value"));
    CHECK(errors[1].find("(in fixed_update; the instance is stopped)") != std::string::npos);
}

TEST_CASE("Errors in start, update, and stop stop only their instance, and stop follows a failed start", "[scripting]") {
    auto scripts = Scripts{};
    const auto bad_start = scripts.add(1, "bad_start", R"(
local B = {}
function B:start() error("cannot start") end
function B:fixed_update(dt) maya.log("start failed but fixed_update ran") end
function B:stop() maya.log("stopped after a failed start") end
return B
)");
    const auto bad_update = scripts.add(2, "bad_update", R"(
local B = {}
function B:fixed_update(dt) maya.log("fixed " .. maya.tick()) end
function B:update(dt) error("cannot update") end
return B
)");
    const auto bad_stop = scripts.add(3, "bad_stop", R"(
local B = {}
function B:stop() error("cannot stop") end
return B
)");
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<Native>([](TickContext& tick) {
        if (tick.tick == 2) tick.commands.destroy(handle(tick.world, 3));
        if (tick.tick == 3) tick.commands.destroy(handle(tick.world, 1)); // its stop runs though start failed
    }));
    auto session = play(scripts, {entity(1, {script(bad_start)}), entity(2, {script(bad_update)}), entity(3, {script(bad_stop)})}, {},
                        std::move(systems));
    session.run(5);
    CHECK(session.texts(SimulationMessage::Level::info) ==
          std::vector<std::string>{"entity 70 2 (bad_update.luau): fixed 0", "entity 70 1 (bad_start.luau): stopped after a failed start"});
    const auto errors = session.texts(SimulationMessage::Level::error);
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].find("bad_start.luau:3: cannot start") != std::string::npos);
    CHECK(errors[0].find("(in start;") != std::string::npos);
    CHECK(errors[1].find("bad_update.luau:4: cannot update") != std::string::npos);
    CHECK(errors[1].find("(in update;") != std::string::npos);
    CHECK(errors[2].find("bad_stop.luau:3: cannot stop") != std::string::npos);
    CHECK(errors[2].find("(in stop;") != std::string::npos);
    CHECK(session.world().size() == 1); // both destroys went through
    // A failing native system still stops the session, scripts or not.
    auto natives = std::vector<std::unique_ptr<SimulationSystem>>{};
    natives.push_back(std::make_unique<Native>([](TickContext& tick) {
        if (tick.tick == 1) throw std::runtime_error("native failure");
    }));
    auto failing = play(scripts, {entity(2, {script(bad_update)})}, {}, std::move(natives));
    failing.run();
    const auto stopped = failing.play->update(frame);
    CHECK(stopped.error == "Tick 1: Native failed: native failure");
}

TEST_CASE("Budgets stop runaway scripts; memory limits stop hoarding ones", "[scripting]") {
    auto scripts = Scripts{};
    const auto spinner = scripts.add(1, "spinner", "local S = {}\nfunction S:fixed_update(dt) while true do end end\nreturn S");
    const auto hoarder = scripts.add(2, "hoarder",
                                     "local H = {}\nfunction H:fixed_update(dt) local t = {} for i = 1, 1e8 do t[i] = i end end\nreturn H");
    const auto counter = scripts.add(3, "counter", R"(
local C = {}
function C:fixed_update(dt)
    local total = 0
    for i = 1, 1000 do total += i end
    self.total = (self.total or 0) + total
    if maya.tick() == 3 then maya.log(tostring(self.total)) end
end
return C
)");
    auto settings = ScriptSettings{};
    settings.limits = {100'000, size_t{8} << 20};
    auto session = play(scripts, {entity(1, {script(spinner)}), entity(3, {script(counter)})}, settings);
    session.run(4);
    CHECK(session.said("work budget exceeded (100000 safepoints)"));
    CHECK(session.texts(SimulationMessage::Level::info) == std::vector<std::string>{"entity 70 3 (counter.luau): 2002000"});
    settings.limits.work_per_call = 1'000'000'000; // enough work to run into the memory limit
    auto hoarding = play(scripts, {entity(2, {script(hoarder)}), entity(3, {script(counter)})}, settings);
    hoarding.run(4);
    CHECK(hoarding.said("not enough memory"));
    CHECK(hoarding.said("2002000")); // the other script is unaffected
    // Setting up a script and its instances runs within the limit too. Here one script fills the VM,
    // then another arrives: it cannot be set up, which is its error, and play goes on.
    const auto filler = scripts.add(4, "filler", R"(
local F = {}
function F:start()
    for _, size in {4096, 256, 16, 1, 0} do
        pcall(function() while true do self.hoard = {self.hoard, table.create(size, 0)} end end)
    end
end
return F
)");
    settings.limits.memory_bytes = size_t{2} << 20;
    auto ticks = 0;
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<Native>([&](TickContext& tick) {
        if (tick.tick == 1) tick.commands.add(handle(tick.world, 3), script(counter));
        ++ticks;
    }));
    auto full = play(scripts, {entity(4, {script(filler)}), entity(3, {})}, settings, std::move(systems));
    full.run(4);
    CHECK(full.texts(SimulationMessage::Level::error) == std::vector<std::string>{"entity 70 3 (counter.luau): counter.luau: not enough memory"});
    CHECK(ticks == 4);
}

TEST_CASE("Scripts cannot move physics bodies, and update hooks cannot change the simulation", "[scripting]") {
    auto scripts = Scripts{};
    const auto pusher = scripts.add(1, "pusher", "local P = {}\nfunction P:fixed_update(dt) self.entity:set_position(vector.one) end\nreturn P");
    const auto watcher = scripts.add(2, "watcher", R"(
local W = {}
function W:update(dt) self.entity:set_position(vector.one) end
return W
)");
    auto session = play(scripts, {entity(1, {TransformComponent{}, ColliderComponent{}, RigidBodyComponent{}, script(pusher)}),
                                  entity(2, {TransformComponent{}, script(watcher)})});
    session.run(2);
    CHECK(session.said("cannot set the transform of entity 70 1: its dynamic body's pose is written by physics"));
    CHECK(session.said("update hooks cannot move entities; make changes in fixed_update"));
    CHECK(transform_of(session.world(), 2).translation.x == 0.0f);
}

TEST_CASE("Two scripts writing one property in a tick: the later wins, and both are named", "[scripting]") {
    auto scripts = Scripts{};
    const auto left = scripts.add(1, "left", "local S = {}\nfunction S:fixed_update(dt) maya.find('70 3'):set_position(vector.create(-1, 0, 0)) end\nreturn S");
    const auto right = scripts.add(2, "right", "local S = {}\nfunction S:fixed_update(dt) maya.find('70 3'):set_position(vector.create(1, 0, 0)) end\nreturn S");
    auto session = play(scripts, {entity(1, {NameComponent{"Left"}, script(left)}), entity(2, {NameComponent{"Right"}, script(right)}),
                                  entity(3, {TransformComponent{}})});
    session.run();
    CHECK(transform_of(session.world(), 3).translation.x == 1.0f);
    CHECK(session.texts(SimulationMessage::Level::warning) ==
          std::vector<std::string>{"Left (left.luau) and Right (right.luau) both set maya.transform.translation of entity 70 3 this "
                                   "tick; Right (right.luau)'s value wins"});
}

TEST_CASE("Scripts read this tick's input", "[scripting]") {
    auto scripts = Scripts{};
    const auto reader = scripts.add(1, "reader", R"(
local R = {}
function R:fixed_update(dt)
    if maya.input.pressed("W") then maya.log("W down at " .. maya.tick()) end
    if maya.input.down("Space") then maya.log("space held") end
end
return R
)");
    auto session = play(scripts, {entity(1, {script(reader)})});
    session.run();
    session.play->input().feed({KeyEvent{KeyCode::W, true, KeyModifiers::none}});
    session.run();
    session.run();
    CHECK(session.texts(SimulationMessage::Level::info) == std::vector<std::string>{"entity 70 1 (reader.luau): W down at 1"});
    const auto bad = scripts.add(2, "bad", "local B = {}\nfunction B:fixed_update(dt) maya.input.down('Hyper') end\nreturn B");
    auto second = play(scripts, {entity(1, {script(bad)})});
    second.run();
    CHECK(second.said("unknown key 'Hyper'"));
}

TEST_CASE("Random numbers and created entities repeat from the session's seed", "[scripting]") {
    auto scripts = Scripts{};
    const auto dice = scripts.add(1, "dice", R"(
local D = {}
function D:start()
    maya.log(tostring(math.random(1, 1000000)) .. " " .. maya.create("Die", vector.zero):id())
end
return D
)");
    const auto roll = [&](uint64_t seed) {
        auto settings = ScriptSettings{};
        settings.seed = seed;
        auto session = play(scripts, {entity(1, {script(dice)})}, settings);
        session.run();
        return session.texts(SimulationMessage::Level::info).at(0);
    };
    CHECK(roll(7) == roll(7));
    CHECK(roll(7) != roll(8));
}

TEST_CASE("A spin script matches the built-in spin component for 600 ticks", "[scripting]") {
    auto scripts = Scripts{};
    const auto spin = scripts.add(1, "spin", R"(
local Spin = {}
Spin.properties = {
    axis = { type = "vector", default = vector.create(0, 1, 0) },
    speed = { type = "number", default = math.pi / 4, unit = "rad/s" },
}
function Spin:fixed_update(dt)
    if vector.magnitude(self.axis) <= 1e-6 or self.speed == 0 then return end
    local turn = maya.quaternion.from_axis_angle(self.axis, self.speed * dt)
    self.entity:set_rotation((self.entity:rotation() * turn):normalized())
end
return Spin
)");
    const auto axis = math::Vec3{0.3f, 1.0f, -0.2f};
    auto document = std::vector<SceneEntity>{
        entity(1, {TransformComponent{}, SpinComponent{axis, 1.3f}}),
        entity(2, {TransformComponent{}, script(spin, {{"axis", ScriptValueType::vector, axis}, {"speed", ScriptValueType::number, 1.3f}})})};
    auto session = play(scripts, std::move(document), {}, builtin_systems());
    session.run(600);
    const auto built_in = transform_of(session.world(), 1).rotation;
    const auto scripted = transform_of(session.world(), 2).rotation;
    CHECK(scripted.x == Approx(built_in.x).margin(1e-4));
    CHECK(scripted.y == Approx(built_in.y).margin(1e-4));
    CHECK(scripted.z == Approx(built_in.z).margin(1e-4));
    CHECK(scripted.w == Approx(built_in.w).margin(1e-4));
    CHECK(session.messages.empty());
}

TEST_CASE("Entity handles held by scripts are checked on every use", "[scripting]") {
    auto scripts = Scripts{};
    const auto keeper = scripts.add(1, "keeper", R"(
local K = {}
K.properties = { target = { type = "entity" } }
function K:fixed_update(dt)
    if maya.tick() == 0 then self.target:destroy()
    elseif maya.tick() == 1 then
        maya.log("alive: " .. tostring(self.target:alive()))
        self.target:set_position(vector.one)
    end
end
return K
)");
    auto session = play(scripts, {entity(1, {script(keeper, {{"target", ScriptValueType::entity, EntityId{0x70, 2}}})}),
                                  entity(2, {TransformComponent{}})});
    session.run(2);
    CHECK(session.said("alive: false"));
    CHECK(session.said("entity 70 2 does not exist (yet, or any more)"));
}

TEST_CASE("Script cost at 1,000 and 10,000 scripted entities", "[.][scripting][cost]") {
    auto scripts = Scripts{};
    const auto mover = scripts.add(1, "mover", R"(
local M = {}
M.properties = { speed = { type = "number", default = 1 } }
function M:fixed_update(dt)
    self.entity:set_position(self.entity:position() + vector.create(0, self.speed * dt, 0))
end
return M
)");
    const auto idle = scripts.add(2, "idle", "local I = {}\nfunction I:fixed_update(dt) end\nreturn I");
    for (const auto which : {idle, mover})
        for (const auto count : {1000, 10000}) {
            auto entities = std::vector<SceneEntity>{};
            for (int i = 0; i < count; ++i) entities.push_back(entity(10 + i, {TransformComponent{}, script(which)}));
            auto session = play(scripts, std::move(entities));
            session.run(2);
            auto total = 0.0, worst = 0.0;
            for (int i = 0; i < 120; ++i) {
                const auto before = std::chrono::steady_clock::now();
                session.run();
                const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
                total += ms;
                worst = std::max(worst, ms);
            }
            std::printf("%s x %5d: tick mean %.3f ms (%.0f ns per instance), max %.3f ms\n", which == idle ? "empty fixed_update" : "move by set_position",
                        count, total / 120.0, total / 120.0 * 1e6 / count, worst);
        }
}
