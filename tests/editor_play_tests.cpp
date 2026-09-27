#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include <catch2/catch_approx.hpp>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;

namespace {
const auto camera_id = EntityId{0x6d617961, 0x100};
const auto pyramid_id = EntityId{0x6d617961, 0x200};
const auto red_cube_id = EntityId{0x6d617961, 0x201};
const auto blue_cube_id = EntityId{0x6d617961, 0x202};

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
TransformComponent transform_in(const World& world, EntityId id) {
    auto value = read_component(world, *world.find(id), ComponentId::transform);
    REQUIRE(value);
    return std::get<TransformComponent>(*value);
}
/// Canonical text of a World: equal text means equal IDs, hierarchy, and values.
std::string text(const World& world) {
    auto out = std::ostringstream{};
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    REQUIRE(write_scene(out, capture_scene(world), any_asset).empty());
    return out.str();
}
void tap(Harness& harness, KeyCode code) {
    harness.frame(key(code, true));
    harness.frame(key(code, false));
}
} // namespace

TEST_CASE("Play runs a separate World; Stop restores the authored scene and selection", "[editor][play]") {
    Harness harness;
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    scene.select(red_cube_id);
    REQUIRE(scene.rename(blue_cube_id, "Unsaved blue")); // play shows unsaved edits too
    const auto authored = text(scene.world());
    const auto history = scene.history_size();
    REQUIRE(scene.dirty());

    chord(harness, {KeyCode::LeftSuper}, KeyCode::P);
    auto* play = harness.shell.play_session();
    REQUIRE(play);
    CHECK(&play->world() != &scene.world());
    CHECK(play->camera() == camera_id);
    CHECK(scene.locked());
    const auto started = play->clock().tick();
    harness.frames(30);
    CHECK(play->clock().tick() == started + 30); // one 60 Hz tick per 1/60 s frame
    // The pyramid spins in the play World only.
    CHECK(transform_in(play->world(), pyramid_id).rotation.w < 0.999f);
    CHECK(transform_in(scene.world(), pyramid_id).rotation.w == 1.0f);
    CHECK(text(scene.world()) == authored);
    // Unsaved edits were played: the play World started from the scene as it was.
    const auto blue = read_component(play->world(), *play->world().find(blue_cube_id), ComponentId::name);
    CHECK(std::get<NameComponent>(*blue).value == "Unsaved blue");

    // The authored scene cannot change while playing, but the selection can.
    CHECK(scene.rename(red_cube_id, "Nope").error == "Stop playing to edit the scene");
    CHECK_FALSE(scene.can_undo());
    CHECK(scene.undo().error == "Stop playing to edit the scene");
    press(harness, row_center(harness, blue_cube_id));
    CHECK(scene.primary() == blue_cube_id);
    tap(harness, KeyCode::Delete);
    CHECK(scene.record(blue_cube_id)); // the hierarchy does not delete during play
    chord(harness, {KeyCode::LeftSuper}, KeyCode::Z);
    CHECK(scene.display_name(blue_cube_id) == "Unsaved blue");
    CHECK(harness.shell.renaming() == std::nullopt);

    // Stop: the play World goes; the authored scene, history, unsaved state, and selection return.
    press(harness, control(harness, "play.play"));
    CHECK_FALSE(harness.shell.play_session());
    CHECK_FALSE(scene.locked());
    CHECK(text(scene.world()) == authored);
    CHECK(scene.history_size() == history);
    CHECK(scene.dirty());
    CHECK(scene.selection() == std::vector{red_cube_id});
    CHECK(scene.undo()); // editing works again
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play, "Stopped after"));
}

TEST_CASE("The same scene plays the same in the player's path and in the editor", "[editor][play]") {
    // The player's path: the project's scene file, straight into a play session.
    auto device = EditorDevice{};
    const auto project = open_project(sample_project());
    REQUIRE(project);
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device));
    REQUIRE(assets);
    auto loaded = load_scene_file(*project.project.startup_scene, asset_property_context(*assets.registry));
    REQUIRE(loaded);
    auto player = PlaySession::start(std::move(loaded.document), asset_property_context(*assets.registry), builtin_systems());
    REQUIRE(player);
    // The editor's path: the open scene's document, through the Play button.
    Harness harness;
    harness.frames(2);
    press(harness, control(harness, "play.play"));
    auto* editor = harness.shell.play_session();
    REQUIRE(editor);
    while (editor->clock().tick() < 120) harness.frame();
    while (player.session->clock().tick() < editor->clock().tick()) player.session->update(1.0 / 60.0);
    CHECK(player.session->clock().tick() == editor->clock().tick());
    CHECK(player.session->camera() == editor->camera());
    CHECK(text(player.session->world()) == text(editor->world()));
}

