#include "editor_harness.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/assets/registry.hpp"
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;

namespace {
const auto cube_mesh = AssetId{0x6d617961, 2};
const auto red_material = AssetId{0x6d617961, 0x11};
const auto blue_material = AssetId{0x6d617961, 0x12};

ImVec2 center(const EditorLayout::Field& field) { return {(field.min.x + field.max.x) / 2, (field.min.y + field.max.y) / 2}; }
/// The center of a control or inspector field.
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    if (!found) found = harness.shell.layout().field(key);
    auto shown = std::string{};
    for (const auto& control : harness.shell.layout().controls) shown += control.key + " ";
    INFO(key << " among: " << shown);
    REQUIRE(found);
    return center(*found);
}
void type(Harness& harness, const std::string& text) {
    for (const auto c : text) harness.frame({TextEvent{uint32_t(c)}});
}
void enter(Harness& harness) {
    harness.frame(key(KeyCode::Enter, true));
    harness.frame(key(KeyCode::Enter, false));
}
void double_click(Harness& harness, ImVec2 at) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    for (int i = 0; i < 2; ++i) {
        harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
        harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    }
}
/// Canonical text of the open scene as it would be saved: IDs, root order, hierarchy, and values.
std::string saved_text(Harness& harness) {
    auto out = std::ostringstream{};
    REQUIRE(write_scene(out, harness.shell.scene()->document(), asset_property_context(*harness.shell.assets())).empty());
    return out.str();
}
const MeshRendererComponent& renderer_of(SceneEditor& scene, EntityId id) {
    const auto* record = scene.record(id);
    REQUIRE(record);
    for (const auto& value : record->components)
        if (const auto* renderer = std::get_if<MeshRendererComponent>(&value)) return *renderer;
    FAIL("no mesh renderer");
    throw;
}
} // namespace

