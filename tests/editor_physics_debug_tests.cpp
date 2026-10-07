#include "editor_harness.hpp"
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <sstream>

// Physics debug views, collider handles, and physics stats in the editor (#1022,
// docs/editor.md#physics-debug-views).
using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;

namespace {
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
ColliderComponent collider_of(SceneEditor& scene, EntityId id) {
    return std::get<ColliderComponent>(*read_component(scene.world(), *scene.world().find(id), ComponentId::collider));
}
/// The sample's physics scene, with "Crate 1" selected, framed, and its collider handles shown.
EntityId edit_crate(Harness& harness) {
    REQUIRE(harness.shell.open_scene("physics.scene"));
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto crate = find_named(scene, "Crate 1");
    scene.select(crate);
    harness.frame({MouseMoveEvent{harness.viewport_center().x, harness.viewport_center().y}});
    harness.frame(key(KeyCode::F, true));
    harness.frame(key(KeyCode::F, false));
    harness.frame(key(KeyCode::C, true));
    harness.frame(key(KeyCode::C, false));
    REQUIRE(harness.shell.collider_editing());
    harness.frames(2);
    return crate;
}
/// Drags a handle outward, along the line from the collider's centre through it on screen.
void pull(Harness& harness, std::string_view handle, float pixels) {
    const auto from = control(harness, handle);
    const auto centre = control(harness, "collider.centre");
    auto dx = from.x - centre.x, dy = from.y - centre.y;
    const auto length = std::sqrt(dx * dx + dy * dy);
    REQUIRE(length > 4.0f);
    dx /= length;
    dy /= length;
    drag(harness, from, {from.x + dx * pixels, from.y + dy * pixels});
}
/// Drags a handle in past the centre, to twice as far beyond it as it was.
void pull_through(Harness& harness, std::string_view handle) {
    const auto from = control(harness, handle);
    const auto centre = control(harness, "collider.centre");
    drag(harness, from, {centre.x + (centre.x - from.x) * 2.0f, centre.y + (centre.y - from.y) * 2.0f});
}
} // namespace

TEST_CASE("Editor preferences round-trip, skip lines they do not know, and refuse other files", "[editor][physics-debug]") {
    auto preferences = EditorPreferences{};
    preferences.physics_debug = {uint8_t(PhysicsDebugCategory::colliders) | uint8_t(PhysicsDebugCategory::queries), 0x00F3};
    auto text = std::stringstream{};
    write_preferences(text, preferences);
    CHECK(text.str() == "maya-editor-preferences 1\nphysics-debug colliders queries\nphysics-debug-groups f3\ndebug-view none\nskeletons off\n");
    auto read = read_preferences(text);
    CHECK(read.error.empty());
    CHECK(read.preferences == preferences);
    preferences.debug_view = DebugView::false_color; // #1032, one choice since #1037
    auto exposure = std::stringstream{};
    write_preferences(exposure, preferences);
    CHECK(exposure.str().find("\ndebug-view false-color\n") != std::string::npos);
    CHECK(read_preferences(exposure).preferences.debug_view == DebugView::false_color);
    auto unknown_view = std::istringstream("maya-editor-preferences 1\ndebug-view sepia\n");
    CHECK(read_preferences(unknown_view).error == "debug-view needs a debug view's name, such as none, base-color, or false-color");
    // Files from #1032 and #1034 kept exposure and shadow views apart; either is read as the one view.
    auto older = std::istringstream("maya-editor-preferences 1\nexposure-view none\nshadow-view texels\n");
    CHECK(read_preferences(older).preferences.debug_view == DebugView::texels);
    auto oldest = std::istringstream("maya-editor-preferences 1\nexposure-view luminance\n");
    CHECK(read_preferences(oldest).preferences.debug_view == DebugView::luminance);

    auto newer = std::istringstream("maya-editor-preferences 1\ntheme \"night\"\nphysics-debug contacts sparkles\n");
    read = read_preferences(newer);
    CHECK(read.error.empty());
    CHECK(read.preferences.physics_debug.categories == uint8_t(PhysicsDebugCategory::contacts));
    CHECK(read.preferences.physics_debug.groups == all_collision_groups);
    auto foreign = std::istringstream("maya-scene 1\n");
    CHECK_FALSE(read_preferences(foreign).error.empty());
    auto bad = std::istringstream("maya-editor-preferences 1\nphysics-debug-groups zz\n");
    CHECK(read_preferences(bad).error == "physics-debug-groups needs a hexadecimal mask");
}

