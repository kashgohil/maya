// Generated content in the editor (#1093, docs/editor.md#generated-content): a scatter generator's items,
// previewed live, regenerated on jobs when it changes (superseded remakes cancelled), undone, overridden,
// removed, and baked.

#include "editor_harness.hpp"
#include "maya/assets/generator_file.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <set>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
constexpr auto rocks = AssetId{0x6d617961, 0x93};

ImVec2 centre(const EditorLayout::Field* rect) {
    REQUIRE(rect);
    return {(rect->min.x + rect->max.x) / 2, (rect->min.y + rect->max.y) / 2};
}
/// Frames until scatter has made and put in everything the camera wants.
void settle(Harness& harness, bool some = true) {
    for (int i = 0; i < 600; ++i) {
        harness.frame();
        const auto* scatter = harness.shell.scatter();
        const auto* cells = harness.shell.cells(); // in a world, its cells first: items wait for them
        const auto cells_done = !cells || (cells->stats().cells[size_t(EditorCellState::loading)] == 0 && cells->stats().loads_in_flight == 0);
        if (scatter && cells_done && (!some || scatter->stats().items > 0) && scatter->stats().cells_waiting == 0) return;
    }
    const auto& stats = harness.shell.scatter()->stats();
    FAIL("scatter did not settle: " << stats.items << " items, " << stats.cells_loading << " loading, " << stats.regenerating << " regenerating, "
                                     << stats.cells_active << " active; " << stats.last_error);
}
std::set<EntityId> generated(SceneEditor& scene) {
    auto ids = std::set<EntityId>{};
    scene.world().for_each<GeneratedComponent>([&](EntityHandle entity, const GeneratedComponent&) {
        const auto id = *scene.world().persistent_id(entity);
        if (scene.transient(id)) ids.insert(id);
    });
    return ids;
}
} // namespace

