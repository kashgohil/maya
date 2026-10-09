// Skeletal animation (#1038, docs/animation.md): glTF's samplers, clip time, cooked skins and clips, name
// paths, and the animation system in play: when changes show, how presentation follows and jumps, and what
// is reported when a clip cannot move what it names.

#include "maya/assets/animation.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/scripting.hpp"
#include "maya/world/components.hpp"
#include "maya/world/name_path.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <map>

using namespace maya;
using Catch::Approx;

namespace {

AnimationChannel channel(std::string target, ChannelPath path, Interpolation interpolation, std::vector<float> times,
                         std::vector<float> values) {
    return {std::move(target), path, interpolation, std::move(times), std::move(values)};
}
math::Quat about_y(float angle) { return math::Quat::from_axis_angle({0, 1, 0}, angle); }
/// The angle of a rotation, from its quaternion.
float angle_of(const math::Quat& q) {
    return 2.0f * std::atan2(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z), std::abs(q.w));
}

// --- Samplers --------------------------------------------------------------------------------------

TEST_CASE("Step holds each key until the next, and every sampler holds its first and last keys", "[animation]") {
    const auto step = channel("A", ChannelPath::translation, Interpolation::step, {0, 1, 2}, {10, 0, 0, 20, 0, 0, 30, 0, 0});
    REQUIRE(check_channel(step).empty());
    CHECK(sample_channel(step, -1.0f).x == 10.0f);
    CHECK(sample_channel(step, 0.0f).x == 10.0f);
    CHECK(sample_channel(step, 0.999f).x == 10.0f);
    CHECK(sample_channel(step, 1.0f).x == 20.0f);
    CHECK(sample_channel(step, 1.5f).x == 20.0f);
    CHECK(sample_channel(step, 2.0f).x == 30.0f);
    CHECK(sample_channel(step, 5.0f).x == 30.0f);
    const auto single = channel("A", ChannelPath::scale, Interpolation::linear, {0.5f}, {2, 3, 4});
    REQUIRE(check_channel(single).empty());
    CHECK(sample_channel(single, 0.0f).y == 3.0f);
    CHECK(sample_channel(single, 9.0f).z == 4.0f);
}

TEST_CASE("Linear interpolates translations and scales, and slerps rotations along the shorter arc", "[animation]") {
    const auto moving = channel("A", ChannelPath::translation, Interpolation::linear, {1, 3}, {0, 0, 0, 4, -2, 8});
    const auto at = sample_channel(moving, 1.5f);
    CHECK(at.x == Approx(1.0f));
    CHECK(at.y == Approx(-0.5f));
    CHECK(at.z == Approx(2.0f));
    // The second key is 90 degrees about +Y written as its negation: the same rotation the other way round
    // the sphere, which slerp must not take.
    const auto q0 = math::Quat{}, q1 = about_y(math::PI / 2);
    const auto turning = channel("A", ChannelPath::rotation, Interpolation::linear, {0, 1},
                                 {q0.x, q0.y, q0.z, q0.w, -q1.x, -q1.y, -q1.z, -q1.w});
    for (const auto t : {0.25f, 0.5f, 0.75f}) {
        const auto v = sample_channel(turning, t);
        const auto q = math::Quat{v.x, v.y, v.z, v.w};
        CHECK(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) == Approx(1.0f));
        CHECK(angle_of(q) == Approx(math::PI / 2 * t).margin(1e-5));
    }
}

TEST_CASE("Cubic splines follow glTF's Hermite curve, with tangents scaled by the keys' spacing", "[animation]") {
    // Keys at 1 s and 3 s, values 0 and 1, out-tangent 2 and in-tangent -1, each stored as in-tangent, value,
    // out-tangent. At the middle (s = 0.5, a 2 s span): 0.5 + 0.5 + 0.25.
    const auto curve = channel("A", ChannelPath::translation, Interpolation::cubic_spline, {1, 3},
                               {0, 0, 0, 0, 0, 0, 2, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0});
    REQUIRE(check_channel(curve).empty());
    CHECK(sample_channel(curve, 2.0f).x == Approx(1.25f));
    CHECK(sample_channel(curve, 1.0f).x == 0.0f);
    CHECK(sample_channel(curve, 3.0f).x == 1.0f);
    CHECK(sample_channel(curve, 0.0f).x == 0.0f); // the first key's value, not its tangent
    CHECK(sample_channel(curve, 4.0f).x == 1.0f);
    // A cubic rotation is normalized.
    const auto spin = channel("A", ChannelPath::rotation, Interpolation::cubic_spline, {0, 1},
                              {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.7071f, 0, 0.7071f, 0, 0, 0, 0});
    const auto v = sample_channel(spin, 0.5f);
    CHECK(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w) == Approx(1.0f));
}