TEST_CASE("Playing and stopping again and again releases what each play made", "[editor][play]") {
    Harness harness;
    harness.frames(4);
    const auto before = harness.device.stats();
    const auto authored = text(harness.shell.scene()->world());
    for (int round = 0; round < 40; ++round) {
        REQUIRE(harness.shell.start_play());
        harness.frames(3);
        harness.shell.stop_play();
        harness.frames(1);
    }
    harness.frames(4); // let retired resources pass through the frames in flight
    const auto after = harness.device.stats();
    CHECK(after.buffers == before.buffers);
    CHECK(after.textures == before.textures);
    CHECK(after.pipelines == before.pipelines);
    CHECK(after.pending_retirements == before.pending_retirements);
    CHECK(text(harness.shell.scene()->world()) == authored);
    CHECK_FALSE(harness.shell.scene()->can_undo());
    CHECK(harness.shell.viewport().allocations() == 1); // the view was never reallocated
}

TEST_CASE("The game gets the mouse and keyboard only after a click on the game view", "[editor][play]") {
    Harness harness;
    harness.frames(3);
    REQUIRE(harness.shell.start_play());
    harness.frames(2);
    auto& play = *harness.shell.play_session();
    const auto start = transform_in(play.world(), camera_id).translation;
    // Before the click, keys stay with the editor.
    harness.frame({MouseMoveEvent{harness.viewport_center().x, harness.viewport_center().y}});
    harness.frame(key(KeyCode::W, true));
    harness.frames(10);
    harness.frame(key(KeyCode::W, false));
    CHECK(transform_in(play.world(), camera_id).translation.z == start.z);
    // A click on the game view hands them over: W flies the scene's camera (maya.fly_control).
    click(harness, harness.viewport_center());
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    CHECK(harness.shell.game_has_input());
    CHECK(harness.captured);
    harness.frame(key(KeyCode::W, true));
    harness.frames(30);
    harness.frame(key(KeyCode::W, false));
    const auto flown = transform_in(play.world(), camera_id).translation.z;
    CHECK(flown == Approx(start.z - 1.5f).margin(0.06f)); // 3 m/s for about half a second
    // Shortcuts belong to the game too: ⌘P does not stop play.
    chord(harness, {KeyCode::LeftSuper}, KeyCode::P);
    CHECK(harness.shell.play_session() == &play);
    // Escape takes them back.
    tap(harness, KeyCode::Escape);
    CHECK_FALSE(harness.shell.game_has_input());
    CHECK_FALSE(harness.captured);
    harness.frame(key(KeyCode::W, true));
    harness.frames(10);
    harness.frame(key(KeyCode::W, false));
    CHECK(transform_in(play.world(), camera_id).translation.z == flown);
    // In the scene view, a click does not give the game the input, and the editor camera can fly.
    const auto before_scene = transform_in(play.world(), camera_id).translation.z;
    press(harness, control(harness, "view.scene"));
    CHECK_FALSE(harness.shell.game_view());
    click(harness, harness.viewport_center());
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    CHECK_FALSE(harness.shell.game_has_input());
    const auto editor_camera = harness.shell.camera().position;
    harness.frame({MouseButtonEvent{MouseButton::right, true, KeyModifiers::none}});
    harness.frame(key(KeyCode::W, true));
    harness.frames(10);
    harness.frame(key(KeyCode::W, false));
    harness.frame({MouseButtonEvent{MouseButton::right, false, KeyModifiers::none}});
    CHECK(harness.shell.camera().position.z != editor_camera.z);
    CHECK(transform_in(play.world(), camera_id).translation.z == before_scene);
    // Taking the input back with Escape while a key is held releases it: the camera stops.
    press(harness, control(harness, "view.game"));
    click(harness, harness.viewport_center());
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    REQUIRE(harness.shell.game_has_input());
    harness.frame(key(KeyCode::S, true));
    harness.frames(5);
    tap(harness, KeyCode::Escape);
    const auto stopped = transform_in(play.world(), camera_id).translation.z;
    CHECK(stopped > flown); // it backed away while S was held
    harness.frames(10);
    CHECK(transform_in(play.world(), camera_id).translation.z == stopped);
    harness.frame(key(KeyCode::S, false)); // the release reaches the UI, not the game
    // Losing focus while the game has the input takes it back and releases held keys.
    press(harness, control(harness, "view.game"));
    CHECK(harness.shell.game_view());
    click(harness, harness.viewport_center());
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    REQUIRE(harness.shell.game_has_input());
    harness.frame(key(KeyCode::W, true));
    harness.frame({FocusEvent{false}});
    CHECK_FALSE(harness.shell.game_has_input());
    const auto released = transform_in(play.world(), camera_id).translation.z;
    harness.frames(10);
    CHECK(transform_in(play.world(), camera_id).translation.z == released);
}

