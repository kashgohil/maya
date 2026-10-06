// Importing models in the editor (#1036, docs/editor.md#importing-models): from the Scene menu, by dropping
// a file on the window (from inside the project or outside it), and by dragging an imported scene into
// the viewport; the Assets panel names imported parts by what the import file calls them.

#include "editor_harness.hpp"
#include "support/gltf.hpp"
#include <chrono>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
void write_props(const fs::path& folder) {
    fs::create_directories(folder / "textures");
    std::ofstream(folder / "props.gltf", std::ios::binary) << test::props_gltf();
    std::ofstream(folder / "textures/normal.png", std::ios::binary) << test::flat_normal_png();
}
void change_file(const fs::path& path, const std::string& text, int seconds) {
    std::ofstream(path, std::ios::binary) << text;
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(seconds));
}
/// The open scene's root entities with this name.
std::vector<EntityId> roots_named(SceneEditor& scene, std::string_view name) {
    auto found = std::vector<EntityId>{};
    for (const auto root : scene.roots())
        if (scene.display_name(root) == name) found.push_back(root);
    return found;
}
} // namespace

TEST_CASE("Importing a model from the project catalogs it, lists its scene, and names its parts", "[editor][import]") {
    const auto copy = ProjectCopy();
    write_props(copy.content / "models");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    // The Scene menu offers the project's glTF files.
    REQUIRE(std::ranges::count(harness.shell.model_files(), fs::path("models/props.gltf")) == 1);
    const auto before = harness.shell.assets()->records().size();
    const auto result = harness.shell.import_model("models/props.gltf", false);
    REQUIRE(result);
    harness.frames(2);
    // The registry was refreshed with the new entries, and the scene file is listed.
    CHECK(harness.shell.assets()->records().size() == before + result.records.size());
    CHECK(std::ranges::count(harness.shell.scene_files(), fs::path("models/props.scene")) == 1);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Imported models/props.gltf: 3 meshes, 2 textures, 3 materials"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "materials[0]: transforms its maps differently"));
    // Parts are named by the import file and are not reported missing.
    for (const auto& record : result.records) {
        INFO(record.path.generic_string());
        CHECK(std::ranges::count(harness.shell.missing_asset_files(), record.id) == 0);
    }
    const auto label = [&](std::string_view path) {
        const auto found = std::ranges::find_if(result.records, [&](const AssetRecord& r) { return r.path.generic_string() == path; });
        REQUIRE(found != result.records.end());
        return harness.shell.asset_label(*found);
    };
    CHECK(label("models/props.gltf#mesh/0/0") == "Panel");
    CHECK(label("models/props.gltf#mesh/0/1") == "Panel 1");
    CHECK(label("models/props.gltf#texture/1/normal") == "normal");
    CHECK(label("models/props/materials/Glow.material") == "Glow");
    // Reimporting from the menu keeps every ID.
    const auto again = harness.shell.import_model("models/props.gltf", false);
    REQUIRE(again);
    for (size_t i = 0; i < result.records.size(); ++i) CHECK(again.records[i].id == result.records[i].id);
}

TEST_CASE("Dropping a glTF file on the editor imports it and places it in the open scene, as one undo step", "[editor][import]") {
    const auto copy = ProjectCopy();
    write_props(copy.content / "models");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    auto& scene = *harness.shell.scene();
    harness.frames(2);
    const auto roots = scene.roots().size();
    harness.frame({FileDropEvent{{(copy.content / "models/props.gltf").string()}}});
    harness.frames(2);
    // A root named for the file, with the file's nodes below it, selected.
    const auto placed = roots_named(scene, "props");
    REQUIRE(placed.size() == 1);
    CHECK(scene.roots().size() == roots + 1);
    CHECK(scene.selection() == std::vector<EntityId>{placed[0]});
    REQUIRE(scene.children(placed[0]).size() == 1);
    const auto model_root = scene.children(placed[0])[0];
    CHECK(scene.display_name(model_root) == "Root");
    CHECK(scene.children(model_root).size() == 3); // Panel, Lamp, Eye
    CHECK(scene.undo_label() == "Place props");
    CHECK(harness.shell.extraction().skipped == 0);
    // Placing again gives new IDs and a new name; undo removes only the copy.
    harness.frame({FileDropEvent{{(copy.content / "models/props.gltf").string()}}});
    harness.frames(1);
    CHECK(roots_named(scene, "props (1)").size() == 1);
    REQUIRE(scene.undo());
    CHECK(roots_named(scene, "props (1)").empty());
    REQUIRE(scene.undo());
    CHECK(scene.roots().size() == roots);
}