TEST_CASE("Channels with keys out of order, the wrong number of values, or values that are not finite are refused", "[animation]") {
    CHECK_FALSE(check_channel(channel("A", ChannelPath::translation, Interpolation::linear, {0, 2, 1}, std::vector<float>(9))).empty());
    CHECK_FALSE(check_channel(channel("A", ChannelPath::translation, Interpolation::linear, {0, 0}, std::vector<float>(6))).empty());
    CHECK_FALSE(check_channel(channel("A", ChannelPath::rotation, Interpolation::linear, {0, 1}, std::vector<float>(6))).empty());
    CHECK_FALSE(check_channel(channel("A", ChannelPath::scale, Interpolation::cubic_spline, {0, 1}, std::vector<float>(6))).empty());
    CHECK_FALSE(check_channel(channel("A", ChannelPath::scale, Interpolation::linear, {}, {})).empty());
    CHECK_FALSE(check_channel(channel("A", ChannelPath::scale, Interpolation::linear, {0, NAN}, std::vector<float>(6))).empty());
    CHECK_FALSE(check_channel(channel("A", ChannelPath::scale, Interpolation::linear, {0, 1}, {1, 1, 1, INFINITY, 1, 1})).empty());
}

TEST_CASE("Clip time wraps into the clip when looping and holds at its ends otherwise", "[animation]") {
    CHECK(clip_time(2.0f, 0.5f, true) == 0.5f);
    CHECK(clip_time(2.0f, 2.5f, true) == Approx(0.5f));
    CHECK(clip_time(2.0f, 2.0f, true) == 0.0f);
    CHECK(clip_time(2.0f, -0.5f, true) == Approx(1.5f));
    CHECK(clip_time(2.0f, 2.5f, false) == 2.0f);
    CHECK(clip_time(2.0f, -0.5f, false) == 0.0f);
    CHECK(clip_time(0.0f, 3.0f, true) == 0.0f);
}

TEST_CASE("Cooked skins and clips read back as written, and damaged ones are refused", "[animation]") {
    auto skin = SkinAsset{{"Armature/Hips", "Armature/Hips/Spine"}, {math::Mat4::identity(), math::Mat4::translate({0, -1, 0})}};
    const auto skin_bytes = write_skin(skin);
    const auto read_back = read_skin(skin_bytes);
    REQUIRE(read_back);
    CHECK(read_back->joints == skin.joints);
    CHECK(read_back->inverse_bind[1].at(1, 3) == -1.0f);
    auto clip = AnimationAsset{"Walk", 1.5f, {channel("Armature/Hips", ChannelPath::rotation, Interpolation::cubic_spline, {0, 1.5f},
                                                      std::vector<float>(24, 0.5f))}};
    const auto clip_bytes = write_animation(clip);
    const auto clip_back = read_animation(clip_bytes);
    REQUIRE(clip_back);
    CHECK(clip_back->name == "Walk");
    CHECK(clip_back->duration == 1.5f);
    REQUIRE(clip_back->channels.size() == 1);
    CHECK(clip_back->channels[0].interpolation == Interpolation::cubic_spline);
    CHECK(clip_back->channels[0].values == clip.channels[0].values);
    for (size_t cut = 0; cut < skin_bytes.size(); cut += 7) CHECK_FALSE(read_skin(std::span(skin_bytes).first(cut)));
    for (size_t cut = 0; cut < clip_bytes.size(); cut += 7) CHECK_FALSE(read_animation(std::span(clip_bytes).first(cut)));
}

// --- Name paths ------------------------------------------------------------------------------------