TEST_CASE("A scene is authored from project assets, saved, and reopened from another working directory", "[editor][project]") {
    const auto copy = ProjectCopy("content"); // a configured content root, not the sample's "assets"
    auto expected = std::string{};
    {
        // Opened by a path relative to where the editor starts, outside the checkout.
        const auto cwd = WorkingDirectory(copy.root);
        Harness harness(false);
        REQUIRE(harness.shell.open_project("Sample Game"));
        REQUIRE(harness.shell.project());
        CHECK(harness.shell.project()->content_root == copy.content);
        CHECK(harness.shell.scene_path() == copy.content / "basic.scene");
        CHECK(harness.shell.scene_files() == std::vector<fs::path>{"basic.scene", "physics.scene", "v1_reference.scene"});
        harness.frames(3);

        // ⌘N: the open scene has no changes, so it is replaced at once by a camera and a light.
        chord(harness, {KeyCode::LeftSuper}, KeyCode::N);
        auto* scene = harness.shell.scene();
        REQUIRE(scene);
        CHECK(harness.shell.prompt() == EditorPrompt::none);
        CHECK(harness.shell.scene_path().empty());
        CHECK(scene->world().size() == 2);
        CHECK_FALSE(scene->dirty());
        harness.frames(2);

        // Drag the cube from the Assets panel onto the ground at the origin: it rests on it.
        drag(harness, control(harness, "asset.cube.obj"), on_screen(harness, {0.0f, 0.0f, 0.0f}));
        REQUIRE(scene->world().size() == 3);
        const auto cube = *scene->primary();
        CHECK(scene->display_name(cube) == "cube");
        CHECK(renderer_of(*scene, cube).mesh.id == cube_mesh);
        const auto placed = std::get<TransformComponent>(
            *read_component(scene->world(), *scene->world().find(cube), ComponentId::transform));
        // The pointer lands on whole points, a few millimetres from the exact origin at this distance.
        CHECK(placed.translation.x == Approx(0.0f).margin(0.02));
        CHECK(placed.translation.y == Approx(0.5f).margin(1e-4)); // the cube reaches 0.5 below its origin
        CHECK(placed.translation.z == Approx(0.0f).margin(0.02));
        CHECK(scene->undo_label() == "Create cube");
        CHECK(scene->dirty());
        harness.frames(2); // render it, so the viewport can find it under the pointer

        // Drop the red material onto the cube in the viewport.
        drag(harness, control(harness, "asset.materials/red.material"), on_screen(harness, {0.3f, 0.8f, 0.3f}));
        CHECK(renderer_of(*scene, cube).material.id == red_material);
        CHECK(scene->undo_label() == "Edit Mesh renderer");

        // Edit a value, duplicate, and give the copy the blue material by dropping it on its hierarchy row.
        auto moved = placed;
        moved.translation.x = -1.5f;
        REQUIRE(scene->set_component(cube, moved));
        chord(harness, {KeyCode::LeftSuper}, KeyCode::D);
        const auto twin = *scene->primary();
        CHECK(scene->display_name(twin) == "cube (1)");
        harness.frames(1);
        drag(harness, control(harness, "asset.materials/blue_metal.material"), row_center(harness, twin));
        CHECK(renderer_of(*scene, twin).material.id == blue_material);
        CHECK(renderer_of(*scene, twin).mesh.id == cube_mesh);
        CHECK(renderer_of(*scene, cube).material.id == red_material);

        // A mesh dropped on the hierarchy's empty space is placed in view.
        const auto before = scene->world().size();
        const auto* last = harness.shell.layout().row(scene->roots().back());
        REQUIRE(last);
        drag(harness, control(harness, "asset.cube.obj"), {last->min.x + 40.0f, last->max.y + 60.0f});
        REQUIRE(scene->world().size() == before + 1);
        CHECK(scene->display_name(*scene->primary()) == "cube");
        REQUIRE(scene->undo());
        CHECK(*scene->primary() == twin);

        // Double-clicking a mesh places another in view; the pyramid then goes first among the roots.
        double_click(harness, control(harness, "asset.pyramid.obj"));
        const auto pyramid = *scene->primary();
        CHECK(scene->display_name(pyramid) == "pyramid");
        REQUIRE(scene->move(pyramid, scene->roots().front(), Placement::before));
        CHECK(scene->roots().front() == pyramid);

        // ⌘S on a new scene asks for a path; the extension and the folder are added.
        chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
        harness.frames(1);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        type(harness, "levels/workshop");
        enter(harness);
        CHECK(harness.shell.prompt() == EditorPrompt::none);
        CHECK(harness.shell.scene_path() == copy.content / "levels/workshop.scene");
        CHECK_FALSE(scene->dirty());
        CHECK(harness.shell.scene_files() == std::vector<fs::path>{"basic.scene", "levels/workshop.scene", "physics.scene", "v1_reference.scene"});
        harness.frames(1);
        CHECK(harness.shell.layout().control("scene.levels/workshop.scene")); // listed in the Assets panel
        expected = saved_text(harness);
        CHECK(copy.read("levels/workshop.scene") == expected);
        // References are catalog IDs, never file paths.
        CHECK(expected.find("mesh 6d617961 2") != std::string::npos);
        CHECK(expected.find("material 6d617961 11") != std::string::npos);
        CHECK(expected.find(".obj") == std::string::npos);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::scene, "Saved levels/workshop.scene"));
    }

    // A different working directory; the project is found from its own file.
    const auto cwd = WorkingDirectory(fs::temp_directory_path());
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder / "project.maya"));
    REQUIRE(harness.shell.open_scene("levels/workshop.scene"));
    CHECK(saved_text(harness) == expected); // same IDs, values, hierarchy, and root order
    auto& scene = *harness.shell.scene();
    CHECK(scene.display_name(scene.roots().front()) == "pyramid");
    CHECK_FALSE(scene.can_undo());
    harness.frames(3);
    CHECK(harness.shell.extraction().mesh_renderers == 3);
    CHECK(harness.shell.extraction().skipped == 0);
    // Saving again without changes writes the same bytes.
    CHECK(harness.shell.save_scene().empty());
    CHECK(copy.read("levels/workshop.scene") == expected);
}