TEST_CASE("A scatter generator previews, regenerates, cancels, undoes, and bakes in the editor, and its overrides are saved", "[editor][scatter]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    REQUIRE(harness.shell.open_scene(copy.content / "terrain.scene"));
    harness.shell.camera() = EditorCamera({0.0, 80.0, 90.0}, 0.0f, -0.6f);
    settle(harness);
    auto& scene = *harness.shell.scene();
    // The preview: the rocks are in the scene as generated items, not saved and not in the history.
    const auto first = generated(scene);
    REQUIRE(first.size() > 50);
    CHECK(scene.transient_count() == first.size());
    CHECK_FALSE(scene.dirty());
    CHECK_FALSE(scene.can_undo());
    CHECK(scene.document().entities.size() == 6); // the scene file's entities alone
    const auto text = copy.read("terrain.scene");

    // Regenerate: the generator edited in the Inspector (from the Assets panel), its items made again on jobs.
    filter_assets(harness, "rocks");
    press(harness, centre(harness.shell.layout().control("asset.terrain/rocks.generator")));
    harness.frames(2);
    REQUIRE(harness.shell.layout().control("generator.stats"));
    const auto from = centre(harness.shell.layout().field("generator.density"));
    drag(harness, from, {from.x - 6.0f, from.y}, 6); // sparser
    REQUIRE(scene.generator(rocks));
    const auto sparser = *scene.generator(rocks);
    CHECK(sparser.density < 0.3f);
    CHECK(sparser.density > 0.0f);
    CHECK(scene.undo_label() == "Edit rocks"); // the drag is one step
    settle(harness);
    const auto second = generated(scene);
    CHECK(second.size() < first.size());
    for (const auto id : second) CHECK(first.contains(id)); // the survivors keep their IDs
    // Cancel: a change superseded before it is made is cancelled; only the last is shown.
    const auto cancelled = harness.shell.scatter()->stats().cancelled;
    auto denser = sparser;
    denser.density = 0.6f;
    REQUIRE(scene.set_generator(rocks, denser, "Edit rocks"));
    harness.frame();
    denser.density = 0.5f;
    REQUIRE(scene.set_generator(rocks, denser, "Edit rocks"));
    harness.frame();
    settle(harness);
    CHECK(harness.shell.scatter()->stats().cancelled > cancelled);
    const auto third = generated(scene);
    CHECK(third.size() > first.size());
    // Undo, twice and once more: the items are the first ones again.
    REQUIRE(harness.shell.undo_edit());
    REQUIRE(harness.shell.undo_edit());
    REQUIRE(harness.shell.undo_edit());
    settle(harness);
    CHECK(generated(scene) == first);
    CHECK_FALSE(scene.dirty());
    // A new seed, from its button: other places for the same squares' IDs.
    press(harness, centre(harness.shell.layout().control("generator.seed")));
    settle(harness);
    CHECK(scene.generator(rocks)->seed != 1093);
    REQUIRE(harness.shell.undo_edit());
    settle(harness);
    CHECK(generated(scene) == first);

    // Override: an item moved is authored, saved with the scene, and the generator does not make it again.
    const auto moved = *first.begin();
    const auto pose = scene.world().world_matrix(*scene.world().find(moved));
    REQUIRE(scene.set_component(moved, TransformComponent{pose->translation + math::DVec3{0.0, 2.0, 0.0}, {}, math::Vec3(1.0f)}));
    CHECK_FALSE(scene.transient(moved));
    CHECK_FALSE(harness.shell.scatter()->made(moved));
    // Removed: an item deleted leaves a marker; bake: an item selected and baked loses its marker.
    const auto gone = *std::next(first.begin());
    scene.select(gone);
    REQUIRE(scene.delete_selection());
    REQUIRE(scene.record(gone));
    CHECK(std::get<GeneratedComponent>(*find_component(*scene.record(gone), ComponentId::generated)).removed);
    CHECK(scene.world().find(gone));
    CHECK_FALSE(find_component(*scene.record(gone), ComponentId::mesh_renderer)); // nothing shown in its place
    const auto baked = *std::next(first.begin(), 2);
    scene.select(baked);
    harness.frames(2);
    press(harness, centre(harness.shell.layout().control("generated.bake")));
    CHECK_FALSE(find_component(*scene.record(baked), ComponentId::generated));
    CHECK_FALSE(scene.transient(baked));
    CHECK(scene.undo_label() == "Bake rocks");
    // Undoing the bake makes it generated again; redoing bakes it.
    REQUIRE(harness.shell.undo_edit());
    CHECK(scene.transient(baked));
    CHECK(harness.shell.scatter()->made(baked));
    REQUIRE(harness.shell.redo_edit());
    CHECK_FALSE(scene.transient(baked));
    // Saved: the three, and nothing else of the generator's, are in the scene file.
    REQUIRE(harness.shell.save_scene().empty());
    auto loaded = load_scene_file(copy.content / "terrain.scene", asset_property_context(*harness.shell.assets()));
    REQUIRE(loaded);
    CHECK(loaded.document.entities.size() == 6 + 3);
    for (const auto id : {moved, gone, baked})
        CHECK(std::ranges::any_of(loaded.document.entities, [&](const SceneEntity& entity) { return entity.id == id; }));
    // Reopened: the overrides stand for their items, each ID once.
    REQUIRE(harness.shell.open_scene(copy.content / "terrain.scene"));
    settle(harness);
    auto& reopened = *harness.shell.scene();
    const auto again = generated(reopened);
    CHECK(again.size() == first.size() - 3);
    for (const auto id : {moved, gone, baked}) {
        CHECK_FALSE(again.contains(id));
        CHECK(reopened.record(id));
    }
    // Deleting the removed marker brings the item back; undoing that removes it again: the marker is authored
    // (saved), not an edit of the generated item.
    reopened.select(gone);
    REQUIRE(reopened.delete_selection());
    settle(harness);
    CHECK(generated(reopened).contains(gone));
    CHECK(reopened.transient(gone));
    REQUIRE(harness.shell.undo_edit());
    CHECK_FALSE(reopened.transient(gone));
    CHECK(std::get<GeneratedComponent>(*find_component(*reopened.record(gone), ComponentId::generated)).removed);
    CHECK(std::ranges::any_of(reopened.document().entities, [&](const SceneEntity& entity) { return entity.id == gone; }));
    REQUIRE(harness.shell.redo_edit());
    settle(harness);
    CHECK(reopened.transient(gone));
    // Play scatters too, in the play World, around the overrides.
    REQUIRE(harness.shell.start_play());
    harness.frames(3);
    auto* play = harness.shell.play_session();
    REQUIRE(play);
    CHECK(play->world().find(moved));
    CHECK(play->world().find(gone));
    harness.shell.stop_play();
    CHECK(text != copy.read("terrain.scene"));
}