TEST_CASE("Name paths escape slashes and backslashes in names and refuse empty names", "[animation]") {
    CHECK(append_name_path("", "Hips") == "Hips");
    CHECK(append_name_path("Armature", "L/R") == "Armature/L\\/R");
    CHECK(append_name_path("A", "back\\slash") == "A/back\\\\slash");
    CHECK(split_name_path("Armature/L\\/R/Hand") == std::vector<std::string>{"Armature", "L/R", "Hand"});
    CHECK(split_name_path("A/back\\\\slash") == std::vector<std::string>{"A", "back\\slash"});
    CHECK_FALSE(split_name_path(""));
    CHECK_FALSE(split_name_path("A//B"));
    CHECK_FALSE(split_name_path("A/"));
    CHECK_FALSE(split_name_path("A\\"));
}

TEST_CASE("Name paths resolve below their root, the first of same-named siblings, and nowhere else", "[animation]") {
    World world;
    auto commands = world.commands();
    const auto entity = [&](const char* name, std::optional<PendingEntity> parent) {
        const auto created = commands.create();
        commands.add(created, NameComponent{name});
        commands.add(created, TransformComponent{});
        if (parent) commands.reparent(created, *parent, ReparentPolicy::keep_local);
        return created;
    };
    const auto root = entity("Root", std::nullopt), twin = entity("Arm", root), twin_hand = entity("Hand", twin),
               arm = entity("Arm", root), hand = entity("Hand", arm), slash = entity("L/R", root), outside = entity("Arm", std::nullopt);
    const auto result = world.commit(commands);
    REQUIRE(result);
    const auto handle = [&](PendingEntity pending) { return result.created[pending.index]; };
    (void)twin_hand;
    (void)outside;
    // Reparenting puts a child first among its siblings, so `arm`, added after its twin, is first.
    REQUIRE(world.children(handle(root)).at(1) == handle(arm));
    const auto found = resolve_name_paths(world, handle(root), std::vector<std::string>{"Arm/Hand", "Arm", "L\\/R", "Leg", "Arm/Foot", "Hand", ""});
    CHECK(found[0] == handle(hand));
    CHECK(found[1] == handle(arm));
    CHECK(found[2] == handle(slash));
    CHECK_FALSE(found[3]);
    CHECK_FALSE(found[4]);
    CHECK_FALSE(found[5]); // a grandchild is not a child
    CHECK_FALSE(found[6]);
}

// --- The animation system --------------------------------------------------------------------------

constexpr AssetId turn_id{0x616e, 1}, lift_id{0x616e, 2}, shrink_id{0x616e, 3}, flip_id{0x616e, 4}, missing_id{0x616e, 9};
constexpr float dt = 1.0f / 60.0f;

/// "turn" (1 s): Arm turns from none to 90 degrees about +Y; Arm/Hand moves from the origin to (0, 0, 2).
/// "lift" (0.5 s): Arm steps from (0, 1, 0) to (0, 2, 0) at 0.25 s. "shrink" (1 s): Hand's scale falls to 0.
/// "flip" (1 s): Hand's X scale falls to -1.
std::map<AssetId, AnimationAsset> test_clips() {
    const auto q = about_y(math::PI / 2);
    auto clips = std::map<AssetId, AnimationAsset>{};
    clips[turn_id] = {"turn", 1.0f, {channel("Arm", ChannelPath::rotation, Interpolation::linear, {0, 1}, {0, 0, 0, 1, q.x, q.y, q.z, q.w}),
                                     channel("Arm/Hand", ChannelPath::translation, Interpolation::linear, {0, 1}, {0, 0, 0, 0, 0, 2})}};
    clips[lift_id] = {"lift", 0.5f, {channel("Arm", ChannelPath::translation, Interpolation::step, {0, 0.25f, 0.5f},
                                             {0, 1, 0, 0, 2, 0, 0, 2, 0})}};
    clips[shrink_id] = {"shrink", 1.0f, {channel("Arm/Hand", ChannelPath::scale, Interpolation::linear, {0, 1}, {1, 1, 1, 0, 0, 0})}};
    clips[flip_id] = {"flip", 1.0f, {channel("Arm/Hand", ChannelPath::scale, Interpolation::linear, {0, 1}, {1, 1, 1, -1, 1, 1})}};
    return clips;
}
AnimationClips inline_clips() {
    return [clips = test_clips()](AssetId id) -> AnimationClipResult {
        const auto found = clips.find(id);
        if (found == clips.end()) return {nullptr, "not one of the test's clips"};
        return {std::make_shared<const AnimationAsset>(found->second), {}};
    };
}

