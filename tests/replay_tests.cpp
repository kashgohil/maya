#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/recording.hpp"
#include "maya/simulation/scripting.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <map>
#include <sstream>

// Play recordings and replay (#1023, docs/play.md#recording-and-replay).
using namespace maya;

namespace {
constexpr auto frame = 1.0 / 60.0;

SceneEntity entity(uint64_t low, std::vector<ComponentValue> components) {
    return SceneEntity{EntityId{0x72, low}, {}, std::move(components)};
}
TransformComponent placed(math::Vec3 at) {
    auto transform = TransformComponent{};
    transform.translation = at;
    return transform;
}
ColliderComponent box(math::Vec3 half = math::Vec3(0.5f), bool sensor = false) {
    auto collider = ColliderComponent{};
    collider.half_extents = half;
    collider.sensor = sensor;
    return collider;
}
ColliderComponent ball() {
    auto collider = ColliderComponent{};
    collider.shape = ColliderShape::sphere;
    return collider;
}
ScriptComponent script(uint64_t low) { return ScriptComponent{AssetRef<ScriptAsset>{AssetId{0x72, low}}, {}}; }

const std::map<AssetId, ScriptSource> sources = {
    // Counts what enters the zone; no pairs over table, function, or userdata keys.
    {AssetId{0x72, 1}, {"zone.luau", R"(
local Z = {}
function Z:start() self.entered = 0 end
function Z:on_trigger_enter(other) self.entered += 1; maya.log("entered " .. self.entered .. " by " .. other:name()) end
return Z
)"}},
    // Space kicks the ball up and along; it logs its landings.
    {AssetId{0x72, 2}, {"kicker.luau", R"(
local K = {}
function K:fixed_update(dt)
    if maya.input.pressed("Space") then self.entity:add_impulse(vector.create(300, 3000, 0)) end
end
function K:on_contact_begin(other, contact)
    maya.log(string.format("hit %s at %.3f", other:name(), contact.speed))
end
return K
)"}},
};

/// Stacked boxes on a floor, a trigger zone above them, and a ball kicked by input.
SceneDocument scene() {
    auto document = SceneDocument{};
    document.entities = {entity(1, {NameComponent{"Floor"}, placed({0, -0.5f, 0}), box({20, 0.5f, 20})}),
                         entity(2, {NameComponent{"Zone"}, placed({0, 4, 0}), box({3, 0.5f, 3}, true), script(1)}),
                         entity(3, {NameComponent{"Ball"}, placed({-3, 0.5f, 0}), ball(), RigidBodyComponent{}, script(2)})};
    for (uint64_t i = 0; i < 5; ++i)
        document.entities.push_back(entity(10 + i, {NameComponent{"Box"}, placed({0, 0.5f + float(i), 0}), box(), RigidBodyComponent{}}));
    return document;
}

std::unique_ptr<PlaySession> start(const SceneDocument& document) {
    auto systems = play_systems([](AssetId id) -> ScriptSourceResult {
        const auto found = sources.find(id);
        if (found == sources.end()) return {std::nullopt, "not in the test's sources"};
        return {found->second, {}};
    }, {});
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    auto started = PlaySession::start(document, any_asset, std::move(systems));
    INFO(started.error);
    REQUIRE(started);
    return std::move(started.session);
}

std::vector<InputEvent> space(bool down) { return {KeyEvent{KeyCode::Space, down, KeyModifiers::none}}; }

/// Plays 1,800 ticks with uneven frame times, kicking the ball now and then, and records them.
PlayRecording record() {
    auto session = start(scene());
    session->start_recording();
    const double pattern[] = {0.3, 1.7, 0.9, 1.4, 0.5, 1.2, 1.0};
    auto frames = 0;
    while (session->clock().tick() < 1800) {
        if (frames % 150 == 10) session->input().feed(space(true));
        if (frames % 150 == 14) session->input().feed(space(false));
        const auto remaining = 1800 - session->clock().tick();
        const auto delta = std::min(pattern[frames % 7], double(remaining)) * frame;
        REQUIRE(session->update(delta).error.empty());
        ++frames;
    }
    auto recording = PlayRecording{};
    recording.build = recording_build();
    recording.physics = recording_physics({});
    recording.seed = ScriptSettings{}.seed;
    recording.inputs = session->recorded_inputs();
    recording.checkpoints = session->recorded_checkpoints();
    recording.final_state = session->full_state_hash();
    recording.scene = "(the test's scene)";
    return recording;
}

/// Replays a recording at a steady frame rate and returns the session.
std::unique_ptr<PlaySession> replay(const PlayRecording& recording) {
    auto session = start(scene());
    session->start_replay(recording.inputs, recording.checkpoints);
    for (int i = 0; i < 4000 && !session->replay().finished; ++i) REQUIRE(session->update(frame).error.empty());
    return session;
}
} // namespace

TEST_CASE("A recording round-trips through its file, and malformed files are refused", "[replay]") {
    auto recording = PlayRecording{};
    recording.build = "abc123 Release sanitizers none AppleClang 17";
    recording.physics = "Jolt 5.6.0, single precision; 1 collision step";
    recording.seed = 0x6d617961;
    recording.scene_name = "levels/one \"quoted\".scene";
    recording.scene = "maya-scene 1\n\nentity 72 1\nend\n";
    recording.assets = {{AssetId{0x72, 1}, "script", "scripts/zone.luau", content_hash("return {}")}};
    recording.reloads = {{120, AssetId{0x72, 1}, "scripts/zone.luau"}};
    auto held = InputFrame{};
    held.held.set(size_t(KeyCode::W));
    held.look = {0.1f, -2.5f};
    auto pressed = held;
    pressed.pressed.set(size_t(KeyCode::Space));
    pressed.buttons_pressed.set(0);
    pressed.scroll = 1.0f / 3.0f;
    for (int i = 0; i < 5; ++i) recording.inputs.push({});
    for (int i = 0; i < 40; ++i) recording.inputs.push(held);
    recording.inputs.push(pressed);
    for (int i = 0; i < 14; ++i) recording.inputs.push(held);
    recording.checkpoints = {{60, 0x0123456789abcdefull}};
    recording.final_state = 0xfedcba9876543210ull;
    CHECK(recording.inputs.size() == 60);
    CHECK(recording.inputs.changes().size() == 4); // stored where it changes

    auto text = std::stringstream{};
    write_recording(text, recording);
    const auto read = read_recording(text);
    INFO(read.error);
    REQUIRE(read);
    const auto& back = *read.recording;
    CHECK(back.build == recording.build);
    CHECK(back.physics == recording.physics);
    CHECK(back.seed == recording.seed);
    CHECK(back.scene_name == recording.scene_name);
    CHECK(back.scene == recording.scene);
    CHECK(back.assets == recording.assets);
    CHECK(back.reloads == recording.reloads);
    CHECK(back.checkpoints == recording.checkpoints);
    CHECK(back.final_state == recording.final_state);
    REQUIRE(back.inputs.size() == 60);
    for (uint64_t tick = 0; tick < 60; ++tick) CHECK(same_input(back.inputs.at(tick), recording.inputs.at(tick)));

    const auto refused = [](const std::string& file) {
        auto in = std::istringstream(file);
        return read_recording(in).error;
    };
    CHECK(refused("") == "The file is empty");
    CHECK(refused("maya-scene 1\n").starts_with("Not a Maya play recording"));
    CHECK(refused("maya-recording 2\n") == "Recording format 2; this build reads format 1");
    auto whole = std::stringstream{};
    write_recording(whole, recording);
    const auto full = whole.str();
    CHECK(refused(full.substr(0, full.size() - 12)).find("cut short") != std::string::npos);
    CHECK(refused("maya-recording 1\nbuild \"b\"\nphysics \"p\"\nticks 1\nfinal 0\nwhat 1\n") == "line 6: unexpected \"what\"");
}

TEST_CASE("A replay repeats a recorded session exactly, with any worker count", "[replay]") {
    const auto recording = record();
    CHECK(recording.inputs.size() == 1800);
    CHECK(recording.checkpoints.size() == 30);
    CHECK(recording.inputs.changes().size() < 100); // a few presses in 1,800 ticks

    auto file = std::stringstream{};
    write_recording(file, recording);
    const auto read = read_recording(file);
    REQUIRE(read);
    for (const auto workers : {0, 4}) {
        INFO(workers << " workers");
        set_physics_worker_threads(workers);
        const auto session = replay(*read.recording);
        CHECK(session->replay().finished);
        CHECK(session->replay().checked == 30);
        CHECK_FALSE(session->replay().first_difference);
        CHECK(session->clock().tick() == 1800);
        CHECK(session->clock().paused());
        CHECK(session->full_state_hash() == recording.final_state);
    }
    set_physics_worker_threads(-1);

    // Other input: the replay says where it first differs.
    auto kick = std::optional<uint64_t>{}; // the first tick Space went down
    for (uint64_t tick = 0; tick < recording.inputs.size() && !kick; ++tick)
        if (recording.inputs.at(tick).went_down(KeyCode::Space)) kick = tick;
    REQUIRE(kick);
    auto changed = PlayRecording{};
    changed.checkpoints = recording.checkpoints;
    for (uint64_t tick = 0; tick < recording.inputs.size(); ++tick) {
        auto input = recording.inputs.at(tick);
        if (tick == *kick) input.pressed.reset(); // that kick never happens
        changed.inputs.push(input);
    }
    const auto different = replay(changed);
    REQUIRE(different->replay().first_difference);
    CHECK(*different->replay().first_difference == (*kick / 60 + 1) * 60); // the first checkpoint after it
}

TEST_CASE("Recordings from another build or physics, with changed assets, or with a reload are refused", "[replay]") {
    auto recording = PlayRecording{};
    recording.build = recording_build();
    recording.physics = recording_physics({});
    const auto zone = RecordedAsset{AssetId{0x72, 1}, "script", "scripts/zone.luau", content_hash("zone v1")};
    recording.assets = {zone};
    CHECK(replay_refusal(recording, {zone}, {}).empty());

    auto other = recording;
    other.build = "0123456789ab Release sanitizers none AppleClang 16";
    CHECK(replay_refusal(other, {zone}, {}).starts_with("It was recorded by build 0123456789ab"));
    other = recording;
    other.physics = "Jolt 5.5.0, single precision; 1 collision step";
    CHECK(replay_refusal(other, {zone}, {}).starts_with("It was recorded with Jolt 5.5.0"));
    auto two_steps = PhysicsSettings{};
    two_steps.collision_steps = 2;
    CHECK(replay_refusal(recording, {zone}, two_steps).find("2 collision steps") != std::string::npos);
    auto edited = zone;
    edited.hash = content_hash("zone v2");
    CHECK(replay_refusal(recording, {edited}, {}) == "scripts/zone.luau has changed since the recording");
    CHECK(replay_refusal(recording, {}, {}) == "scripts/zone.luau is no longer in the project");
    other = recording;
    other.reloads = {{240, zone.id, "scripts/zone.luau"}};
    CHECK(replay_refusal(other, {zone}, {}) ==
          "scripts/zone.luau was reloaded at tick 240 of the recorded session, so its replay is not promised to match");
}

TEST_CASE("A recording made before double positions still reads, its scene migrates, and its replay is refused for the physics", "[replay][precision]") {
    // #1065: a recording from a single-precision build reads unchanged; its scene (maya.transform 1) loads
    // as the floats it held; and its replay is refused with the reason, since double-precision physics
    // gives other poses than the session it recorded.
    auto old = PlayRecording{};
    old.build = "0123456789ab Release sanitizers none AppleClang 17";
    old.physics = "Jolt 5.6.0, single precision; 1 collision step";
    old.scene_name = "basic.scene";
    old.scene = "maya-scene 1\n\nentity 72 1\n  component maya.transform 1\n    translation 0.1 4096.37 -2\n"
                "    rotation 0 0 0 1\n    scale 1 1 1\nend\n";
    for (int i = 0; i < 60; ++i) old.inputs.push({});
    old.checkpoints = {{60, 0x0123456789abcdefull}};
    auto file = std::ostringstream{};
    write_recording(file, old);
    auto input = std::istringstream(file.str());
    const auto read = read_recording(input);
    INFO(read.error);
    REQUIRE(read);
    CHECK(read.recording->scene == old.scene);
    CHECK(read.recording->checkpoints == old.checkpoints);
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    const auto scene = read_scene(read.recording->scene, any_asset);
    REQUIRE(scene);
    const auto& transform = std::get<TransformComponent>(scene.document.entities.front().components.front());
    CHECK(transform.translation == math::DVec3{double(0.1f), double(4096.37f), -2.0});
    old.build = recording_build(); // even from this build, the physics differs
    const auto refusal = replay_refusal(old, {}, {});
    CHECK(refusal.starts_with("It was recorded with Jolt 5.6.0, single precision"));
    CHECK(refusal.find("double precision") != std::string::npos);
}

TEST_CASE("Recording cost at 1,000, 10,000, and 50,000 entities", "[.][replay][cost]") {
    for (const auto count : {1000, 10000, 50000}) {
        auto document = SceneDocument{};
        for (int i = 0; i < count; ++i) {
            auto components = std::vector<ComponentValue>{placed({float(i % 100) * 2, float(i / 1000) * 2, float(i / 100 % 10) * 2})};
            if (i % 10 == 0) { // every tenth a body
                components.push_back(box());
                components.push_back(RigidBodyComponent{});
            }
            document.entities.push_back(entity(uint64_t(100 + i), std::move(components)));
        }
        const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
        const auto time = [](auto&& work) {
            const auto start = std::chrono::steady_clock::now();
            work();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        };
        auto text = std::ostringstream{};
        const auto scene_ms = time([&] { write_scene(text, document, any_asset); });
        auto session = start(document);
        session->start_recording();
        REQUIRE(session->update(frame).error.empty());
        auto hash = uint64_t{};
        auto hash_ms = 0.0;
        constexpr int rounds = 10;
        for (int i = 0; i < rounds; ++i) hash_ms += time([&] { hash ^= session->state_hash(); });
        const auto full_ms = time([&] { hash ^= session->full_state_hash(); });
        std::printf("%6d entities (%d bodies): scene text at start %.2f ms, state hash %.3f ms (every 60 ticks), full state at Stop %.2f ms%s\n",
                    count, count / 10, scene_ms, hash_ms / rounds, full_ms, hash ? "" : " ");
    }
}