TEST_CASE("In a world, scatter waits for each cell, and an override is saved in its cell and streams with it", "[editor][scatter][worlds]") {
    const auto copy = ProjectCopy();
    // A scatter generator on flat ground over four cells.
    auto pebbles = GeneratorAsset{};
    pebbles.kind = GeneratorKind::scatter;
    pebbles.version = scatter_version;
    pebbles.seed = 5;
    pebbles.base = 0.0f;
    pebbles.region_min_x = pebbles.region_min_z = 0;
    pebbles.region_max_x = pebbles.region_max_z = 1;
    pebbles.spacing = 16.0f;
    pebbles.density = 0.5f;
    pebbles.mesh0 = AssetRef<MeshAsset>{{0x6d617961, 2}};
    pebbles.material0 = AssetRef<MaterialAsset>{{0x6d617961, 0x11}};
    std::ofstream(copy.content / "pebbles.generator") << write_generator_file(pebbles);
    std::ofstream(copy.content / "catalog.maya", std::ios::app) << "generator 6d617961 3b \"pebbles.generator\"\n";
    const auto generator = AssetRef<GeneratorAsset>{{0x6d617961, 0x3b}};
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    harness.shell.camera() = EditorCamera::looking_at({128, 60, 128}, {128, 0, 100});
    REQUIRE(harness.shell.new_world("levels/pebbles.world"));
    auto& scene = *harness.shell.scene();
    REQUIRE(scene.create("Pebbles", std::nullopt, {GeneratorComponent{generator}}));
    harness.shell.cells()->settings().load_radius = 300.0;
    settle(harness);
    // The generator is persistent; its items wait for their cells (none on disk yet: ready at once).
    CHECK(harness.shell.cells()->part_of_root(find_named(scene, "Pebbles")) == std::nullopt);
    const auto items = generated(scene);
    REQUIRE(items.size() > 20);
    harness.frames(2);
    CHECK(harness.shell.cells()->changed_cells().empty()); // generated items belong to no cell until edited
    // An item of cell 1_0 moved: an override, saved in that cell's file.
    auto item = EntityId{};
    for (const auto id : items) {
        const auto pose = scene.world().world_matrix(*scene.world().find(id));
        if (cell_of(pose->translation) == CellIndex{1, 0}) item = id;
    }
    REQUIRE(item.valid());
    const auto pose = scene.world().world_matrix(*scene.world().find(item));
    REQUIRE(scene.set_component(item, TransformComponent{pose->translation + math::DVec3{0.0, 1.0, 0.0}, {}, math::Vec3(1.0f)}));
    harness.frames(1);
    CHECK(harness.shell.cells()->part_of_root(item) == WorldPart(CellIndex{1, 0}));
    CHECK(harness.shell.cells()->changed({1, 0}));
    // Undone, it is generated again, in no cell: nothing changed. Redone, the override again.
    REQUIRE(harness.shell.undo_edit());
    harness.frames(1);
    CHECK(scene.transient(item));
    CHECK(harness.shell.cells()->changed_cells().empty());
    REQUIRE(harness.shell.redo_edit());
    harness.frames(1);
    CHECK(harness.shell.cells()->changed({1, 0}));
    REQUIRE(harness.shell.save_scene().empty());
    auto cell_file = std::ifstream(copy.content / "levels/pebbles/cells/1_0.scene");
    const auto cell_text = std::string(std::istreambuf_iterator<char>(cell_file), {});
    CHECK(cell_text.find("maya.generated") != std::string::npos);
    auto entities = size_t{0};
    for (auto at = cell_text.find("\nentity "); at != std::string::npos; at = cell_text.find("\nentity ", at + 1)) ++entities;
    CHECK(entities + (cell_text.starts_with("entity ") ? 1 : 0) == 1); // the override alone, none of the generated items
    auto persistent_file = std::ifstream(copy.content / "levels/pebbles/persistent.scene");
    CHECK(std::string(std::istreambuf_iterator<char>(persistent_file), {}).find("maya.generated") == std::string::npos);
    // The camera leaves: the cell and its items go, the override with them.
    harness.shell.camera() = EditorCamera::looking_at({9000, 60, 9000}, {9000, 0, 8990});
    for (int i = 0; i < 300 && (scene.record(item) || harness.shell.scatter()->stats().items > 0); ++i) harness.frame();
    CHECK_FALSE(scene.record(item));
    CHECK(harness.shell.scatter()->stats().items == 0);
    // Back: the cell comes in, then its items; the override stands for its item, which is not made.
    harness.shell.camera() = EditorCamera::looking_at({128, 60, 128}, {128, 0, 100});
    settle(harness);
    REQUIRE(scene.record(item));
    CHECK_FALSE(scene.transient(item));
    CHECK_FALSE(harness.shell.scatter()->made(item));
    CHECK(generated(scene).size() == items.size() - 1);
}
