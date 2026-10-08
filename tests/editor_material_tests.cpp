// Material editing in the editor (#1033, docs/editor.md#materials): the Inspector edits material assets
// through the property system, in the scene's undo history; edits show at once and are saved with ⌘S.

#include "editor_harness.hpp"
#include "maya/assets/material_file.hpp"
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
constexpr auto red = AssetId{0x6d617961, 0x11};
constexpr auto grid = AssetId{0x6d617961, 0x50}, grid_normal = AssetId{0x6d617961, 0x51};

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
ImVec2 field(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().field(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
/// What the registry serves now: what the renderer extracts next frame.
MaterialAsset served(Harness& harness, AssetId id) {
    const auto lease = harness.shell.assets()->acquire(AssetRef<MaterialAsset>{id});
    REQUIRE(lease);
    return lease.lease.value();
}
MaterialAsset on_disk(const ProjectCopy& copy, const std::string& path) {
    auto input = std::istringstream(copy.read(path));
    const auto read = read_material_file(input);
    INFO(read.error);
    REQUIRE(read);
    return read.material;
}
/// Opens the copy's basic scene with the red cube selected and its material unfolded in the Inspector.
SceneEditor& red_cube_material(Harness& harness, const ProjectCopy& copy) {
    REQUIRE(harness.shell.open_project(copy.folder));
    auto& scene = *harness.shell.scene();
    scene.select(find_named(scene, "Red cube"));
    harness.frames(2);
    CHECK_FALSE(harness.shell.layout().field("material.roughness")); // folded at first
    press(harness, control(harness, "material.fold"));
    harness.frames(2);
    REQUIRE(harness.shell.layout().field("material.roughness"));
    return scene;
}
} // namespace

TEST_CASE("A material edited in the Inspector shows at once, undoes in the scene's history, and saves with ⌘S", "[editor][materials]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    auto& scene = red_cube_material(harness, copy);
    const auto file = copy.read("materials/red.material");
    CHECK(file.starts_with("maya-material 1\n")); // a sample still at version 1
    const auto original = on_disk(copy, "materials/red.material");
    CHECK_FALSE(scene.dirty());

    // A scene edit, then a material edit: one history.
    const auto pyramid = find_named(scene, "Pyramid");
    REQUIRE(scene.rename(pyramid, "Peak"));
    const auto from = field(harness, "material.roughness");
    drag(harness, from, {from.x + 60.0f, from.y}, 10);
    const auto edited = *scene.material(red);
    CHECK(edited.roughness > original.roughness);
    CHECK(served(harness, red) == edited); // published for the next extraction
    CHECK(scene.undo_label() == "Edit red"); // the drag is one step
    CHECK(scene.dirty_materials() == std::vector{red});
    CHECK(copy.read("materials/red.material") == file); // nothing written yet

    REQUIRE(scene.undo());
    CHECK(*scene.material(red) == original);
    CHECK(served(harness, red) == original);
    CHECK(scene.undo_label() == "Rename");
    REQUIRE(scene.undo());
    CHECK(scene.display_name(pyramid) == "Pyramid");
    REQUIRE(scene.redo());
    REQUIRE(scene.redo());
    CHECK(served(harness, red) == edited);
    CHECK(scene.dirty());

    // ⌘S writes the scene and the material, migrating it to version 2.
    chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
    CHECK_FALSE(scene.dirty());
    CHECK(scene.dirty_materials().empty());
    CHECK(copy.read("materials/red.material").starts_with("maya-material 2\n"));
    CHECK(on_disk(copy, "materials/red.material") == edited);
    CHECK(copy.read("basic.scene").find("Peak") != std::string::npos);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Saved materials/red.material"));
    // Undoing past the save makes it unsaved again; redoing returns to the saved state.
    REQUIRE(scene.undo());
    CHECK(scene.dirty_materials() == std::vector{red});
    REQUIRE(scene.redo());
    CHECK_FALSE(scene.dirty());
}

TEST_CASE("Material edits are refused while playing, like scene edits", "[editor][materials]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    auto& scene = red_cube_material(harness, copy);
    harness.shell.start_play();
    harness.frames(2);
    auto rougher = *scene.material(red);
    rougher.roughness = 0.9f;
    CHECK_FALSE(scene.set_material(red, rougher, "Edit red"));
    harness.shell.stop_play();
    harness.frames(1);
    CHECK(scene.set_material(red, rougher, "Edit red"));
}

TEST_CASE("Unsaved material edits ask before closing, and are put back from their files when discarded", "[editor][materials]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    auto& scene = red_cube_material(harness, copy);
    const auto original = on_disk(copy, "materials/red.material");
    auto shiny = *scene.material(red);
    shiny.metallic = 1.0f;
    REQUIRE(scene.set_material(red, shiny, "Edit red"));
    REQUIRE(scene.dirty());

    SECTION("Closing asks") {
        CHECK_FALSE(harness.shell.request_close());
        harness.frames(2);
        CHECK(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        press(harness, control(harness, "dialog.discard"));
        CHECK(harness.close_requests == 1);
    }
    SECTION("Opening another scene without saving restores the material everywhere") {
        harness.shell.request_open_scene("physics.scene");
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::unsaved_changes);
        press(harness, control(harness, "dialog.discard"));
        CHECK(harness.shell.scene_path() == copy.content / "physics.scene");
        CHECK(served(harness, red) == original);
        CHECK(on_disk(copy, "materials/red.material") == original);
    }
    SECTION("Saving from the prompt writes the material") {
        harness.shell.request_open_scene("physics.scene");
        harness.frames(2);
        press(harness, control(harness, "dialog.save"));
        CHECK(on_disk(copy, "materials/red.material") == shiny);
        CHECK(served(harness, red) == shiny);
    }
}

TEST_CASE("A changed material file reloads into the next frames, and an unsaved edit outlasts the reload", "[editor][materials]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    auto& scene = red_cube_material(harness, copy);
    const auto reload = [&] {
        press(harness, control(harness, "asset.materials/red.material"), MouseButton::right);
        harness.frames(1);
        press(harness, control(harness, "asset.reload"));
        harness.frames(1);
    };
    // Unedited, the file's new value replaces the editor's.
    copy.write("materials/red.material", "maya-material 2\nbase_color 0 1 0\nroughness 0.2\n");
    reload();
    const auto green = *scene.material(red);
    CHECK(green.base_color.y == 1.0f);
    CHECK(served(harness, red) == green);
    CHECK_FALSE(scene.dirty());
    // Edited, the edit stays shown and unsaved; the file's value is what it differs from.
    auto edited = *scene.material(red);
    edited.metallic = 0.75f;
    REQUIRE(scene.set_material(red, edited, "Edit red"));
    copy.write("materials/red.material", "maya-material 2\nbase_color 0 0 1\n");
    reload();
    CHECK(served(harness, red) == edited);
    CHECK(scene.dirty_materials() == std::vector{red});
    // Undo returns to the value before the edit, which now differs from the file.
    REQUIRE(scene.undo());
    CHECK(served(harness, red) == green);
    CHECK(scene.dirty_materials() == std::vector{red});
    CHECK(scene.dirty()); // back at the saved history position, but not at the file's value
}

TEST_CASE("Textures dragged from the Assets panel fill a material's map slots, and a map of the wrong kind is flagged", "[editor][materials]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    auto& scene = *harness.shell.scene();
    harness.frames(3); // the textures load, a frame each
    // Choosing a material in the Assets panel shows it in the Inspector, unfolded.
    press(harness, control(harness, "asset.materials/red.material"));
    harness.frames(2);
    REQUIRE(harness.shell.layout().field("material.base_color_texture"));
    drag(harness, control(harness, "asset.textures/grid.texture"), field(harness, "material.base_color_texture"));
    harness.frames(1);
    REQUIRE(scene.material(red));
    CHECK(scene.material(red)->base_color_texture.id == grid);
    CHECK(served(harness, red).base_color_texture.id == grid);
    CHECK(scene.undo_label() == "Edit red");
    // A color texture in the normal slot is accepted, flagged here, and drawn as the placeholder.
    drag(harness, control(harness, "asset.textures/grid.texture"), field(harness, "material.normal_texture"));
    harness.frames(2);
    CHECK(scene.material(red)->normal_texture.id == grid);
    CHECK(harness.shell.layout().control("material.warning.normal_texture"));
    drag(harness, control(harness, "asset.textures/grid_normal.texture"), field(harness, "material.normal_texture"));
    harness.frames(2);
    CHECK(scene.material(red)->normal_texture.id == grid_normal);
    CHECK_FALSE(harness.shell.layout().control("material.warning.normal_texture"));
    // A mesh is not a texture: dropping one changes nothing.
    const auto before = *scene.material(red);
    drag(harness, control(harness, "asset.cube.obj"), field(harness, "material.base_color_texture"));
    harness.frames(1);
    CHECK(*scene.material(red) == before);
    // Selecting an object brings its components back.
    scene.select(find_named(scene, "Pyramid"));
    harness.frames(2);
    CHECK_FALSE(harness.shell.layout().field("material.base_color_texture"));
    CHECK(harness.shell.layout().control("material.fold"));
}

namespace {
/// Rewrites a copied project's file and moves its time on, so a rewrite within the same second is seen.
void rewrite(const ProjectCopy& copy, const std::string& path, const std::string& text) {
    static auto ahead = 0;
    copy.write(path, text);
    fs::last_write_time(copy.content / path, fs::file_time_type::clock::now() + std::chrono::seconds(++ahead));
}
/// How many times the asset log said `text`: repeated messages are merged into one entry with a count.
size_t logged_count(Harness& harness, std::string_view text) {
    auto total = size_t{0};
    for (const auto& entry : harness.shell.diagnostics().entries())
        if (entry.source == DiagnosticSource::asset && entry.message.find(text) != std::string::npos) total += entry.count;
    return total;
}
} // namespace

TEST_CASE("A material file changed outside the editor reloads by itself, keeping unsaved edits and the last good version", "[editor][materials]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    auto& scene = red_cube_material(harness, copy);
    harness.check_files(); // the loaded material's file, as it was loaded

    // Changed outside the editor: the registry serves the file's value, and the editor follows it.
    rewrite(copy, "materials/red.material", "maya-material 2\nbase_color 0 1 0\nroughness 0.2\n");
    harness.check_files();
    const auto green = on_disk(copy, "materials/red.material");
    CHECK(served(harness, red) == green);
    CHECK(*scene.material(red) == green);
    CHECK_FALSE(scene.dirty());
    CHECK(logged_count(harness, "Reloaded materials/red.material") == 1);
    harness.check_files(); // unchanged: nothing more
    CHECK(logged_count(harness, "Reloaded materials/red.material") == 1);

    // The editor checks by itself, a few times a second.
    rewrite(copy, "materials/red.material", "maya-material 2\nbase_color 0 0 1\n");
    harness.frames(20);
    CHECK(served(harness, red).base_color.z == 1.0f);

    // A file caught half-written fails once, keeps the last good version, and loads when it is whole.
    const auto blue = served(harness, red);
    rewrite(copy, "materials/red.material", "maya-material 2\nbase_col");
    harness.check_files();
    CHECK(served(harness, red) == blue);
    CHECK(logged_count(harness, "the last version stays in use") == 1);
    rewrite(copy, "materials/red.material", "maya-material 2\nbase_col"); // touched, still broken
    harness.check_files();
    CHECK(logged_count(harness, "the last version stays in use") == 1); // the same problem, reported once
    rewrite(copy, "materials/red.material", "maya-material 2\nbase_color 1 1 0\n");
    harness.check_files();
    CHECK(served(harness, red).base_color.y == 1.0f);
    CHECK(logged_count(harness, "which loads again") == 1);

    // An unsaved edit outlasts an outside change, and stays unsaved against the file's new value.
    auto edited = *scene.material(red);
    edited.metallic = 0.75f;
    REQUIRE(scene.set_material(red, edited, "Edit red"));
    rewrite(copy, "materials/red.material", "maya-material 2\nbase_color 1 0 1\n");
    harness.check_files();
    CHECK(served(harness, red) == edited);
    CHECK(scene.dirty_materials() == std::vector{red});

    // The editor's own save is not an outside change: no reload follows it.
    const auto reloads = logged_count(harness, "Reloaded materials/red.material");
    chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
    CHECK_FALSE(scene.dirty());
    harness.check_files();
    harness.frames(20);
    CHECK(logged_count(harness, "Reloaded materials/red.material") == reloads);
    CHECK(served(harness, red) == edited);

    // A deleted file keeps the last version in use, and is reported.
    fs::remove(copy.content / "materials/red.material");
    harness.check_files();
    CHECK(served(harness, red) == edited);
    CHECK(logged_count(harness, "the last version stays in use") == 2);
}