TEST_CASE("Physics debug toggles draw in the viewport, cost no debug draws when off, and are saved as preferences", "[editor][physics-debug]") {
    const auto folder = fs::temp_directory_path() / ("maya-preferences-" + std::to_string(::getpid()));
    fs::remove_all(folder);
    const auto file = folder / "editor.preferences";
    {
        Harness harness;
        harness.shell.use_preferences_file(file);
        REQUIRE(harness.shell.open_scene("physics.scene"));
        harness.frames(3);
        // Off by default: nothing drawn, and no debug draw encoded.
        CHECK_FALSE(harness.shell.physics_debug().any());
        CHECK(harness.shell.viewport_debug().empty());
        CHECK(harness.shell.renderer_stats().debug_draws == 0);
        CHECK_FALSE(fs::exists(file)); // nothing changed, nothing saved

        // The menu's checks: colliders on, from the viewport's tool bar.
        press(harness, control(harness, "tool.physics-debug"));
        harness.frames(2); // the menu sizes itself on its first frame
        press(harness, control(harness, "debug.colliders"));
        harness.frames(2);
        CHECK(harness.shell.physics_debug().has(PhysicsDebugCategory::colliders));
        CHECK(harness.shell.viewport_debug().shapes.size() == 7); // the floor and six crates
        CHECK(harness.shell.renderer_stats().debug_draws > 0);
        // A group off hides the bodies in it.
        press(harness, control(harness, "debug.group.0"));
        harness.frames(2);
        CHECK(harness.shell.physics_debug().groups == uint16_t(all_collision_groups & ~1u));
        CHECK(harness.shell.viewport_debug().empty());
        press(harness, control(harness, "debug.groups.all"));
        harness.frames(2);
        CHECK(harness.shell.viewport_debug().shapes.size() == 7);
        CHECK(fs::exists(file));

        // Off again: debug draws stop.
        harness.shell.set_physics_debug({});
        harness.frames(2);
        const auto draws = harness.shell.renderer_stats().debug_draws;
        harness.frames(3);
        CHECK(harness.shell.renderer_stats().debug_draws == draws);
        harness.shell.set_physics_debug({uint8_t(PhysicsDebugCategory::triggers) | uint8_t(PhysicsDebugCategory::contacts), 0x0003});
    }
    // A new editor starts with them.
    Harness again;
    again.shell.use_preferences_file(file);
    CHECK(again.shell.physics_debug() == PhysicsDebugOptions{uint8_t(PhysicsDebugCategory::triggers) | uint8_t(PhysicsDebugCategory::contacts), 0x0003});
    fs::remove_all(folder);
}

TEST_CASE("Play draws from the physics world, captures contacts only while shown, and shows physics stats", "[editor][physics-debug]") {
    Harness harness;
    REQUIRE(harness.shell.open_scene("physics.scene"));
    harness.shell.set_physics_debug({uint8_t(PhysicsDebugCategory::contacts)});
    REQUIRE(harness.shell.start_play());
    harness.shell.set_game_view(false);
    harness.frames(5);
    auto* session = harness.shell.play_session();
    REQUIRE(session);
    CHECK(session->physics().debug_capture());
    CHECK_FALSE(harness.shell.viewport_debug().lines.empty()); // the stack's contacts, while it is awake
    CHECK(harness.shell.viewport_debug().shapes.empty());

    harness.shell.set_physics_debug({uint8_t(PhysicsDebugCategory::body_state)});
    harness.frames(25);
    CHECK_FALSE(session->physics().debug_capture()); // outlines need no capture
    CHECK(harness.shell.viewport_debug().shapes.size() == 7);
    CHECK(harness.shell.viewport_debug().lines.empty());

    // Diagnostics: the stats it shows are the session's, refreshed four times a second.
    harness.frames(20);
    const auto& shown = harness.shell.shown_physics();
    REQUIRE(shown);
    CHECK(shown->bodies == 7);
    CHECK(shown->static_bodies == 1);
    CHECK(shown->dynamic_bodies == 6);
    CHECK(shown->steps > 30);
    CHECK(shown->contacts > 0);
    CHECK(shown->steps_with_errors == 0);
    harness.shell.stop_play();
    harness.frames(20);
    CHECK_FALSE(harness.shell.shown_physics());
}

