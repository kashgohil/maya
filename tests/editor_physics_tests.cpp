#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include <catch2/catch_approx.hpp>
#include <sstream>

// Authoring physics in the editor (#1019): collider and rigid-body components in the Inspector, the
// project's collision groups, and physics scenes played in the editor and the player.
using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;

namespace {
ImVec2 centre(const EditorLayout::Field& rect) { return {(rect.min.x + rect.max.x) / 2, (rect.min.y + rect.max.y) / 2}; }

ColliderComponent collider_of(SceneEditor& scene, EntityId id) {
    return std::get<ColliderComponent>(*read_component(scene.world(), *scene.world().find(id), ComponentId::collider));
}
/// Canonical text of a World: equal text means equal IDs, hierarchy, and values.
std::string text(const World& world) {
    auto out = std::ostringstream{};
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    REQUIRE(write_scene(out, capture_scene(world), any_asset).empty());
    return out.str();
}
void type_into(Harness& harness, ImVec2 at, const std::string& value) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    REQUIRE(harness.shell.ui_wants_text());
    harness.frame({KeyEvent{KeyCode::LeftSuper, true, KeyModifiers::super}});
    harness.frame(key(KeyCode::A, true)); // select all
    harness.frame(key(KeyCode::A, false));
    harness.frame({KeyEvent{KeyCode::LeftSuper, false, KeyModifiers::none}});
    for (const auto c : value) harness.frame({TextEvent{uint32_t(c)}});
    harness.frame(key(KeyCode::Enter, true));
    harness.frame(key(KeyCode::Enter, false));
}
} // namespace

TEST_CASE("Colliders and rigid bodies are added and edited in the Inspector as undoable steps", "[editor][physics]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    scene.select(cube);
    REQUIRE(scene.set_component(cube, RigidBodyComponent{}));
    harness.frames(2);
    // Without a collider, the Inspector says Play will refuse the body.
    auto note = harness.shell.physics_note(cube, ComponentId::rigid_body);
    CHECK(note.warning);
    CHECK(note.text.find("Add a collider") != std::string::npos);
    const auto history = scene.history_size();
    REQUIRE(scene.set_component(cube, ColliderComponent{}));
    harness.frames(2);
    // With a collider, what is left is the sample cube's 0.7 scale.
    CHECK(harness.shell.physics_note(cube, ComponentId::rigid_body).text.find("needs unit scale") != std::string::npos);
    CHECK(scene.history_size() == history + 1);
    // A box shows its half extents; a sphere its radius.
    const auto& layout = harness.shell.layout();
    CHECK(layout.field("collider.half_extents.x"));
    CHECK_FALSE(layout.field("collider.radius"));
    CHECK(layout.field("collider.group"));
    CHECK(layout.field("collider.mask"));
    CHECK(layout.field("rigid_body.linear_velocity.x"));
    auto sphere = collider_of(scene, cube);
    sphere.shape = ColliderShape::sphere;
    REQUIRE(scene.set_component(cube, sphere));
    harness.frames(2);
    CHECK(layout.field("collider.radius"));
    CHECK_FALSE(layout.field("collider.half_extents.x"));
    CHECK(scene.undo_label() == "Edit Collider");
    // A drag on the radius is one undo step.
    const auto before = collider_of(scene, cube).radius;
    const auto from = centre(*layout.field("collider.radius"));
    drag(harness, from, {from.x + 60.0f, from.y}, 10);
    CHECK(collider_of(scene, cube).radius > before);
    REQUIRE(scene.undo());
    CHECK(collider_of(scene, cube).radius == before);
    REQUIRE(scene.undo()); // back to a box
    CHECK(collider_of(scene, cube).shape == ColliderShape::box);
    // Invalid values are refused with the schema's reason.
    auto bouncy = collider_of(scene, cube);
    bouncy.restitution = 2.0f;
    CHECK(scene.set_component(cube, bouncy).error == "Value is nonfinite or outside its allowed range");
    auto kinematic = RigidBodyComponent{};
    kinematic.motion = BodyMotion::kinematic;
    kinematic.linear_velocity = {1.0f, 0.0f, 0.0f};
    CHECK(scene.set_component(cube, kinematic).error.find("A kinematic body has no initial velocity") == 0);
    // A kinematic body's initial velocity is not shown.
    kinematic.linear_velocity = {};
    REQUIRE(scene.set_component(cube, kinematic));
    harness.frames(2);
    CHECK_FALSE(layout.field("rigid_body.linear_velocity.x"));
    // Removing the rigid body is undoable too.
    REQUIRE(scene.remove_component(cube, ComponentId::rigid_body));
    REQUIRE(scene.undo());
    CHECK(scene.world().has<RigidBodyComponent>(*scene.world().find(cube)));
}