TEST_CASE("A glTF file dropped from outside the project is copied into models/ with the files it names", "[editor][import]") {
    const auto copy = ProjectCopy();
    const auto outside = copy.root / "Downloads";
    write_props(outside);
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    harness.frame({FileDropEvent{{(outside / "props.gltf").string()}}});
    harness.frames(1);
    CHECK(fs::exists(copy.content / "models/props/props.gltf"));
    CHECK(fs::exists(copy.content / "models/props/textures/normal.png"));
    CHECK(fs::exists(copy.content / "models/props/props.scene"));
    CHECK(roots_named(*harness.shell.scene(), "props").size() == 1);
    // Dropping it again finds the copy there, and changes nothing.
    const auto catalog = copy.read("catalog.maya");
    harness.frame({FileDropEvent{{(outside / "props.gltf").string()}}});
    harness.frames(1);
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    CHECK(harness.shell.prompt_message().find("models/props/props.gltf already exists") != std::string::npos);
    CHECK(copy.read("catalog.maya") == catalog);
}

TEST_CASE("Files that are not glTF are not imported, and a broken glTF file changes nothing", "[editor][import]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    const auto catalog = copy.read("catalog.maya");
    fs::create_directories(copy.content / "models");
    copy.write("models/notes.txt", "hello");
    copy.write("models/broken.glb", "not a glb");
    harness.frame({FileDropEvent{{(copy.content / "models/notes.txt").string()}}});
    harness.frames(1);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Not imported: notes.txt is not a .gltf or .glb file"));
    harness.frame({FileDropEvent{{(copy.content / "models/broken.glb").string()}}});
    harness.frames(1);
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    CHECK(harness.shell.prompt_message().find("Nothing was changed") != std::string::npos);
    CHECK(copy.read("catalog.maya") == catalog);
}

TEST_CASE("An imported scene dragged from the Assets panel into the viewport is placed where it is dropped", "[editor][import]") {
    const auto copy = ProjectCopy();
    write_props(copy.content / "models");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    REQUIRE(harness.shell.import_model("models/props.gltf", false));
    auto& scene = *harness.shell.scene();
    harness.frames(2);
    filter_assets(harness, "props");
    drag(harness, control(harness, "scene.models/props.scene"), harness.viewport_center());
    harness.frames(1);
    const auto placed = roots_named(scene, "props");
    REQUIRE(placed.size() == 1);
    CHECK(scene.undo_label() == "Place props");
    // On the ground in front of the camera, where the pointer was.
    const auto* record = scene.record(placed[0]);
    REQUIRE(record);
    const auto transform = std::ranges::find_if(record->components, [](const ComponentValue& v) { return std::holds_alternative<TransformComponent>(v); });
    REQUIRE(transform != record->components.end());
    const auto expected = harness.shell.drop_point(harness.viewport_center());
    REQUIRE(expected);
    CHECK((std::get<TransformComponent>(*transform).translation - *expected).length() < 0.05f);
}

TEST_CASE("A changed imported file, or a file it names, is imported again by itself, and its open scene shows it", "[editor][import]") {
    const auto copy = ProjectCopy();
    write_props(copy.content / "models");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    REQUIRE(harness.shell.import_model("models/props.gltf", false));
    harness.shell.request_open_scene("models/props.scene");
    harness.frames(3);
    REQUIRE(harness.shell.scene_path() == copy.content / "models/props.scene");
    const auto has_entity = [&](std::string_view name) {
        auto& scene = *harness.shell.scene();
        auto found = false;
        scene.world().for_each<NameComponent>([&](EntityHandle, const NameComponent& value) { found |= value.value == name; });
        return found;
    };
    REQUIRE(has_entity("Lamp"));
    harness.shell.check_imported_sources(); // nothing changed
    CHECK_FALSE(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "changed; importing it again"));
    // Edited in the modelling program: the lamp renamed.
    auto text = test::props_gltf();
    text.replace(text.find(R"("name":"Lamp")"), 13, R"("name":"Light")");
    // Caught half-written first: an empty file waits.
    copy.write("models/props.gltf", "");
    fs::last_write_time(copy.content / "models/props.gltf", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    harness.shell.check_imported_sources();
    CHECK_FALSE(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "changed; importing it again"));
    std::ofstream(copy.content / "models/props.gltf", std::ios::binary) << text;
    fs::last_write_time(copy.content / "models/props.gltf", fs::file_time_type::clock::now() + std::chrono::seconds(7));
    harness.shell.check_imported_sources();
    harness.frames(2);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "models/props.gltf changed; importing it again"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "models/props.gltf: added entity /Root/Light"));
    CHECK(has_entity("Light")); // the open scene was the import's, unedited: shown again
    CHECK_FALSE(has_entity("Lamp"));
    // An image the file names changes: imported again too.
    std::ofstream(copy.content / "models/textures/normal.png", std::ios::binary) << test::encode_png_rgba(1, 1, {128, 128, 255, 255});
    fs::last_write_time(copy.content / "models/textures/normal.png", fs::file_time_type::clock::now() + std::chrono::seconds(9));
    const auto imports = [&] {
        size_t count = 0;
        for (const auto& entry : harness.shell.diagnostics().entries())
            if (entry.message.find("changed; importing it again") != std::string::npos) count += entry.count;
        return count;
    };
    const auto before = imports();
    harness.shell.check_imported_sources();
    CHECK(imports() == before + 1);
    CHECK(harness.shell.prompt() != EditorPrompt::notice);
    // Its import file's settings edited by hand: imported again with them.
    auto settings = copy.read("models/props.gltf.import");
    settings.replace(settings.find("scale 1\n"), 8, "scale 0.5\n");
    change_file(copy.content / "models/props.gltf.import", settings, 11);
    harness.shell.check_imported_sources();
    harness.frames(2);
    CHECK(imports() == before + 2);
    CHECK(copy.read("models/props.scene").find("0.5") != std::string::npos);
}