TEST_CASE("Collider handles resize and move a collider as undoable edits, holding the opposite face still", "[editor][physics-debug]") {
    Harness harness;
    const auto crate = edit_crate(harness);
    auto& scene = *harness.shell.scene();
    CHECK_FALSE(harness.shell.layout().gizmo_origin); // the handles replace the gizmo
    for (const auto* handle : {"collider.centre", "collider.+x", "collider.-x", "collider.+y", "collider.-y", "collider.+z", "collider.-z"})
        CHECK(harness.shell.layout().control(handle));
    const auto before = collider_of(scene, crate);
    const auto history = scene.history_size();

    // A box face: wider by what it moved, and the opposite face stays where it was.
    pull(harness, "collider.+x", 60.0f);
    auto after = collider_of(scene, crate);
    CHECK(after.half_extents.x > before.half_extents.x + 0.1f);
    CHECK(after.half_extents.y == before.half_extents.y);
    CHECK(after.offset.x - after.half_extents.x == Approx(before.offset.x - before.half_extents.x).margin(1e-5));
    CHECK(after.offset.x > 0.0f);
    CHECK(scene.history_size() == history + 1); // the whole drag is one step
    CHECK(scene.undo_label() == "Resize the collider of Crate 1");
    REQUIRE(scene.undo());
    harness.frames(2);
    CHECK(collider_of(scene, crate).half_extents.x == before.half_extents.x);
    CHECK(collider_of(scene, crate).offset.x == before.offset.x);
    REQUIRE(scene.redo());
    CHECK(collider_of(scene, crate).half_extents.x == after.half_extents.x);
    REQUIRE(scene.undo());
    harness.frames(2);

    // The centre moves the offset; the size stays.
    const auto from = control(harness, "collider.centre");
    drag(harness, from, {from.x + 40.0f, from.y});
    after = collider_of(scene, crate);
    CHECK(after.offset.x > 0.05f);
    CHECK(after.half_extents.x == before.half_extents.x);
    CHECK(scene.undo_label() == "Move the collider of Crate 1");
    REQUIRE(scene.undo());
    harness.frames(2);

    // A sphere's handles change its radius about a fixed centre.
    auto sphere = before;
    sphere.shape = ColliderShape::sphere;
    REQUIRE(scene.set_component(crate, sphere));
    harness.frames(2);
    pull(harness, "collider.+y", 40.0f);
    after = collider_of(scene, crate);
    CHECK(after.radius > sphere.radius + 0.05f);
    CHECK(after.offset.y == sphere.offset.y);

    // A capsule's end lengthens it, holding the other end.
    auto capsule = before;
    capsule.shape = ColliderShape::capsule;
    capsule.radius = 0.25f;
    capsule.half_height = 0.25f;
    REQUIRE(scene.set_component(crate, capsule));
    harness.frames(2);
    pull(harness, "collider.+y", 40.0f);
    after = collider_of(scene, crate);
    CHECK(after.half_height > capsule.half_height + 0.05f);
    CHECK(after.radius == capsule.radius);
    CHECK(after.offset.y - after.half_height == Approx(capsule.offset.y - capsule.half_height).margin(1e-5));

    // Off, the gizmo is back; while playing there are no handles.
    harness.shell.set_collider_editing(false);
    harness.frames(2);
    CHECK(harness.shell.layout().gizmo_origin);
    CHECK_FALSE(harness.shell.layout().control("collider.+x"));
    harness.shell.set_collider_editing(true);
    REQUIRE(harness.shell.start_play());
    harness.shell.set_game_view(false);
    harness.frames(2);
    CHECK_FALSE(harness.shell.layout().control("collider.+x"));
    harness.shell.stop_play();
}

TEST_CASE("Handles never make a size the Inspector would refuse: pulled past the other side, it stops small", "[editor][physics-debug]") {
    Harness harness;
    const auto crate = edit_crate(harness);
    auto& scene = *harness.shell.scene();
    pull_through(harness, "collider.+x");
    CHECK(collider_of(scene, crate).half_extents.x == Approx(0.005f));
    CHECK(harness.shell.edit_error().empty());

    auto capsule = collider_of(scene, crate);
    capsule.shape = ColliderShape::capsule;
    capsule.radius = 0.25f;
    capsule.half_height = 0.25f;
    REQUIRE(scene.set_component(crate, capsule));
    harness.frames(2);
    pull_through(harness, "collider.+y");
    CHECK(collider_of(scene, crate).half_height == Approx(0.005f)); // still positive, as validation requires
    CHECK(harness.shell.edit_error().empty());
}

TEST_CASE("Exposure views come from the eye menu and are saved; the editor camera has its own exposure", "[editor][tone]") {
    const auto folder = fs::temp_directory_path() / ("maya-exposure-" + std::to_string(::getpid()));
    fs::remove_all(folder);
    const auto file = folder / "editor.preferences";
    {
        Harness harness;
        harness.shell.use_preferences_file(file);
        harness.frames(3);
        CHECK(harness.shell.debug_view() == DebugView::none);
        press(harness, control(harness, "tool.physics-debug"));
        harness.frames(2); // the menu sizes itself on its first frame
        press(harness, control(harness, "debug.view.false-color"));
        harness.frames(2);
        CHECK(harness.shell.debug_view() == DebugView::false_color);
        CHECK(fs::exists(file));
        press(harness, control(harness, "debug.view.luminance"));
        harness.frames(2);
        CHECK(harness.shell.debug_view() == DebugView::luminance);
    }
    // A new editor starts with it.
    Harness again;
    again.shell.use_preferences_file(file);
    CHECK(again.shell.debug_view() == DebugView::luminance);
    again.frames(3);

    // The Scene view's exposure is the editor camera's, typed in the Inspector; the scene is not edited.
    CHECK(again.shell.camera().camera.exposure == 0.0f);
    const auto field = control(again, "camera.exposure");
    click(again, {field.x - 30.0f, field.y}); // the value, left of the step buttons
    again.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    REQUIRE(again.shell.ui_wants_text());
    chord(again, {KeyCode::LeftSuper}, KeyCode::A);
    for (const auto c : std::string("-1.5")) again.frame({TextEvent{uint32_t(c)}});
    again.frame(key(KeyCode::Enter, true));
    again.frame(key(KeyCode::Enter, false));
    again.frames(1);
    CHECK(again.shell.camera().camera.exposure == -1.5f);
    CHECK_FALSE(again.shell.scene()->dirty());
    CHECK(again.shell.layout().control("camera.tone-mapping"));
    fs::remove_all(folder);
}
