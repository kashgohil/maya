// Editing a world in the editor (#1067, docs/editor.md#worlds), as a person would: create it, place content
// in several cells, move across a boundary, reference across cells, save, reopen part of it, and play.

#include "editor_harness.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/world_io.hpp"
#include <fstream>
#include <map>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
namespace fs = std::filesystem;

namespace {
constexpr auto cube_mesh = AssetId{0x6d617961, 2};
constexpr auto red = AssetId{0x6d617961, 0x11};
constexpr auto pointer_script = AssetId{0x6d617961, 0x3a};
/// A script whose one property is another entity: the cross-cell reference.
constexpr auto pointer = R"(local Pointer = { properties = { target = { type = "entity", label = "Target" } } }
return Pointer
)";

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
void type(Harness& harness, const std::string& text) {
    for (const auto c : text) harness.frame({TextEvent{uint32_t(c)}});
}
/// Frames until the editor's cells are what its camera wants.
void settle(Harness& harness) {
    for (int i = 0; i < 600; ++i) {
        harness.frame();
        const auto& stats = harness.shell.cells()->stats();
        if (stats.cells[size_t(EditorCellState::loading)] == 0 && stats.loads_in_flight == 0 && i > 2) return;
    }
    FAIL("the cells did not settle");
}
TransformComponent at(double x, double y, double z) { return {{x, y, z}, {}, math::Vec3(1.0f)}; }
EntityId named(SceneEditor& scene, const std::string& name) { return find_named(scene, name); }
/// Every entity of the world on disk, by name: the part its file is in.
std::map<std::string, std::string> parts_on_disk(const fs::path& file, const PropertyValidationContext& context) {
    auto loaded = load_world(file, context);
    INFO((loaded.diagnostics.empty() ? std::string() : loaded.diagnostics.front().message));
    REQUIRE(loaded);
    auto parts = std::map<std::string, std::string>{};
    const auto note = [&](const SceneDocument& part, const std::string& where) {
        for (const auto& entity : part.entities)
            for (const auto& value : entity.components)
                if (const auto* name = std::get_if<NameComponent>(&value)) parts[name->value] = where;
    };
    note(loaded.persistent, "persistent");
    for (const auto& [index, cell] : loaded.cells) note(cell, cell_name(index));
    return parts;
}
} // namespace