constexpr EntityId root_id{0xa0, 1}, arm_id{0xa0, 2}, hand_id{0xa0, 3}, prop_id{0xa0, 4};
const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};

/// Root (animated) > Arm > Hand > Prop: a prop held in the hand, as joints carry child entities.
SceneDocument rig(AnimationComponent animation, std::optional<ScriptComponent> script = std::nullopt) {
    auto document = SceneDocument{};
    auto root = std::vector<ComponentValue>{NameComponent{"Root"}, TransformComponent{{5, 0, 0}, {}, {1, 1, 1}}, animation};
    if (script) root.push_back(*script);
    document.entities.push_back({root_id, std::nullopt, std::move(root)});
    document.entities.push_back({arm_id, root_id, {NameComponent{"Arm"}, TransformComponent{{0, 1, 0}, {}, {1, 1, 1}}}});
    document.entities.push_back({hand_id, arm_id, {NameComponent{"Hand"}, TransformComponent{}}});
    document.entities.push_back({prop_id, hand_id, {NameComponent{"Sword"}, TransformComponent{{0, 0.5f, 0}, {}, {1, 1, 1}}}});
    return document;
}

/// A native system for tests, run before the animation system: a callback each tick.
class Native final : public SimulationSystem {
public:
    explicit Native(std::function<void(TickContext&)> tick) : m_tick(std::move(tick)) {}
    std::string_view name() const override { return "Native"; }
    void fixed_update(TickContext& tick) override { m_tick(tick); }

private:
    std::function<void(TickContext&)> m_tick;
};

struct Session {
    std::unique_ptr<PlaySession> play;
    std::vector<SimulationMessage> messages;
    void run(int ticks = 1, double frame = 1.0 / 60.0) {
        for (int i = 0; i < ticks; ++i) {
            auto result = play->update(frame);
            INFO(play->error());
            REQUIRE(result.error.empty());
            for (auto& message : result.messages) messages.push_back(std::move(message));
        }
    }
    TransformComponent local(EntityId id) const {
        auto result = TransformComponent{};
        REQUIRE(play->world().with<TransformComponent>(*play->world().find(id), [&](const TransformComponent& value) { result = value; }));
        return result;
    }
    math::Mat4 world_matrix(EntityId id) const { return play->world().world_matrix(*play->world().find(id))->matrix(); }
    size_t said(const std::string& part) const {
        return size_t(std::ranges::count_if(messages, [&](const SimulationMessage& m) { return m.text.find(part) != std::string::npos; }));
    }
};

Session start(SceneDocument document, std::vector<std::unique_ptr<SimulationSystem>> before = {},
              std::map<AssetId, ScriptSource> scripts = {}) {
    auto systems = std::move(before);
    auto settings = ScriptSettings{};
    settings.assets = any_asset;
    for (auto& system : play_systems([scripts](AssetId id) -> ScriptSourceResult {
             const auto found = scripts.find(id);
             if (found == scripts.end()) return {std::nullopt, "not in the test's sources"};
             return {found->second, {}};
         }, inline_clips(), settings))
        systems.push_back(std::move(system));
    auto started = PlaySession::start(std::move(document), any_asset, std::move(systems));
    INFO(started.error);
    REQUIRE(started);
    return {std::move(started.session), {}};
}

TEST_CASE("A clip poses what it names from the first tick, then advances by each tick times its speed", "[animation]") {
    auto session = start(rig({{turn_id}, true, false, 1.0f, 0.0f}));
    session.run(); // tick 0 samples the start
    CHECK(angle_of(session.local(arm_id).rotation) == Approx(0.0f).margin(1e-6));
    session.run(30); // tick 30 samples 0.5 s
    CHECK(angle_of(session.local(arm_id).rotation) == Approx(math::PI / 4).margin(1e-5));
    CHECK(session.local(hand_id).translation.z == Approx(1.0f));
    // What it does not name keeps its authored pose; the joint's own translation, which it does not move, too.
    CHECK(session.local(arm_id).translation.y == 1.0f);
    CHECK(session.local(prop_id).translation.y == 0.5f);
    // The prop follows the hand: its world position is the hand's, half a metre up the hand's parent's turned frame.
    const auto prop = session.world_matrix(prop_id), hand = session.world_matrix(hand_id);
    CHECK(prop.at(1, 3) == Approx(hand.at(1, 3) + 0.5f));
    // Once, it holds its last pose at the end.
    session.run(60);
    CHECK(angle_of(session.local(arm_id).rotation) == Approx(math::PI / 2).margin(1e-5));
    CHECK(session.local(hand_id).translation.z == Approx(2.0f));
    CHECK(session.messages.empty());
}