TEST_CASE("The Inspector says where a collider belongs and where a body cannot go", "[editor][physics]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto base = find_named(scene, "Red cube");
    const auto child = find_named(scene, "Blue cube");
    REQUIRE(scene.set_component(base, RigidBodyComponent{}));
    REQUIRE(scene.set_component(child, ColliderComponent{}));
    REQUIRE(scene.move(child, base));
    const auto note = harness.shell.physics_note(child, ComponentId::collider);
    CHECK_FALSE(note.warning);
    CHECK(note.text == "Part of the rigid body on Red cube.");
    // Its shape is the child's collider; only the sample cube's scale is left to fix.
    CHECK(harness.shell.physics_note(base, ComponentId::rigid_body).text.find("needs unit scale") != std::string::npos);
    REQUIRE(scene.set_component(child, RigidBodyComponent{}));
    CHECK(harness.shell.physics_note(child, ComponentId::rigid_body).text ==
          "A rigid body must be on a root entity; Play refuses it here.");
}

TEST_CASE("Collision groups are named in the project, saved with it, and shown in the Inspector", "[editor][physics][project]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    CHECK(harness.shell.collision_groups() == default_collision_groups());
    CHECK(harness.shell.rename_collision_group(1, "Player").empty());
    CHECK(harness.shell.rename_collision_group(2, "\"quoted\"") ==
          "A collision group name cannot contain quotes, backslashes, or control characters");
    CHECK(harness.shell.rename_collision_group(16, "Far") == "Collision groups are numbered 0 to 15");
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::project, "Collision group 1 is now Player"));
    // The window renames a group as typed.
    harness.shell.show_collision_groups(true);
    harness.frames(3);
    const auto* field = harness.shell.layout().control("group.2");
    REQUIRE(field);
    type_into(harness, centre(*field), "Water");
    harness.frames(2);
    CHECK(harness.shell.collision_groups()[2] == "Water");
    // Both names are in the project file, and reopening reads them back.
    const auto reopened = open_project(copy.folder);
    REQUIRE(reopened);
    CHECK(reopened.project.settings.collision_groups[1] == "Player");
    CHECK(reopened.project.settings.collision_groups[2] == "Water");
    CHECK(reopened.project.settings.content == "assets");
    CHECK(reopened.project.settings.startup_scene == "basic.scene");
    // An unnamed group goes back to its number.
    CHECK(harness.shell.rename_collision_group(2, "").empty());
    CHECK(collision_group_label(harness.shell.collision_groups(), 2) == "Group 2");
}

TEST_CASE("Play refuses a scene whose physics cannot be built and says why", "[editor][physics][play]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    REQUIRE(scene.set_component(cube, RigidBodyComponent{}));
    CHECK_FALSE(harness.shell.start_play());
    CHECK(harness.shell.play_session() == nullptr);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play,
                 "a rigid body needs a collider on its entity or on an entity below it"));
    REQUIRE(scene.set_component(cube, ColliderComponent{}));
    // The sample's cube is scaled: a moving body needs unit scale, and the Inspector says so.
    CHECK(harness.shell.physics_note(cube, ComponentId::rigid_body).text.find("needs unit scale") != std::string::npos);
    CHECK_FALSE(harness.shell.start_play());
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play, "a dynamic body needs unit scale"));
    auto unscaled = std::get<TransformComponent>(*read_component(scene.world(), *scene.world().find(cube), ComponentId::transform));
    unscaled.scale = math::Vec3(1.0f);
    REQUIRE(scene.set_component(cube, unscaled));
    CHECK(harness.shell.physics_note(cube, ComponentId::rigid_body).text.empty());
    REQUIRE(harness.shell.start_play());
    CHECK(harness.shell.play_session()->physics().stats().dynamic_bodies == 1);
}

TEST_CASE("The physics sample plays the same in the player's path and in the editor", "[editor][physics][play]") {
    // The player's path: the scene file, straight into a play session.
    auto device = EditorDevice{};
    const auto project = open_project(sample_project());
    REQUIRE(project);
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device));
    REQUIRE(assets);
    const auto file = project.project.content_root / "physics.scene";
    auto loaded = load_scene_file(file, asset_property_context(*assets.registry));
    REQUIRE(loaded);
    auto player = PlaySession::start(std::move(loaded.document), asset_property_context(*assets.registry), builtin_systems());
    INFO(player.error);
    REQUIRE(player);
    // The editor's path: the same scene opened, then Play.
    Harness harness;
    harness.frames(2);
    REQUIRE(harness.shell.open_scene(file));
    harness.frames(2);
    REQUIRE(harness.shell.start_play());
    auto* editor = harness.shell.play_session();
    REQUIRE(editor);
    CHECK(editor->physics().stats().dynamic_bodies == 6);
    CHECK(editor->physics().stats().static_bodies == 1);
    while (editor->clock().tick() < 300) harness.frame();
    while (player.session->clock().tick() < editor->clock().tick()) player.session->update(1.0 / 60.0);
    CHECK(player.session->clock().tick() == editor->clock().tick());
    CHECK(text(player.session->world()) == text(editor->world()));
    // The crates came to rest on the floor instead of falling through it.
    auto& world = editor->world();
    world.for_each<NameComponent, TransformComponent>([&](EntityHandle, const NameComponent& name, const TransformComponent& pose) {
        if (name.value.starts_with("Crate") || name.value == "Falling crate") {
            INFO(name.value);
            CHECK(pose.translation.y > 0.4f);
            CHECK(pose.translation.y < 6.0f);
        }
    });
}
