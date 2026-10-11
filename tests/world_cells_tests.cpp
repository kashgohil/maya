// Editing a world's cells (#1067, docs/editor.md#worlds): the editor's cells come and go with its camera
// outside the history, stay while changed, and save only what changed.

#include "world_cells.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/scene/world_io.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace maya;
using namespace maya::editor;
namespace fs = std::filesystem;

namespace {
const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
constexpr double size = 128.0;
EntityId id(uint64_t high, uint64_t low) { return {0xed00000000000000ull | high, low}; }
TransformComponent at(double x, double y, double z) { return {{x, y, z}, {}, math::Vec3(1.0f)}; }
uint64_t key(int x, int z) { return uint64_t(uint32_t(x + 1000)) << 16 | uint32_t(z + 1000); }

/// A 5 × 5-cell world around the origin: a camera and a sun persistent, and in each cell two trees, each
/// with a branch.
SceneDocument grid() {
    auto scene = SceneDocument{};
    scene.entities.push_back({id(0, 1), std::nullopt, {NameComponent{"Camera"}, at(0, 2, 0), CameraComponent{}}});
    scene.entities.push_back({id(0, 2), std::nullopt, {NameComponent{"Sun"}, at(0, 10, 0), LightComponent{}}});
    for (int x = -2; x <= 2; ++x)
        for (int z = -2; z <= 2; ++z)
            for (uint64_t i = 0; i < 2; ++i) {
                const auto tree = id(key(x, z), 10 + i * 2);
                scene.entities.push_back({tree, std::nullopt, {NameComponent{"Tree " + cell_name({x, z}) + "/" + std::to_string(i)},
                                                               at(x * size + 30 + double(i) * 50, 0, z * size + 40)}});
                scene.entities.push_back({id(key(x, z), 11 + i * 2), tree, {NameComponent{"Branch"}, at(0, 3, 1)}});
            }
    return scene;
}

struct Opened {
    fs::path folder, file;
    std::unique_ptr<SceneEditor> scene;
    std::unique_ptr<EditorCells> cells;
    Opened() {
        static std::atomic<int> counter{0};
        folder = fs::temp_directory_path() / ("maya-world-cells-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(folder / "levels");
        file = folder / "levels/grid.world";
        REQUIRE(save_world(file, grid(), any_asset).empty());
        open();
    }
    ~Opened() {
        cells.reset();
        scene.reset();
        auto error = std::error_code{};
        fs::remove_all(folder, error);
    }
    /// As the editor opens a world: its persistent part as the scene, the cells streamed around the camera.
    void open() {
        cells.reset();
        scene.reset();
        auto text = std::ifstream(file);
        auto read = read_world(std::string(std::istreambuf_iterator<char>(text), {}));
        REQUIRE(read);
        auto persistent = load_scene_file(file.parent_path() / read.document->persistent, any_asset);
        REQUIRE(persistent);
        auto built = instantiate_scene(persistent.document, any_asset);
        REQUIRE(built);
        scene = std::make_unique<SceneEditor>(std::move(built.world));
        scene->set_validation_context(any_asset);
        auto settings = EditorCellsSettings{};
        settings.load_radius = 100.0;
        settings.hysteresis = 20.0;
        cells = std::make_unique<EditorCells>(*scene, file, *read.document,
                                              cooked_cell_loader(file.parent_path(), std::make_shared<CookCache>(folder / "cache"), any_asset),
                                              any_asset, settings);
    }
    /// The world on disk, whole, as text: what a reload would see.
    std::string disk() const {
        auto loaded = load_world(file, any_asset);
        REQUIRE(loaded);
        auto whole = loaded.whole();
        std::ranges::sort(whole.entities, {}, &SceneEntity::id);
        auto out = std::ostringstream{};
        write_scene(out, whole, any_asset);
        return out.str();
    }
};
size_t active(const EditorCells& cells) { return cells.stats().cells[size_t(EditorCellState::active)]; }
} // namespace

TEST_CASE("The editor's cells follow its camera, outside the history, and leave the scene unchanged", "[editor][worlds]") {
    Opened world;
    auto& scene = *world.scene;
    auto& cells = *world.cells;
    CHECK(scene.world().size() == 2); // the persistent part alone
    cells.set_source({64, 0, 64}); // the middle of cell 0_0
    cells.settle();
    // Within 100 m of the middle of 0_0: it and all eight neighbours (the diagonal ones start 64 √2 = 90.5 m away).
    CHECK(active(cells) == 9);
    CHECK(cells.state({0, 0}) == EditorCellState::active);
    CHECK(cells.state({1, 1}) == EditorCellState::active);
    CHECK(cells.state({2, 0}) == EditorCellState::unloaded);
    CHECK(scene.world().size() == 2 + 9 * 4);
    CHECK(scene.record(id(key(1, -1), 13))->parent == id(key(1, -1), 12));
    CHECK_FALSE(scene.dirty());
    CHECK_FALSE(scene.can_undo());
    CHECK(cells.changed_cells().empty());
    CHECK_FALSE(cells.persistent_changed());
    // The camera moves two cells east: the west column goes, the next comes.
    cells.set_source({64 + 2 * size, 0, 64});
    cells.settle();
    CHECK(cells.state({-1, 0}) == EditorCellState::unloaded);
    CHECK(cells.state({2, 0}) == EditorCellState::active);
    CHECK_FALSE(scene.record(id(key(-1, 0), 10)));
    CHECK(scene.world().size() == 2 + 6 * 4); // columns 1 and 2: there is no column 3
    CHECK_FALSE(scene.dirty());
    // Once every cell has been read, where each entity is is known, loaded or not.
    for (int i = 0; i < 1000 && !cells.index_ready(); ++i) cells.update();
    REQUIRE(cells.index_ready());
    REQUIRE(cells.find(id(key(-2, -2), 11)));
    CHECK(cells.find(id(key(-2, -2), 11))->part == WorldPart(CellIndex{-2, -2}));
    CHECK(cells.find(id(0, 1))->part == std::nullopt);
    const auto there = cells.unloaded_entities({-2, -2});
    CHECK(there.size() == 4);
}

TEST_CASE("A changed cell stays loaded until saved, and saving writes it and nothing else", "[editor][worlds]") {
    Opened world;
    auto& scene = *world.scene;
    auto& cells = *world.cells;
    cells.set_source({64, 0, 64});
    cells.settle();
    const auto before = world.disk();
    const auto untouched = world.file.parent_path() / cells.document().cell({1, 0})->scene;
    const auto untouched_time = fs::last_write_time(untouched);
    REQUIRE(scene.rename(id(key(0, 0), 10), "Oak"));
    cells.update();
    CHECK(cells.changed({0, 0}));
    CHECK(cells.changed_cells() == std::vector<CellIndex>{{0, 0}});
    CHECK_FALSE(cells.persistent_changed());
    // Undoing back to what is saved is unchanged again; redoing, changed.
    REQUIRE(scene.undo());
    cells.update();
    CHECK_FALSE(cells.changed({0, 0}));
    REQUIRE(scene.redo());
    cells.update();
    CHECK(cells.changed({0, 0}));
    // The camera leaves: the changed cell stays, and cannot be unloaded by hand.
    cells.set_source({64 + 10 * size, 0, 64});
    cells.settle();
    CHECK(cells.state({0, 0}) == EditorCellState::active);
    CHECK(cells.state({1, 0}) == EditorCellState::unloaded);
    CHECK_FALSE(cells.hold({0, 0}, CellHold::unloaded).empty());
    // Saved: that cell and the persistent part are written; the rest keep their files.
    REQUIRE(cells.save().empty());
    scene.mark_saved();
    CHECK_FALSE(cells.changed({0, 0}));
    CHECK((fs::last_write_time(untouched) == untouched_time));
    CHECK(world.disk() != before);
    CHECK(world.disk().find("Oak") != std::string::npos);
    cells.settle();
    CHECK(cells.state({0, 0}) == EditorCellState::unloaded); // saved: free to go
    CHECK(scene.world().size() == 2);
}

TEST_CASE("A root moved into an unloaded cell brings that cell in; the saved world is the whole world as edited", "[editor][worlds]") {
    Opened world;
    auto& scene = *world.scene;
    auto& cells = *world.cells;
    cells.set_source({64, 0, 64});
    cells.settle();
    // A tree of 0_0 moved east into 2_0, which is not loaded: 2_0 comes in, with its own trees.
    const auto tree = id(key(0, 0), 10);
    REQUIRE(cells.state({2, 0}) == EditorCellState::unloaded);
    REQUIRE(scene.set_component(tree, at(2 * size + 10, 0, 40)));
    cells.update();
    CHECK(cells.state({2, 0}) == EditorCellState::active);
    CHECK(scene.record(id(key(2, 0), 10))); // its own content is here
    CHECK(cells.part_of_root(tree) == WorldPart(CellIndex{2, 0}));
    CHECK(cells.changed({0, 0}));
    CHECK(cells.changed({2, 0}));
    CHECK(cells.find(id(key(0, 0), 11))->part == WorldPart(CellIndex{2, 0})); // its branch went with it
    // A tree moved where no cell was: a new cell.
    REQUIRE(scene.set_component(id(key(0, 0), 12), at(9 * size + 1, 0, 1)));
    cells.update();
    CHECK(cells.changed({9, 0}));
    const auto loaded_untouched = world.file.parent_path() / cells.document().cell({1, 1})->scene;
    REQUIRE(cells.state({1, 1}) == EditorCellState::active);
    const auto loaded_untouched_time = fs::last_write_time(loaded_untouched);
    REQUIRE(cells.save().empty());
    CHECK((fs::last_write_time(loaded_untouched) == loaded_untouched_time)); // loaded, but unchanged: not written
    // What is on disk is the whole world with those two moves: a whole-world reference.
    auto expected = grid();
    for (auto& entity : expected.entities)
        if (entity.id == tree) std::get<TransformComponent>(entity.components[1]) = at(2 * size + 10, 0, 40);
        else if (entity.id == id(key(0, 0), 12)) std::get<TransformComponent>(entity.components[1]) = at(9 * size + 1, 0, 1);
    std::ranges::sort(expected.entities, {}, &SceneEntity::id);
    auto out = std::ostringstream{};
    write_scene(out, expected, any_asset);
    CHECK(world.disk() == out.str());
    REQUIRE(cells.document().cell({9, 0}));
    CHECK(cells.document().cell({9, 0})->entities == 2);
    CHECK_FALSE(cells.document().cell({0, 0})); // both of its trees left: no longer a cell of the world
}

TEST_CASE("A hierarchy that would leave its cell is refused, and saying why", "[editor][worlds]") {
    Opened world;
    auto& scene = *world.scene;
    auto& cells = *world.cells;
    cells.set_source({64, 0, 64});
    cells.settle();
    const auto branch = id(key(0, 0), 11);
    const auto moved = scene.set_component(branch, at(200, 3, 1)); // 230 m east of its tree: cell 1_0
    CHECK_FALSE(moved);
    CHECK(moved.error == "'Branch' would be in cell 1_0, but its root 'Tree 0_0/0' is in cell 0_0: a hierarchy stays in one cell");
    CHECK(std::get<TransformComponent>(*find_component(*scene.record(branch), ComponentId::transform)).translation.x == 0.0);
    CHECK_FALSE(scene.can_undo());
    // Within the cell it is fine; the persistent part has no cells.
    CHECK(scene.set_component(branch, at(20, 3, 1)));
    CHECK(scene.set_component(id(0, 2), at(5000, 10, 0)));
}

TEST_CASE("Undo reaches a saved cell that has unloaded by loading it first", "[editor][worlds]") {
    Opened world;
    auto& scene = *world.scene;
    auto& cells = *world.cells;
    cells.set_source({64, 0, 64});
    cells.settle();
    const auto tree = id(key(1, 1), 10);
    REQUIRE(scene.rename(tree, "Birch"));
    REQUIRE(cells.save().empty());
    scene.mark_saved();
    cells.set_source({64 + 20 * size, 0, 64});
    cells.settle();
    REQUIRE(cells.state({1, 1}) == EditorCellState::unloaded);
    REQUIRE_FALSE(scene.record(tree));
    // What the shell does before undoing: the cells the step touches, in.
    REQUIRE(scene.undo_change());
    REQUIRE(cells.load_for(*scene.undo_change()));
    CHECK(cells.state({1, 1}) == EditorCellState::active);
    REQUIRE(scene.undo());
    cells.update();
    CHECK(scene.display_name(tree) == "Tree 1_1/0");
    CHECK(cells.changed({1, 1}));
    CHECK(scene.dirty());
}

TEST_CASE("Play gets the world as edited: changed cells from the editor, the rest from disk", "[editor][worlds]") {
    Opened world;
    auto& scene = *world.scene;
    auto& cells = *world.cells;
    cells.set_source({64, 0, 64});
    cells.settle();
    REQUIRE(scene.rename(id(key(0, 0), 10), "Edited"));
    REQUIRE(scene.create("New", std::nullopt, {at(5 * size + 3, 0, 3)})); // a cell the world does not have
    cells.update();
    const auto disk = cooked_cell_loader(world.file.parent_path(), nullptr, any_asset);
    const auto play = cells.play_world(disk);
    CHECK(play.persistent.entities.size() == 2);
    REQUIRE(play.document.cell({5, 0}));
    CHECK(play.document.cell({5, 0})->entities == 1);
    JobScope jobs;
    auto loaded = CellLoad{};
    jobs.submit(JobTier::frame, [&](JobContext& job) { loaded = play.loader(*play.document.cell({0, 0}), job); });
    jobs.wait();
    REQUIRE(loaded.document);
    CHECK(std::ranges::any_of(loaded.document->entities, [](const SceneEntity& entity) {
        return std::ranges::any_of(entity.components, [](const ComponentValue& value) {
            const auto* name = std::get_if<NameComponent>(&value);
            return name && name->value == "Edited";
        });
    }));
    jobs.submit(JobTier::frame, [&](JobContext& job) { loaded = play.loader(*play.document.cell({-2, 2}), job); });
    jobs.wait();
    REQUIRE(loaded.document);
    CHECK(loaded.document->entities.size() == 4); // from disk
    // Nothing was saved.
    CHECK(cells.changed({0, 0}));
}

TEST_CASE("Editing a large world: costs of loading, idle frames, edits, drags, saves, and crossings", "[.][world-benchmark]") {
    // 20 × 20 cells of 250 roots with a child each (200,000 entities); the editor's 256 m radius holds about
    // 25 cells (12,500 entities). Run in Release: maya_editor_tests "[world-benchmark]".
    static std::atomic<int> counter{0};
    const auto folder = fs::temp_directory_path() / ("maya-world-cells-bench-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    fs::create_directories(folder / "levels");
    const auto file = folder / "levels/big.world";
    {
        auto scene = SceneDocument{};
        scene.entities.push_back({id(0, 1), std::nullopt, {NameComponent{"Camera"}, at(0, 2, 0), CameraComponent{}}});
        for (int x = -10; x < 10; ++x)
            for (int z = -10; z < 10; ++z)
                for (uint64_t i = 0; i < 250; ++i) {
                    const auto root = id(key(x, z), 10 + i * 2);
                    scene.entities.push_back({root, std::nullopt, {NameComponent{"Prop"}, at(x * size + 4 + double(i % 15) * 8, 0, z * size + 4 + double(i / 15) * 7)}});
                    scene.entities.push_back({id(key(x, z), 11 + i * 2), root, {NameComponent{"Part"}, at(0, 1, 0)}});
                }
        REQUIRE(save_world(file, scene, any_asset).empty());
    }
    const auto clock = [] { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto since) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count(); };
    auto text = std::ifstream(file);
    auto read = read_world(std::string(std::istreambuf_iterator<char>(text), {}));
    auto persistent = load_scene_file(file.parent_path() / read.document->persistent, any_asset);
    auto scene = SceneEditor(std::move(instantiate_scene(persistent.document, any_asset).world));
    scene.set_validation_context(any_asset);
    auto cells = EditorCells(scene, file, *read.document, cooked_cell_loader(file.parent_path(), std::make_shared<CookCache>(folder / "cache"), any_asset),
                             any_asset);
    cells.set_source({64, 0, 64});
    auto start = clock();
    cells.settle();
    printf("open: %zu cells, %zu entities in %.1f ms (cold)\n", cells.stats().cells[size_t(EditorCellState::active)], scene.world().size(), ms(start));
    auto worst = 0.0, total = 0.0;
    for (int i = 0; i < 100; ++i) {
        start = clock();
        cells.update();
        const auto took = ms(start);
        if (took > worst) printf("  idle update %d: %.3f ms (index ready: %d)\n", i, took, int(cells.index_ready()));
        worst = std::max(worst, took);
        total += took;
    }
    printf("idle update: mean %.3f ms, worst %.3f ms\n", total / 100, worst);
    const auto prop = id(key(0, 0), 10);
    start = clock();
    REQUIRE(scene.rename(prop, "Renamed"));
    const auto edit = ms(start);
    start = clock();
    cells.update();
    printf("rename: edit %.3f ms, update %.3f ms\n", edit, ms(start));
    worst = total = 0.0;
    for (int i = 0; i < 60; ++i) { // a gizmo drag: one transform a frame
        start = clock();
        REQUIRE(scene.set_component(prop, at(4.5 + i * 0.5, 0, 4)));
        cells.update();
        const auto took = ms(start);
        worst = std::max(worst, took);
        total += took;
    }
    printf("drag frame (edit + update): mean %.3f ms, worst %.3f ms\n", total / 60, worst);
    start = clock();
    REQUIRE(cells.save().empty());
    printf("save one changed cell: %.1f ms\n", ms(start));
    worst = total = 0.0;
    cells.set_source({64 + size, 0, 64}); // one cell east: a column in, a column out, within the budget
    auto frames = 0;
    for (; frames < 1000; ++frames) {
        start = clock();
        const auto& stats = cells.update();
        const auto took = ms(start);
        printf("  crossing frame %d: %.3f ms, %zu loading, %zu active\n", frames, took, stats.cells[size_t(EditorCellState::loading)],
               stats.cells[size_t(EditorCellState::active)]);
        worst = std::max(worst, took);
        total += took;
        if (stats.cells[size_t(EditorCellState::loading)] == 0 && stats.loads_in_flight == 0 && frames > 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    printf("crossing: %d frames, update mean %.3f ms, worst %.3f ms, longest cell %.2f ms\n", frames, total / frames, worst,
           cells.stats().longest_activation_ms);
    fs::remove_all(folder);
}

TEST_CASE("However small the frame's budget, each update makes progress", "[editor][worlds]") {
    Opened world;
    auto& cells = *world.cells;
    cells.settings().frame_budget = std::chrono::microseconds(1); // less than any step costs
    cells.set_source({64, 0, 64});
    auto frames = 0;
    for (; frames < 500 && active(cells) < 9; ++frames) {
        cells.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // loads finish on jobs meanwhile
    }
    CHECK(active(cells) == 9);
    CHECK(frames < 500);
}