TEST_CASE("Looping wraps, a negative speed plays backwards, the start time offsets, and a paused clip holds", "[animation]") {
    SECTION("looping") {
        auto session = start(rig({{turn_id}, true, true, 1.0f, 0.0f}));
        session.run(76); // tick 75: 1.25 s, a quarter into the second loop
        CHECK(session.local(hand_id).translation.z == Approx(0.5f).margin(1e-4));
    }
    SECTION("backwards from the start time") {
        auto session = start(rig({{turn_id}, true, false, -2.0f, 0.75f}));
        session.run(7); // tick 6: 0.75 - 6 x 2/60 = 0.55 s
        CHECK(session.local(hand_id).translation.z == Approx(1.1f).margin(1e-4));
        session.run(60);
        CHECK(session.local(hand_id).translation.z == 0.0f); // held at the start
    }
    SECTION("paused") {
        auto session = start(rig({{turn_id}, false, true, 1.0f, 0.25f}));
        session.run(20);
        CHECK(session.local(hand_id).translation.z == Approx(0.5f));
    }
    SECTION("a step clip") {
        auto session = start(rig({{lift_id}, true, false, 1.0f, 0.0f}));
        session.run(15); // tick 14: 0.233 s
        CHECK(session.local(arm_id).translation.y == 1.0f);
        session.run(1); // tick 15: 0.25 s
        CHECK(session.local(arm_id).translation.y == 2.0f);
    }
}