namespace {
/// The open scene's entity with this name.
std::optional<EntityId> entity_named(SceneEditor& scene, std::string_view name) {
    auto found = std::optional<EntityId>{};
    scene.world().for_each<NameComponent>([&](EntityHandle entity, const NameComponent& value) {
        if (value.value == name) found = *scene.world().persistent_id(entity);
    });
    return found;
}
template<class T> std::optional<T> component_of(SceneEditor& scene, EntityId id) {
    const auto* record = scene.record(id);
    if (!record) return std::nullopt;
    for (const auto& value : record->components)
        if (const auto* typed = std::get_if<T>(&value)) return *typed;
    return std::nullopt;
}
} // namespace

TEST_CASE("A scene that places, moves, and overrides imported content keeps it all through a reimport that changes geometry", "[editor][import]") {
    const auto copy = ProjectCopy();
    write_props(copy.content / "models");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    const auto imported = harness.shell.import_model("models/props.gltf", false);
    REQUIRE(imported);
    auto* scene = harness.shell.scene();
    harness.frames(2);
    // Placed, moved, and with the tile's material overridden by one of the project's own; then saved.
    REQUIRE(harness.shell.place_scene("models/props.scene", math::Vec3{0.0f}));
    const auto placed = *entity_named(*scene, "props");
    REQUIRE(scene->set_component(placed, TransformComponent{{3, 0, -2}, {}, math::Vec3{1.0f}}));
    const auto tile = *entity_named(*scene, "Tile");
    const auto red = AssetId{0x6d617961, 0x11}; // materials/red.material
    const auto assigned = harness.shell.assign_asset(tile, red);
    INFO(assigned.error);
    REQUIRE(assigned);
    const auto tile_mesh = component_of<MeshRendererComponent>(*scene, tile)->mesh.id;
    REQUIRE(harness.shell.save_scene().empty());
    harness.frames(2);
    REQUIRE(harness.shell.extraction().skipped == 0);
    // The modelling program makes the quads twice the size: only geometry changes.
    change_file(copy.content / "models/props.gltf", test::props_gltf("", 2.0f), 5);
    harness.shell.check_imported_sources();
    harness.frames(2);
    const auto check = [&](SceneEditor& open) {
        const auto tile_now = entity_named(open, "Tile");
        REQUIRE(tile_now == tile);
        const auto renderer = component_of<MeshRendererComponent>(open, tile);
        REQUIRE(renderer);
        CHECK(renderer->mesh.id == tile_mesh); // the same asset, with new geometry
        CHECK(renderer->material.id == red); // the override stays
        CHECK(component_of<TransformComponent>(open, placed)->translation.x == 3);
        const auto mesh = harness.shell.assets()->acquire(AssetRef<MeshAsset>{tile_mesh});
        REQUIRE(mesh);
        CHECK(mesh.lease.value().geometry().max.x == 2.0f);
        CHECK(harness.shell.extraction().skipped == 0);
    };
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "models/props.gltf changed; importing it again"));
    CHECK_FALSE(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "removed"));
    check(*harness.shell.scene());
    // And from its file, opened again.
    harness.shell.request_open_scene("basic.scene");
    harness.shell.open_scene("basic.scene");
    harness.frames(2);
    check(*harness.shell.scene());
}

TEST_CASE("A source changed while playing reloads into the running session's later frames", "[editor][import]") {
    const auto copy = ProjectCopy();
    write_props(copy.content / "models");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    const auto imported = harness.shell.import_model("models/props.gltf", false);
    REQUIRE(imported);
    REQUIRE(harness.shell.place_scene("models/props.scene", math::Vec3{0.0f}));
    const auto normal = std::ranges::find_if(imported.records, [](const AssetRecord& r) {
        return r.path.generic_string() == "models/props.gltf#texture/1/normal";
    });
    REQUIRE(normal != imported.records.end());
    REQUIRE(harness.shell.start_play());
    harness.frames(3);
    const auto width = [&] {
        const auto texture = harness.shell.assets()->acquire(AssetRef<TextureAsset>{normal->id});
        REQUIRE(texture);
        return texture.lease.value().texture().desc().width;
    };
    REQUIRE(width() == 2);
    // The normal map, named by the source, replaced by a 1x1 one: the source is imported again, and the
    // session's next frames draw with the new texture.
    change_file(copy.content / "models/textures/normal.png", test::encode_png_rgba(1, 1, {128, 128, 255, 255}), 5);
    harness.frames(20); // the editor checks files every quarter second
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "models/props.gltf changed; importing it again"));
    REQUIRE(harness.shell.play_session());
    CHECK(width() == 1);
    CHECK(harness.shell.extraction().skipped == 0);
    harness.shell.stop_play();
}