TEST_CASE("A world is created, filled across cells, linked across them, saved, reopened in part, and played", "[editor][worlds]") {
    const auto copy = ProjectCopy();
    std::ofstream(copy.content / "scripts/pointer.luau") << pointer;
    std::ofstream(copy.content / "catalog.maya", std::ios::app) << "script 6d617961 3a \"scripts/pointer.luau\"\n";
    const auto file = copy.content / "levels/fresh.world";
    auto saved = std::map<std::string, std::string>{};
    {
        Harness harness(false);
        REQUIRE(harness.shell.open_project(copy.folder));
        harness.frames(3);
        // Create: New world, named in its prompt.
        harness.shell.request_new_world();
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        type(harness, "levels/fresh"); // replaces the suggested name, selected as the prompt opens
        press(harness, control(harness, "dialog.save"));
        harness.frames(2);
        REQUIRE(harness.shell.cells());
        REQUIRE(harness.shell.scene_path() == fs::canonical(file));
        auto& scene = *harness.shell.scene();
        CHECK(scene.world().size() == 2); // its camera and sun, persistent
        // Place: a cube dropped in view (cell 0_0), and content in two other cells.
        harness.shell.camera() = EditorCamera::looking_at({0, 12, 30}, {10, 0, 10});
        harness.frames(2);
        drag(harness, control(harness, "asset.cube.obj"), on_screen(harness, {10.0, 0.0, 10.0}));
        const auto cube = *scene.primary();
        REQUIRE(scene.rename(cube, "Crate"));
        REQUIRE(scene.create("Tree", std::nullopt, {at(300, 0, 40), MeshRendererComponent{AssetRef<MeshAsset>{cube_mesh}, AssetRef<MaterialAsset>{red}, true}}));
        const auto tree = *scene.primary();
        REQUIRE(scene.create("Branch", tree, {at(0, 3, 0)}));
        REQUIRE(scene.create("Rock", std::nullopt, {at(20, 0, -350)}));
        const auto rock = *scene.primary();
        harness.frames(2);
        auto& cells = *harness.shell.cells();
        CHECK(cells.part_of_root(cube) == WorldPart(CellIndex{0, 0}));
        CHECK(cells.part_of_root(tree) == WorldPart(CellIndex{2, 0}));
        CHECK(cells.part_of_root(rock) == WorldPart(CellIndex{0, -3}));
        // Move the crate east across the boundary into 1_0, as the Inspector would.
        REQUIRE(scene.set_component(cube, at(140, 0, 10)));
        harness.frames(1);
        CHECK(cells.part_of_root(cube) == WorldPart(CellIndex{1, 0}));
        // A hierarchy may not leave its cell.
        CHECK_FALSE(scene.set_component(named(scene, "Branch"), at(-200, 3, 0)));
        // A lever in 0_0 points at the tree in 2_0: a cross-cell reference.
        REQUIRE(scene.create("Lever", std::nullopt, {at(5, 0, 5), ScriptComponent{AssetRef<ScriptAsset>{pointer_script},
                                                                                     {{"target", ScriptValueType::entity, tree}}}}));
        const auto lever = *scene.primary();
        chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
        harness.frames(2);
        CHECK_FALSE(scene.dirty());
        CHECK(cells.changed_cells().empty());
        // The camera goes far away: every cell leaves, saved; the lever's reference is unresolved.
        harness.shell.camera() = EditorCamera::looking_at({5000, 20, 5000}, {5000, 0, 4990});
        settle(harness);
        CHECK(cells.state({2, 0}) == EditorCellState::unloaded);
        CHECK_FALSE(scene.record(tree));
        // Back near the lever (0_0 loads, 2_0 does not), select it, and follow its reference from the Inspector.
        harness.shell.camera() = EditorCamera::looking_at({-60, 20, -60}, {5, 0, 5});
        auto& settings = cells.settings();
        settings.load_radius = 100.0;
        settle(harness);
        REQUIRE(cells.state({0, 0}) == EditorCellState::active);
        REQUIRE(cells.state({2, 0}) == EditorCellState::unloaded);
        for (int i = 0; i < 600 && !cells.index_ready(); ++i) harness.frame();
        scene.select(lever);
        harness.frames(2);
        press(harness, control(harness, "reference.follow.target"));
        harness.frames(2);
        CHECK(cells.state({2, 0}) == EditorCellState::active);
        REQUIRE(scene.primary());
        CHECK(*scene.primary() == tree);
        saved = parts_on_disk(file, asset_property_context(*harness.shell.assets()));
    }
    // The saved world, part by part, is the whole world as edited: a whole-world reference.
    CHECK(saved == std::map<std::string, std::string>{{"Camera", "persistent"}, {"Sun", "persistent"}, {"Crate", "1_0"},
                                                       {"Lever", "0_0"}, {"Tree", "2_0"}, {"Branch", "2_0"}, {"Rock", "0_-3"}});
    // Reopened, only the cells near the camera load; Play streams the rest, and Stop leaves the set as it was.
    Harness reopened(false);
    REQUIRE(reopened.shell.open_project(copy.folder));
    reopened.shell.camera() = EditorCamera::looking_at({64, 20, 64}, {64, 0, 0});
    REQUIRE(reopened.shell.open_scene("levels/fresh.world"));
    reopened.shell.cells()->settings().load_radius = 150.0;
    settle(reopened);
    auto& cells = *reopened.shell.cells();
    CHECK(cells.state({0, 0}) == EditorCellState::active);
    CHECK(cells.state({1, 0}) == EditorCellState::active);
    CHECK(cells.state({0, -3}) == EditorCellState::unloaded); // 320 m away
    CHECK(cells.state({2, 0}) == EditorCellState::unloaded);
    auto& scene = *reopened.shell.scene();
    CHECK_FALSE(scene.dirty());
    const auto loaded = scene.world().size();
    const auto lever = named(scene, "Lever");
    REQUIRE(reopened.shell.start_play());
    reopened.frames(10);
    REQUIRE(reopened.shell.play_streamer());
    CHECK(reopened.shell.play_streamer()->stats().cells[size_t(CellState::active)] > 0);
    CHECK(reopened.shell.play_session()->world().find(lever));
    reopened.shell.stop_play();
    reopened.frames(2);
    CHECK(scene.world().size() == loaded);
    CHECK(cells.state({0, -3}) == EditorCellState::unloaded);
    CHECK_FALSE(scene.dirty());
}

