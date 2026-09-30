#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/project_recording.hpp"
#include "maya/simulation/script_assets.hpp"
#include <fstream>
#include <sstream>

// Recording Play in the editor, replaying it in the player's path and the editor, and resetting
// exactly on Stop (#1023, docs/play.md#recording-and-replay).
using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
/// Adds a script to a project copy's catalog and returns its ID.
AssetId add_script(const ProjectCopy& copy, uint64_t low, const std::string& path, const std::string& source) {
    copy.write(path, source);
    auto catalog = copy.read("catalog.maya");
    auto line = std::ostringstream{};
    line << "script 6d617961 " << std::hex << low << " \"" << path << "\"\n";
    copy.write("catalog.maya", catalog + line.str());
    return {0x6d617961, low};
}

const auto zone_source = R"(
local Z = {}
function Z:start() self.entered = 0 end
function Z:on_trigger_enter(other) self.entered += 1; maya.log("entered " .. self.entered .. " by " .. other:name()) end
return Z
)";
const auto kicker_source = R"(
local K = {}
function K:fixed_update(dt)
    if maya.input.pressed("Space") then self.entity:add_impulse(vector.create(200, 3000, 0)) end
end
function K:on_contact_begin(other, contact) maya.log(string.format("hit %s at %.3f", other:name(), contact.speed)) end
return K
)";

/// physics.scene with a trigger zone that counts entries and a ball that Space kicks through it.
struct Stage {
    ProjectCopy copy;
    Harness harness{false};
    AssetId zone, kicker;
    Stage() {
        zone = add_script(copy, 0x40, "scripts/zone.luau", zone_source);
        kicker = add_script(copy, 0x41, "scripts/kicker.luau", kicker_source);
        REQUIRE(harness.shell.open_project(copy.folder));
        REQUIRE(harness.shell.open_scene(copy.content / "physics.scene"));
        harness.frames(2);
        auto& scene = *harness.shell.scene();
        auto sensor = ColliderComponent{};
        sensor.half_extents = {3, 0.5f, 3};
        sensor.sensor = true;
        REQUIRE(scene.create("Zone", std::nullopt, {TransformComponent{{0, 4, 4}, {}, math::Vec3(1.0f)}, sensor,
                                                   ScriptComponent{AssetRef<ScriptAsset>{zone}, {}}}));
        auto ball = ColliderComponent{};
        ball.shape = ColliderShape::sphere;
        REQUIRE(scene.create("Ball", std::nullopt, {TransformComponent{{-2, 0.5f, 4}, {}, math::Vec3(1.0f)}, ball, RigidBodyComponent{},
                                                   ScriptComponent{AssetRef<ScriptAsset>{kicker}, {}}}));
        harness.frames(2);
    }
    /// Plays and records `ticks` ticks at uneven frame times, pressing Space in the game view now and then.
    void play(uint64_t ticks) {
        REQUIRE(harness.shell.start_play(true));
        harness.frames(2);
        click(harness, harness.viewport_center()); // the game gets the keyboard
        harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
        REQUIRE(harness.shell.game_has_input());
        const float pattern[] = {0.3f, 1.7f, 0.9f, 1.4f, 0.5f, 1.2f, 1.0f};
        for (int frame = 0; harness.shell.play_session()->clock().tick() < ticks; ++frame) {
            auto events = std::vector<InputEvent>{};
            if (frame % 120 == 30) events.push_back(KeyEvent{KeyCode::Space, true, KeyModifiers::none});
            if (frame % 120 == 34) events.push_back(KeyEvent{KeyCode::Space, false, KeyModifiers::none});
            harness.frame(events, pattern[frame % 7] / 60.0f);
            REQUIRE(harness.shell.play_session());
        }
        harness.shell.stop_play();
    }
};

/// The player's path: the project as files, the recorded scene into a session, replayed. The
/// registry outlives the session, whose script sources read it.
struct PlayerReplay {
    std::unique_ptr<AssetRegistry> registry;
    std::unique_ptr<PlaySession> session;
};
PlayerReplay replay_as_player(const ProjectCopy& copy, EditorDevice& device, const PlayRecording& recording) {
    const auto project = open_project(copy.folder);
    REQUIRE(project);
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto scene = read_scene(recording.scene, context);
    REQUIRE(scene);
    INFO(replay_refusal(recording, recorded_assets(*assets.registry, scene.document), {}));
    REQUIRE(replay_refusal(recording, recorded_assets(*assets.registry, scene.document), {}).empty());
    auto scripts = project_script_settings(project.project.settings);
    scripts.seed = recording.seed;
    auto started = PlaySession::start(scene.document, context, play_systems(registry_script_sources(*assets.registry), scripts));
    INFO(started.error);
    REQUIRE(started);
    started.session->start_replay(recording.inputs, recording.checkpoints);
    for (int i = 0; i < 5000 && !started.session->replay().finished; ++i) REQUIRE(started.session->update(1.0 / 60.0).error.empty());
    return {std::move(assets.registry), std::move(started.session)};
}
} // namespace