TEST_CASE("Pause holds the play World and Step advances it one tick", "[editor][play]") {
    Harness harness;
    harness.frames(3);
    press(harness, control(harness, "play.play"));
    auto& play = *harness.shell.play_session();
    press(harness, control(harness, "play.pause"));
    REQUIRE(play.clock().paused());
    const auto paused = play.clock().tick();
    const auto pose = transform_in(play.world(), pyramid_id).rotation;
    harness.frames(20);
    CHECK(play.clock().tick() == paused);
    CHECK(transform_in(play.world(), pyramid_id).rotation.y == pose.y);
    press(harness, control(harness, "play.step"));
    harness.frame(); // the button acts on release; the tick runs in the next frame's update
    CHECK(play.clock().tick() == paused + 1);
    CHECK(transform_in(play.world(), pyramid_id).rotation.y != pose.y);
    chord(harness, {KeyCode::LeftSuper, KeyCode::LeftShift}, KeyCode::P); // resume
    CHECK_FALSE(play.clock().paused());
    const auto resumed = play.clock().tick();
    harness.frames(10);
    CHECK(play.clock().tick() == resumed + 10); // no burst for the paused time
}

TEST_CASE("Opening or creating a scene ends play; closing is not held up by it", "[editor][play]") {
    Harness harness;
    harness.frames(2);
    REQUIRE(harness.shell.start_play());
    harness.frames(2);
    harness.shell.request_new_scene(); // no unsaved changes, so it happens at once
    CHECK_FALSE(harness.shell.play_session());
    CHECK_FALSE(harness.shell.scene()->locked());
    CHECK(harness.shell.scene_path().empty());
    REQUIRE(harness.shell.start_play());
    CHECK(harness.shell.request_close()); // play has nothing of its own to save
    REQUIRE(harness.shell.open_scene("basic.scene"));
    CHECK_FALSE(harness.shell.play_session());
    CHECK_FALSE(harness.shell.scene()->locked());
}

TEST_CASE("A new scene without a camera plays through the editor camera", "[editor][play]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    scene.select(camera_id);
    REQUIRE(scene.delete_selection());
    REQUIRE(harness.shell.start_play());
    harness.frames(3);
    CHECK_FALSE(harness.shell.play_session()->camera());
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play, "no camera"));
    CHECK(harness.shell.viewport().allocations() == 1);
    CHECK_FALSE(harness.shell.layout().control("view.game") == nullptr); // shown, but disabled
    click(harness, harness.viewport_center());
    CHECK_FALSE(harness.shell.game_has_input());
}

TEST_CASE("A scene that cannot be played says why and stays editable", "[editor][play]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    // The catalog loses the red material that the red cube uses.
    auto catalog = copy.read("catalog.maya");
    copy.write("catalog.maya", catalog.erase(catalog.find("material 6d617961 11"), 46));
    harness.shell.refresh_project();
    CHECK_FALSE(harness.shell.start_play());
    CHECK_FALSE(harness.shell.play_session());
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    CHECK(harness.shell.prompt_message().find("6d617961:11") != std::string::npos);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play, "6d617961:11"));
    CHECK_FALSE(harness.shell.scene()->locked());
    CHECK(harness.shell.scene()->rename(red_cube_id, "Still editable"));
}