TEST_CASE("A scene without unsaved changes is replaced or closed without asking", "[editor][project]") {
    const auto copy = ProjectCopy();
    copy.write("other.scene", copy.read("basic.scene"));
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(3);
    CHECK(harness.shell.request_close());
    harness.shell.request_open_scene("other.scene");
    CHECK(harness.shell.prompt() == EditorPrompt::none);
    CHECK(harness.shell.scene_path().filename() == "other.scene");
    // Undoing back to the saved state counts as no changes.
    auto& scene = *harness.shell.scene();
    REQUIRE(scene.rename(scene.roots().front(), "Renamed"));
    CHECK_FALSE(harness.shell.request_close());
    REQUIRE(scene.undo());
    CHECK(harness.shell.request_close());
    CHECK(harness.close_requests == 0);
}

TEST_CASE("Unsaved changes are never discarded without asking", "[editor][project]") {
    const auto copy = ProjectCopy();
    copy.write("other.scene", copy.read("basic.scene"));
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(3);
    const auto original = copy.read("basic.scene");

    auto* first = harness.shell.scene();
    REQUIRE(first->rename(first->roots().front(), "Renamed"));
    REQUIRE(first->dirty());

    SECTION("Opening another scene asks; Cancel and Escape keep everything") {
        double_click(harness, control(harness, "scene.other.scene"));
        harness.frames(1);
        REQUIRE(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        press(harness, control(harness, "dialog.cancel"));
        CHECK(harness.shell.prompt() == EditorPrompt::none);
        CHECK(harness.shell.scene() == first);
        CHECK(first->dirty());
        harness.shell.request_open_scene("other.scene");
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        harness.frame(key(KeyCode::Escape, true));
        harness.frame(key(KeyCode::Escape, false));
        CHECK(harness.shell.prompt() == EditorPrompt::none);
        CHECK(harness.shell.scene() == first);
        CHECK(copy.read("basic.scene") == original);
    }
    SECTION("Don't save opens the other scene and leaves the file as it was") {
        harness.shell.request_open_scene("other.scene");
        harness.frames(2);
        press(harness, control(harness, "dialog.discard"));
        CHECK(harness.shell.scene_path() == copy.content / "other.scene");
        CHECK_FALSE(harness.shell.scene()->dirty());
        CHECK(copy.read("basic.scene") == original);
    }
    SECTION("Save writes the changes, then opens the other scene") {
        harness.shell.request_open_scene("other.scene");
        harness.frames(2);
        press(harness, control(harness, "dialog.save"));
        CHECK(harness.shell.scene_path() == copy.content / "other.scene");
        CHECK(copy.read("basic.scene").find("Renamed") != std::string::npos);
    }
    SECTION("A failed save keeps the prompt, the scene, and its changes") {
        ProjectCopy::read_only(copy.content);
        harness.shell.request_open_scene("other.scene");
        harness.frames(2);
        press(harness, control(harness, "dialog.save"));
        ProjectCopy::writable(copy.content);
        CHECK(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        CHECK(harness.shell.prompt_message().starts_with("Couldn't save: "));
        CHECK(harness.shell.scene() == first);
        CHECK(first->dirty());
        CHECK(copy.read("basic.scene") == original);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::scene, "Save failed"));
    }
    SECTION("⌘N asks before replacing the scene") {
        chord(harness, {KeyCode::LeftSuper}, KeyCode::N);
        harness.frames(1);
        REQUIRE(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        press(harness, control(harness, "dialog.discard"));
        CHECK(harness.shell.scene_path().empty());
        CHECK(harness.shell.scene()->world().size() == 2);
    }
    SECTION("Closing asks, and closes once the changes are discarded") {
        CHECK_FALSE(harness.shell.request_close());
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        CHECK_FALSE(harness.shell.request_close()); // asking again while the prompt is up changes nothing
        press(harness, control(harness, "dialog.discard"));
        CHECK(harness.close_requests == 1);
        CHECK(harness.shell.request_close());
        CHECK(copy.read("basic.scene") == original);
    }
    SECTION("Closing supersedes an open prompt") {
        harness.shell.request_open_scene("other.scene");
        harness.frames(2);
        CHECK_FALSE(harness.shell.request_close());
        press(harness, control(harness, "dialog.save"));
        CHECK(harness.close_requests == 1);
        CHECK(harness.shell.scene_path().filename() == "basic.scene"); // it closes instead of opening
        CHECK(copy.read("basic.scene").find("Renamed") != std::string::npos);
    }
    SECTION("Closing with a never-saved scene asks for a path, saves, then closes") {
        REQUIRE(harness.shell.new_scene());
        REQUIRE(harness.shell.scene()->create("Thing"));
        CHECK_FALSE(harness.shell.request_close());
        harness.frames(2);
        press(harness, control(harness, "dialog.save"));
        harness.frames(2); // a dialog is measured, hidden, in its first frame
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        CHECK(harness.close_requests == 0);
        type(harness, "closing");
        enter(harness);
        CHECK(harness.close_requests == 1);
        CHECK(copy.read("closing.scene").find("Thing") != std::string::npos);
        CHECK(harness.shell.request_close());
    }
    SECTION("Cancelling the path cancels the close") {
        REQUIRE(harness.shell.new_scene());
        REQUIRE(harness.shell.scene()->create("Thing"));
        CHECK_FALSE(harness.shell.request_close());
        harness.frames(2);
        press(harness, control(harness, "dialog.save"));
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        press(harness, control(harness, "dialog.cancel"));
        CHECK(harness.shell.prompt() == EditorPrompt::none);
        CHECK(harness.close_requests == 0);
        CHECK_FALSE(harness.shell.request_close());
    }
}

TEST_CASE("Failed saves and loads explain why and change nothing", "[editor][project]") {
    const auto copy = ProjectCopy();
    copy.write("other.scene", copy.read("basic.scene"));
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(3);
    auto* scene = harness.shell.scene();
    const auto original = copy.read("basic.scene");
    REQUIRE(scene->rename(scene->roots().front(), "Renamed"));

    SECTION("Save as refuses paths outside the content folder") {
        chord(harness, {KeyCode::LeftSuper, KeyCode::LeftShift}, KeyCode::S);
        harness.frames(1);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        type(harness, "../escape");
        enter(harness);
        CHECK(harness.shell.prompt() == EditorPrompt::save_as);
        CHECK(harness.shell.prompt_message().find("inside the project's content folder") != std::string::npos);
        CHECK_FALSE(fs::exists(copy.folder / "escape.scene"));
        CHECK(scene->dirty());
        // The same rules hold without the dialog, and for files that are not scenes.
        CHECK(harness.shell.save_scene("../escape.scene").find("outside the project's content root") != std::string::npos);
        CHECK(harness.shell.save_scene(copy.root / "escape.scene").find("outside the project's content root") != std::string::npos);
        CHECK(harness.shell.save_scene("notes.txt") == "Scene files must end in .scene");
        fs::create_directory(copy.content / "folder.scene");
        CHECK(harness.shell.save_scene("folder.scene") == "folder.scene is a folder");
        CHECK_FALSE(fs::exists(copy.root / "escape.scene"));
        CHECK_FALSE(fs::exists(copy.content / "notes.txt"));
    }
    SECTION("Replacing another existing file takes a second Save") {
        chord(harness, {KeyCode::LeftSuper, KeyCode::LeftShift}, KeyCode::S);
        harness.frames(1);
        type(harness, "other.scene");
        enter(harness);
        CHECK(harness.shell.prompt() == EditorPrompt::save_as);
        CHECK(harness.shell.prompt_message() == "other.scene already exists. Save again to replace it.");
        CHECK(copy.read("other.scene") == original);
        press(harness, control(harness, "dialog.save"));
        CHECK(harness.shell.prompt() == EditorPrompt::none);
        CHECK(copy.read("other.scene").find("Renamed") != std::string::npos);
        CHECK(harness.shell.scene_path() == copy.content / "other.scene");
        CHECK(copy.read("basic.scene") == original);
    }
    SECTION("A folder that cannot be written") {
        ProjectCopy::read_only(copy.content);
        chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
        ProjectCopy::writable(copy.content);
        harness.frames(1);
        CHECK(harness.shell.prompt() == EditorPrompt::notice);
        CHECK_FALSE(harness.shell.prompt_message().empty());
        CHECK(scene->dirty());
        CHECK(copy.read("basic.scene") == original);
        press(harness, control(harness, "dialog.ok"));
        CHECK(harness.shell.prompt() == EditorPrompt::none);
    }
    SECTION("A reference to an asset that is not in the catalog") {
        // Edits cannot introduce one...
        const auto ground = find_named(*scene, "Ground");
        auto renderer = renderer_of(*scene, ground);
        renderer.material = {AssetId{0xdead, 1}};
        CHECK(scene->set_component(ground, renderer).error == "Asset ID is not registered in this project");
        // ...but the catalog can lose one. The scene still opens and edits, but is not saved.
        auto catalog = copy.read("catalog.maya");
        copy.write("catalog.maya", catalog.erase(catalog.find("material 6d617961 11"), 46));
        harness.shell.refresh_project();
        REQUIRE(harness.shell.assets()->records().size() == 5);
        const auto error = harness.shell.save_scene();
        INFO(error);
        CHECK(error.find("6d617961:11") != std::string::npos);
        CHECK(scene->dirty());
        CHECK(copy.read("basic.scene") == original);
    }
    SECTION("Unreadable scenes, unknown assets, missing files, and paths outside the project") {
        copy.write("broken.scene", "maya-scene 1\n\nentity 6d617961 1\n  component maya.name 1\n    value 12\nend\n");
        copy.write("unknown.scene", std::string(original).replace(original.find("mesh 6d617961 1"), 15, "mesh 6d617961 99"));
        for (const auto& [path, reason] : std::vector<std::pair<fs::path, std::string>>{
                 {"broken.scene", "line 5"}, {"unknown.scene", "6d617961:99"}, {"nope.scene", "nope.scene"},
                 {"../project.maya", "outside the project's content root"}}) {
            INFO(path);
            CHECK_FALSE(harness.shell.open_scene(path));
            CHECK(harness.shell.prompt() == EditorPrompt::notice);
            CHECK(harness.shell.prompt_message().find(reason) != std::string::npos);
            CHECK(harness.shell.scene() == scene); // the open scene and its changes stay
        }
        harness.frames(2);
        CHECK(harness.shell.prompt() == EditorPrompt::notice);
        CHECK(scene->dirty());
    }
}

TEST_CASE("Missing asset files leave the project usable and say what is missing", "[editor][project]") {
    const auto copy = ProjectCopy();
    fs::remove(copy.content / "cube.obj");
    fs::remove(copy.content / "materials/red.material");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    CHECK(harness.shell.scene()->world().size() == 6);
    CHECK(harness.shell.missing_asset_files() == std::vector{cube_mesh, red_material});
    harness.frames(3);
    CHECK(harness.shell.extraction().skipped == 3); // the two cubes and the ground use cube.obj
    const auto red = harness.shell.assets()->info(red_material);
    REQUIRE(red);
    CHECK(red->state == AssetState::failed);
    CHECK(red->diagnostic.message.find("red.material") != std::string::npos);

    // Restored files, a new catalog entry, and a new scene appear on refresh.
    fs::copy(sample_project().parent_path() / "assets/cube.obj", copy.content / "cube.obj");
    fs::copy(sample_project().parent_path() / "assets/cube.obj", copy.content / "crate.obj");
    fs::copy(sample_project().parent_path() / "assets/materials/red.material", copy.content / "materials/red.material");
    copy.write("catalog.maya", copy.read("catalog.maya") + "mesh 6d617961 3 \"crate.obj\"\n");
    copy.write("empty.scene", "maya-scene 1\n");
    harness.shell.refresh_project();
    CHECK(harness.shell.missing_asset_files().empty());
    CHECK(harness.shell.assets()->records().size() == 7);
    CHECK(harness.shell.scene_files() == std::vector<fs::path>{"basic.scene", "empty.scene", "physics.scene", "v1_reference.scene"});
    harness.frames(3);
    CHECK(harness.shell.extraction().skipped == 0);
    CHECK(harness.shell.assets()->info(red_material)->state == AssetState::ready);

    // A broken catalog is reported and the previous one stays in use.
    copy.write("catalog.maya", "maya-assets 1\nmesh nonsense\n");
    harness.shell.refresh_project();
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    CHECK(harness.shell.assets()->records().size() == 7);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::project, "catalog.maya"));
}