TEST_CASE("A script's change to the clip shows on the next tick, from the start, and its joints jump there", "[animation]") {
    constexpr AssetId switcher{0x5c, 1};
    const auto scripts = std::map<AssetId, ScriptSource>{{switcher, {"switcher.luau", R"(
local S = {}
function S:fixed_update(dt)
    if maya.tick() == 10 then
        self.entity:set("maya.animation", "clip", "616e 2")
        self.entity:set("maya.animation", "speed", 2)
    end
end
return S
)"}}};
    auto session = start(rig({{turn_id}, true, false, 1.0f, 0.0f}, ScriptComponent{{switcher}, {}}), {}, scripts);
    session.run(11); // ticks 0 to 10: the script's change is made in tick 10, which still plays the turn
    CHECK(angle_of(session.local(arm_id).rotation) == Approx(math::PI / 2 * 10 * dt).margin(1e-5));
    CHECK(session.local(arm_id).translation.y == 1.0f);
    session.run(1); // tick 11: the lift, from its start
    CHECK(session.local(arm_id).translation.y == 1.0f);
    // What the new clip does not name keeps the pose the old one left.
    CHECK(angle_of(session.local(arm_id).rotation) == Approx(math::PI / 2 * 10 * dt).margin(1e-5));
    session.run(8); // tick 19: 8 ticks at twice the speed, 0.267 s
    CHECK(session.local(arm_id).translation.y == 2.0f);
    CHECK(session.messages.empty());
}

TEST_CASE("Scripts read a clip as an asset ID and refuse what is not one", "[animation]") {
    constexpr AssetId reader{0x5c, 2};
    const auto scripts = std::map<AssetId, ScriptSource>{{reader, {"reader.luau", R"(
local R = {}
function R:fixed_update(dt)
    if maya.tick() == 0 then
        maya.log(tostring(self.entity:get("maya.animation", "clip")))
        local ok, err = pcall(function() self.entity:set("maya.animation", "clip", "not an id") end)
        maya.log(tostring(ok))
        self.entity:set("maya.animation", "clip", nil)
    end
end
return R
)"}}};
    auto session = start(rig({{turn_id}, true, false, 1.0f, 0.0f}, ScriptComponent{{reader}, {}}), {}, scripts);
    session.run(3);
    CHECK(session.said("reader.luau): 616e 1") == 1);
    CHECK(session.said("reader.luau): false") == 1);
    auto clip = AssetRef<AnimationAsset>{};
    session.play->world().with<AnimationComponent>(*session.play->world().find(root_id), [&](const AnimationComponent& value) { clip = value.clip; });
    CHECK_FALSE(clip.valid());
}

TEST_CASE("Presentation between ticks is smooth at 120 Hz, and a clip change jumps rather than blends", "[animation]") {
    constexpr AssetId switcher{0x5c, 3};
    const auto scripts = std::map<AssetId, ScriptSource>{{switcher, {"switcher.luau", R"(
local S = {}
function S:fixed_update(dt)
    if maya.tick() == 40 then self.entity:set("maya.animation", "start", 0.9) end
end
return S
)"}}};
    auto session = start(rig({{turn_id}, true, false, 1.0f, 0.0f}, ScriptComponent{{switcher}, {}}), {}, scripts);
    // Frames of 1/120 s: a tick every second frame, and every frame shown halfway or at a tick.
    const auto shown_angle = [&] {
        const auto poses = session.play->presentation();
        const auto arm = *session.play->world().find(arm_id);
        const auto matrix = poses.world_matrix(session.play->world(), arm);
        REQUIRE(matrix);
        return std::atan2(-matrix->linear.at(2, 0), matrix->linear.at(0, 0)); // about +Y, from the turned X axis
    };
    // Tick 0's pose is the clip's first, which the joints jump to; tick 1 is the first shown between two.
    session.run(4, 1.0 / 120.0);
    auto previous = shown_angle();
    for (int frame = 0; frame < 72; ++frame) { // ticks 2 to 37
        session.run(1, 1.0 / 120.0);
        const auto angle = shown_angle();
        // Each frame shows half a tick more of the turn: 90 degrees a second, 0.75 degrees a frame.
        CHECK(angle - previous == Approx(math::PI / 2 / 120.0f).margin(2e-5));
        previous = angle;
    }
    // Tick 41 starts again at 0.9 s: the arm is shown there, never between where it was and there.
    session.run(9, 1.0 / 120.0); // ticks 38 to 41, then halfway to 42
    const auto poses = session.play->presentation();
    CHECK_FALSE(poses.find(*session.play->world().find(arm_id))); // reset: the World's own pose is shown
    CHECK_FALSE(poses.find(*session.play->world().find(prop_id))); // and the sword's, which the hand carries
    CHECK(shown_angle() == Approx(math::PI / 2 * 0.9f).margin(1e-5));
    session.run(2, 1.0 / 120.0); // tick 42, then halfway to 43: smooth again
    CHECK(session.play->presentation().find(*session.play->world().find(arm_id)));
}

TEST_CASE("A joint renamed during play unbinds, is reported once, and keeps its last pose", "[animation]") {
    auto before = std::vector<std::unique_ptr<SimulationSystem>>{};
    before.push_back(std::make_unique<Native>([](TickContext& tick) {
        if (tick.tick == 5) tick.commands.replace(*tick.world.find(hand_id), NameComponent{"Fist"});
    }));
    auto session = start(rig({{turn_id}, true, false, 1.0f, 0.0f}), std::move(before));
    session.run(6); // tick 5 renames; its pose is the last the clip gives the hand
    const auto held = session.local(hand_id).translation.z;
    CHECK(held == Approx(2.0f * 5 * dt));
    session.run(20);
    CHECK(session.local(hand_id).translation.z == held);
    CHECK(angle_of(session.local(arm_id).rotation) == Approx(math::PI / 2 * 25 * dt).margin(1e-5)); // the rest plays on
    CHECK(session.said("'Root' has no entity at 'Arm/Hand', which its clip 'turn' moves (renamed or removed?)") == 1);
    CHECK(session.messages.size() == 1);
    CHECK(session.messages[0].level == SimulationMessage::Level::warning);
    CHECK(session.messages[0].source == "Animation");
}

TEST_CASE("A zero scale hides what it scales, and a missing clip or a pose the World cannot hold is reported once", "[animation]") {
    SECTION("missing clip") {
        auto session = start(rig({{missing_id}, true, true, 1.0f, 0.0f}));
        session.run(10);
        CHECK(session.said("'Root' cannot play its clip: not one of the test's clips") == 1);
        CHECK(session.messages.size() == 1);
        CHECK(session.local(hand_id).translation.z == 0.0f);
    }
    SECTION("zero scale") {
        auto session = start(rig({{shrink_id}, true, false, 1.0f, 0.0f}));
        session.run(70);
        CHECK(session.messages.empty());
        CHECK(session.local(hand_id).scale.x == min_animated_scale);
        // The sword it holds shrinks with it, to nothing to see.
        const auto sword = session.world_matrix(prop_id);
        CHECK(math::Vec3(sword.at(0, 0), sword.at(1, 0), sword.at(2, 0)).length() == Approx(min_animated_scale));
    }
    SECTION("negative scale") {
        auto session = start(rig({{flip_id}, true, false, 1.0f, 0.0f}));
        session.run(70);
        CHECK_FALSE(session.play->failed());
        CHECK(session.said("'Root' cannot pose 'Arm/Hand' as its clip 'flip' says (a negative scale?)") == 1);
        CHECK(session.messages.size() == 1);
        CHECK(session.local(hand_id).scale.x == min_animated_scale); // its last pose: tick 30, where the scale reaches 0
    }
}

TEST_CASE("A recorded session that changes clips replays exactly", "[animation]") {
    constexpr AssetId keys{0x5c, 4};
    const auto scripts = std::map<AssetId, ScriptSource>{{keys, {"keys.luau", R"(
local K = {}
function K:fixed_update(dt)
    if maya.input.pressed("Space") then
        local lifting = self.entity:get("maya.animation", "clip") == "616e 2"
        self.entity:set("maya.animation", "clip", if lifting then "616e 1" else "616e 2")
    end
    if maya.input.pressed("F") then self.entity:set("maya.animation", "speed", -1.5) end
end
return K
)"}}};
    auto recorded = start(rig({{turn_id}, true, true, 1.0f, 0.0f}, ScriptComponent{{keys}, {}}), {}, scripts);
    recorded.play->start_recording();
    for (int tick = 0; tick < 240; ++tick) {
        auto events = std::vector<InputEvent>{};
        if (tick % 37 == 11) events.push_back(KeyEvent{KeyCode::Space, true, KeyModifiers::none});
        if (tick % 37 == 13) events.push_back(KeyEvent{KeyCode::Space, false, KeyModifiers::none});
        if (tick == 101) events.push_back(KeyEvent{KeyCode::F, true, KeyModifiers::none});
        recorded.play->input().feed(events);
        recorded.run(1, (tick % 3 == 0 ? 0.9 : 1.05) / 60.0); // uneven frames: the ticks, not the frames, decide
    }
    while (recorded.play->clock().tick() % recording_checkpoint_interval != 0) recorded.run(1);
    REQUIRE(recorded.play->recorded_checkpoints().size() >= 4);
    auto replayed = start(rig({{turn_id}, true, true, 1.0f, 0.0f}, ScriptComponent{{keys}, {}}), {}, scripts);
    replayed.play->start_replay(recorded.play->recorded_inputs(), recorded.play->recorded_checkpoints());
    while (!replayed.play->replay().finished) replayed.run(1, 1.0 / 30.0);
    CHECK(replayed.play->replay().checked == recorded.play->recorded_checkpoints().size());
    CHECK_FALSE(replayed.play->replay().first_difference);
    CHECK(replayed.play->state_hash() == recorded.play->state_hash());
    CHECK(replayed.local(arm_id).translation.y == recorded.local(arm_id).translation.y);
}

} // namespace

TEST_CASE("A joint may carry a static collider, which follows it", "[animation]") {
    // Moving bodies are roots (docs/physics.md), so a joint, which is below the animated entity, is at most
    // a static collider, which the clip moves like any entity.
    auto document = rig({{turn_id}, true, true, 1.0f, 0.0f});
    document.entities[2].components.push_back(ColliderComponent{});
    auto session = start(std::move(document));
    session.run(30);
    CHECK_FALSE(session.play->failed());
    CHECK(session.local(hand_id).translation.z == Approx(2.0f * 29 * dt));
    CHECK(session.messages.empty());
}