TEST_CASE("A Play recorded in the editor replays identically in the player's path and in the editor", "[editor][play][replay]") {
    Stage stage;
    stage.play(1800);
    const auto& recording = stage.harness.shell.last_recording();
    REQUIRE(recording);
    CHECK(recording->inputs.size() >= 1800);
    CHECK(recording->checkpoints.size() == recording->inputs.size() / 60);
    CHECK(recording->reloads.empty());
    CHECK(recording->scene_name == "physics.scene");
    const auto pressed = std::ranges::count_if(recording->inputs.changes(), [](const InputTrack::Change& change) {
        return change.frame.went_down(KeyCode::Space);
    });
    CHECK(pressed >= 10); // the ball was kicked through the zone again and again

    // The player's path, from the project's files: every checkpoint and the final state match, so
    // component values, bodies, events, and what scripts logged are the same.
    auto device = EditorDevice{};
    const auto replayed = replay_as_player(stage.copy, device, *recording);
    const auto& player = replayed.session;
    CHECK(player->replay().finished);
    CHECK_FALSE(player->replay().first_difference);
    CHECK(player->replay().checked == recording->checkpoints.size());
    CHECK(player->full_state_hash() == recording->final_state);

    // The editor replays it too, and says so.
    REQUIRE(stage.harness.shell.replay_last_play());
    for (int i = 0; i < 3000 && !stage.harness.shell.play_session()->replay().finished; ++i) stage.harness.frame();
    stage.harness.frame();
    CHECK(logged(stage.harness.shell.diagnostics(), DiagnosticSource::play, "The replay matches the recording"));
    CHECK(stage.harness.shell.play_session()->clock().paused()); // it stays on its last frame
    stage.harness.shell.stop_play();

    // Saved, it reads back as the same recording.
    REQUIRE(stage.harness.shell.save_recording().empty());
    auto file = std::ifstream(stage.copy.content / "recordings/physics.recording");
    const auto read = read_recording(file);
    INFO(read.error);
    REQUIRE(read);
    CHECK(read.recording->final_state == recording->final_state);
    CHECK(read.recording->checkpoints == recording->checkpoints);
}

TEST_CASE("A Play with a script reload is recorded as such, and its replay is refused", "[editor][play][replay]") {
    Stage stage;
    REQUIRE(stage.harness.shell.start_play(true));
    stage.harness.frames(10);
    stage.copy.write("scripts/zone.luau", std::string(zone_source) + "-- edited\n");
    stage.harness.shell.check_script_files();
    stage.harness.frames(5);
    stage.harness.shell.stop_play();
    const auto& recording = stage.harness.shell.last_recording();
    REQUIRE(recording);
    REQUIRE(recording->reloads.size() == 1);
    CHECK(recording->reloads[0].name == "scripts/zone.luau");
    CHECK_FALSE(stage.harness.shell.replay_last_play());
    CHECK(stage.harness.shell.prompt() == EditorPrompt::notice);
    CHECK(logged(stage.harness.shell.diagnostics(), DiagnosticSource::play,
                 "Couldn't replay: scripts/zone.luau was reloaded at tick"));
}

TEST_CASE("A hundred Plays and Stops return to the authored scene and the empty-session baseline", "[editor][play][replay]") {
    Stage stage;
    auto& harness = stage.harness;
    auto& scene = *harness.shell.scene();
    const auto ball = find_named(scene, "Ball");
    scene.select(ball);
    // One Play first, so everything made once per process exists.
    REQUIRE(harness.shell.start_play());
    harness.frames(3);
    harness.shell.stop_play();
    harness.frames(4);
    const auto before = harness.device.stats();
    const auto physics_before = physics_memory().live_bytes;
    const auto authored = scene.document();
    auto authored_text = std::ostringstream{};
    REQUIRE(write_scene(authored_text, authored, asset_property_context(*harness.shell.assets())).empty());
    const auto history = scene.history_size();
    const auto dirty = scene.dirty();
    for (int round = 0; round < 100; ++round) {
        REQUIRE(harness.shell.start_play());
        harness.frames(4);
        CHECK(script_memory_in_use() > 0); // a VM while playing
        harness.shell.stop_play();
        harness.frames(1);
        REQUIRE(script_memory_in_use() == 0); // released at Stop
    }
    harness.frames(4); // retired resources pass through the frames in flight
    const auto after = harness.device.stats();
    CHECK(after.buffers == before.buffers);
    CHECK(after.textures == before.textures);
    CHECK(after.pending_retirements == before.pending_retirements);
    CHECK(physics_memory().live_bytes == physics_before);
    auto text_after = std::ostringstream{};
    REQUIRE(write_scene(text_after, scene.document(), asset_property_context(*harness.shell.assets())).empty());
    CHECK(text_after.str() == authored_text.str());
    CHECK(scene.history_size() == history);
    CHECK(scene.dirty() == dirty);
    CHECK(scene.selection() == std::vector<EntityId>{ball});
    CHECK_FALSE(harness.shell.last_recording()); // plain Play records nothing
}