TEST_CASE("A project whose startup scene cannot be opened starts with a new scene", "[editor][project]") {
    const auto copy = ProjectCopy("assets", "missing.scene");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    CHECK(harness.shell.scene_path().empty());
    CHECK(harness.shell.scene()->world().size() == 2);
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    CHECK(harness.shell.prompt_message().find("missing.scene") != std::string::npos);
    harness.frames(2);
    press(harness, control(harness, "dialog.ok"));
    CHECK(harness.shell.prompt() == EditorPrompt::none);

    // Without a startup scene, a project also opens with a new one, and no notice.
    const auto plain = ProjectCopy("assets", "");
    Harness second(false);
    REQUIRE(second.shell.open_project(plain.folder));
    CHECK(second.shell.scene_path().empty());
    CHECK(second.shell.prompt() == EditorPrompt::none);
    // A project that cannot be opened keeps the one that is open.
    CHECK_FALSE(second.shell.open_project(plain.root / "nowhere"));
    CHECK(second.shell.project()->file == plain.folder / "project.maya");
    CHECK(second.shell.prompt() == EditorPrompt::notice);
}

TEST_CASE("Inspector asset fields take catalog assets, including dropped ones", "[editor][project]") {
    Harness harness;
    auto& scene = *harness.shell.scene();
    const auto red_cube = find_named(scene, "Red cube");
    scene.select(red_cube);
    harness.frames(3);
    // A mesh on a material field is refused; a material is taken, as one undo step.
    drag(harness, control(harness, "asset.cube.obj"), control(harness, "mesh_renderer.material"));
    CHECK(renderer_of(scene, red_cube).material.id == red_material);
    CHECK(harness.shell.edit_error().empty());
    drag(harness, control(harness, "asset.materials/blue_metal.material"), control(harness, "mesh_renderer.material"));
    CHECK(renderer_of(scene, red_cube).material.id == blue_material);
    CHECK(scene.undo_label() == "Edit Mesh renderer");
    REQUIRE(scene.undo());
    CHECK(renderer_of(scene, red_cube).material.id == red_material);
    // Asset references are checked against the catalog, so a known asset of the right kind is accepted
    // and an unknown one is refused with a reason.
    auto renderer = renderer_of(scene, red_cube);
    renderer.mesh = {AssetId{0x6d617961, 1}};
    CHECK(scene.set_component(red_cube, renderer));
    renderer.mesh = {red_material};
    CHECK(scene.set_component(red_cube, renderer).error == "Asset catalog kind does not match the property");
}