TEST_CASE("Undo and redo of a move across cells restore its cell, and whether each cell is saved", "[editor][worlds]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    REQUIRE(harness.shell.new_world("levels/moves.world"));
    auto& scene = *harness.shell.scene();
    auto& cells = *harness.shell.cells();
    REQUIRE(scene.create("Post", std::nullopt, {at(30, 0, 30)}));
    const auto post = *scene.primary();
    REQUIRE(harness.shell.save_scene().empty());
    harness.frames(1);
    CHECK(cells.changed_cells().empty());
    // Moved from 0_0 to -1_0, a cell the world did not have: both changed.
    REQUIRE(scene.set_component(post, at(-30, 0, 30)));
    harness.frames(1);
    CHECK(cells.part_of_root(post) == WorldPart(CellIndex{-1, 0}));
    CHECK(cells.changed_cells() == std::vector<CellIndex>{{-1, 0}, {0, 0}});
    CHECK(scene.dirty());
    // Undo: back in 0_0, and nothing changed since the save.
    REQUIRE(harness.shell.undo_edit());
    harness.frames(1);
    CHECK(cells.part_of_root(post) == WorldPart(CellIndex{0, 0}));
    CHECK(cells.changed_cells().empty());
    CHECK_FALSE(scene.dirty());
    // Redo: in -1_0 again, both changed; saved, the world lists -1_0 and not 0_0.
    REQUIRE(harness.shell.redo_edit());
    harness.frames(1);
    CHECK(cells.part_of_root(post) == WorldPart(CellIndex{-1, 0}));
    CHECK(cells.changed_cells() == std::vector<CellIndex>{{-1, 0}, {0, 0}});
    REQUIRE(harness.shell.save_scene().empty());
    CHECK(cells.document().cell({-1, 0}));
    CHECK_FALSE(cells.document().cell({0, 0}));
    // The camera goes far away and the saved cells leave. Undo loads the cell its step touches first, then
    // puts the post back where it was: changed again, and saving puts it there on disk.
    harness.shell.camera() = EditorCamera::looking_at({8000, 20, 8000}, {8000, 0, 7990});
    for (int i = 0; i < 200 && cells.state({-1, 0}) != EditorCellState::unloaded; ++i) harness.frame();
    REQUIRE(cells.state({-1, 0}) == EditorCellState::unloaded);
    REQUIRE_FALSE(scene.record(post));
    REQUIRE(harness.shell.undo_edit());
    CHECK(cells.state({-1, 0}) == EditorCellState::active);
    REQUIRE(scene.record(post));
    CHECK(cells.part_of_root(post) == WorldPart(CellIndex{0, 0}));
    harness.frames(1);
    CHECK(scene.dirty());
    CHECK(cells.changed_cells() == std::vector<CellIndex>{{-1, 0}, {0, 0}});
    REQUIRE(harness.shell.save_scene().empty());
    CHECK(cells.document().cell({0, 0}));
    CHECK_FALSE(cells.document().cell({-1, 0}));
}
