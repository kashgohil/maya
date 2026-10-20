#include "editor_harness.hpp"
#include "maya/assets/generator_file.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <sstream>
#include <chrono>
#include <thread>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
constexpr auto terrain_id = EntityId{0x6d617961, 0x602};
constexpr auto boulder_id = EntityId{0x6d617961, 0x603};
constexpr auto hills = AssetId{0x6d617961, 0x90};
/// Frames until the editor's terrain has drawn every tile it wants and its overview: by a deadline, not a count of
/// frames, since its tiles generate on jobs, which other tests running beside it slow.
void settle(Harness& harness) {
    for (const auto start = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - start < std::chrono::seconds(20);) {
        harness.frame();
        const auto* terrain = harness.shell.terrain();
        if (terrain && terrain->stats().loading == 0 && terrain->stats().uploaded == 0 && !terrain->terrains().empty() &&
            terrain->terrains().front().overview)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    FAIL("the terrain did not settle");
}
ImVec2 centre(const EditorLayout::Field* rect) {
    REQUIRE(rect);
    return {(rect->min.x + rect->max.x) / 2, (rect->min.y + rect->max.y) / 2};
}
GeneratorAsset on_disk(const ProjectCopy& copy) {
    auto input = std::istringstream(copy.read("terrain/hills.generator"));
    const auto read = read_generator_file(input);
    INFO(read.error);
    REQUIRE(read);
    return read.generator;
}
} // namespace

TEST_CASE("The editor draws a scene's terrain, picks it, and Play collides with it", "[editor][terrain]") {
    Harness harness;
    harness.frames(2);
    REQUIRE(harness.shell.open_scene(sample_project().parent_path() / "assets/terrain.scene"));
    harness.shell.camera() = EditorCamera({0.0, 80.0, 90.0}, 0.0f, -0.6f);
    settle(harness);
    const auto* terrain = harness.shell.terrain();
    REQUIRE(terrain);
    CHECK(terrain->stats().terrains == 1);
    CHECK(terrain->stats().drawn == 16); // the 4 x 4-cell region, all within reach of the view
    CHECK(terrain->stats().height_fields == 0); // nothing collides while editing
    // The boulder and the sample's scattered rocks (#1093), outside the history; terrain is drawn apart from mesh renderers.
    // The rocks come once their cells are made on jobs: waited for by a deadline, then a frame draws them.
    REQUIRE(harness.shell.scatter());
    for (const auto start = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - start < std::chrono::seconds(30);) {
        const auto& stats = harness.shell.scatter()->stats();
        if (stats.items > 0 && stats.cells_waiting == 0) break;
        harness.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    harness.frame();
    CHECK(harness.shell.extraction().mesh_renderers == 1 + harness.shell.scatter()->stats().items);
    CHECK(harness.shell.scene()->transient_count() == harness.shell.scatter()->stats().items);
    CHECK(harness.shell.scatter()->stats().items > 0);

    // A click on the ground selects the terrain.
    const auto ground = terrain->height_at(10.0, 20.0);
    REQUIRE(ground);
    press(harness, on_screen(harness, {10.0, double(*ground), 20.0}));
    harness.frames(2);
    REQUIRE(harness.shell.scene()->selection().size() == 1);
    CHECK(harness.shell.scene()->selection().front() == terrain_id);

    // Play: the ground collides before the first tick, so the boulder dropped from 60 m comes to rest on it.
    REQUIRE(harness.shell.start_play());
    auto* play = harness.shell.play_session();
    REQUIRE(play);
    REQUIRE(harness.shell.terrain());
    CHECK(harness.shell.terrain()->stats().height_fields > 0);
    CHECK(play->physics().height_fields() == harness.shell.terrain()->stats().height_fields);
    while (play->clock().tick() < 600) harness.frame();
    const auto boulder = play->world().find(boulder_id);
    REQUIRE(boulder);
    const auto pose = play->world().world_matrix(*boulder);
    REQUIRE(pose);
    const auto below = harness.shell.terrain()->height_at(pose->translation.x, pose->translation.z);
    REQUIRE(below);
    INFO("boulder at " << pose->translation.x << ", " << pose->translation.y << ", " << pose->translation.z << "; ground " << *below);
    CHECK(pose->translation.y > double(*below) + 0.5); // on it, a sphere of radius 1 (on a slope it may roll)
    CHECK(pose->translation.y < double(*below) + 1.5);
    // Stopping Play takes the height fields out with its physics; the authored terrain draws again.
    harness.shell.stop_play();
    settle(harness);
    CHECK(harness.shell.terrain()->stats().height_fields == 0);
    CHECK(harness.shell.terrain()->stats().drawn == 16);
}

TEST_CASE("A generator edited in the Inspector regenerates the terrain, undoes in the scene's history, and saves with ⌘S", "[editor][terrain]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    REQUIRE(harness.shell.open_scene(copy.content / "terrain.scene"));
    harness.shell.camera() = EditorCamera({0.0, 80.0, 90.0}, 0.0f, -0.6f);
    settle(harness);
    auto& scene = *harness.shell.scene();
    const auto original = on_disk(copy);
    const auto file = copy.read("terrain/hills.generator");
    const auto ground = *harness.shell.terrain()->height_at(10.0, 20.0);

    // The terrain's generator, folded under its component until opened (the terrain's fields leave it
    // below the window here), and shown alone when chosen in the Assets panel.
    scene.select(terrain_id);
    harness.frames(2);
    REQUIRE(harness.shell.layout().control("generator.fold"));
    CHECK_FALSE(harness.shell.layout().field("generator.amplitude"));
    filter_assets(harness, "hills");
    press(harness, centre(harness.shell.layout().control("asset.terrain/hills.generator")));
    harness.frames(2);
    const auto from = centre(harness.shell.layout().field("generator.amplitude"));
    drag(harness, from, {from.x + 80.0f, from.y}, 10);
    const auto* edited = scene.generator(hills);
    REQUIRE(edited);
    CHECK(edited->amplitude > original.amplitude);
    CHECK(scene.undo_label() == "Edit hills"); // the drag is one step
    CHECK(scene.dirty_generators() == std::vector{hills});
    CHECK(copy.read("terrain/hills.generator") == file); // nothing written yet
    const auto served = [&] { return harness.shell.assets()->acquire(AssetRef<GeneratorAsset>{hills}).lease.value(); };
    CHECK(served() == *edited); // published: the terrain regenerates from it
    settle(harness);
    CHECK(harness.shell.terrain()->terrains().front().generator == *edited);
    CHECK(*harness.shell.terrain()->height_at(10.0, 20.0) != ground);

    // Undo puts the file's generator back, and the terrain as it was.
    const auto amplified = *edited;
    REQUIRE(scene.undo());
    CHECK(*scene.generator(hills) == original);
    CHECK(served() == original);
    settle(harness);
    CHECK(*harness.shell.terrain()->height_at(10.0, 20.0) == ground);
    REQUIRE(scene.redo());

    // A refused edit (a region whose last cell comes before its first) changes nothing and says why.
    auto inverted = amplified;
    CHECK_FALSE(edit_properties(inverted, std::vector{PropertyEdit{generator_property("region_max_x")->id, int32_t(-5)}}));

    // ⌘S writes the scene and the generator.
    chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
    CHECK_FALSE(scene.dirty());
    CHECK(scene.dirty_generators().empty());
    CHECK(on_disk(copy) == amplified);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Saved terrain/hills.generator"));
}

TEST_CASE("Terrain and Scatter from the Create menu write new generators, catalog them, and run them", "[editor][terrain]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    REQUIRE(harness.shell.new_scene());
    auto& scene = *harness.shell.scene();
    harness.shell.camera() = EditorCamera::looking_at({300, 60, 380}, {300, 0, 300}); // the ground looked at is in cell 2_2
    harness.frames(2);
    const auto create = [&](const char* key) {
        press(harness, centre(harness.shell.layout().control("hierarchy.create")));
        harness.frames(2);
        press(harness, centre(harness.shell.layout().control(key)));
        harness.frames(2);
        REQUIRE(scene.primary());
        return *scene.primary();
    };
    const auto record = [&](const char* path) {
        const auto& records = harness.shell.assets()->records();
        const auto found = std::ranges::find(records, std::filesystem::path(path), &AssetRecord::path);
        INFO(path);
        REQUIRE(found != records.end());
        CHECK(found->kind == AssetKind::generator);
        return found->id;
    };
    const auto file = [&](const char* path) {
        auto input = std::istringstream(copy.read(path));
        const auto read = read_generator_file(input);
        INFO(path << ": " << read.error);
        REQUIRE(read);
        return read.generator;
    };

    // Terrain: a heights generator with its kind's defaults over the 4 x 4 cells around where the view looks.
    const auto terrain = create("create.terrain");
    const auto heights = record("terrain/terrain.generator");
    CHECK(std::get<TerrainComponent>(*find_component(*scene.record(terrain), ComponentId::terrain)).heights.id == heights);
    auto expected = GeneratorAsset{};
    expected.region_min_x = 0;
    expected.region_min_z = 0;
    expected.region_max_x = 3;
    expected.region_max_z = 3;
    CHECK(file("terrain/terrain.generator") == expected);
    {
        auto input = std::istringstream(copy.read("catalog.maya")); // on disk too
        const auto catalog = read_asset_catalog(input);
        REQUIRE(catalog);
        CHECK(std::ranges::count(catalog.records, heights, &AssetRecord::id) == 1);
    }
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Created terrain/terrain.generator"));
    settle(harness); // it runs: the terrain streams around the camera
    // Another is numbered.
    const auto second = create("create.terrain");
    CHECK(record("terrain/terrain-2.generator") != heights);
    // Undo removes the entity; the file and its catalog line stay, as an import's.
    REQUIRE(harness.shell.undo_edit());
    CHECK_FALSE(scene.record(second));
    CHECK(std::filesystem::is_regular_file(copy.content / "terrain/terrain-2.generator"));

    // Scatter: over the scene's terrain, placing nothing until it has a mesh.
    const auto scatter = create("create.scatter");
    const auto rocks = record("terrain/scatter.generator");
    CHECK(std::get<GeneratorComponent>(*find_component(*scene.record(scatter), ComponentId::generator)).generator.id == rocks);
    const auto made = file("terrain/scatter.generator");
    CHECK(made.kind == GeneratorKind::scatter);
    CHECK(made.heights.id == heights);
    CHECK(made.region_min_x == 0);
    CHECK(made.region_max_z == 3);
    const auto scattered = [&](bool some) { // by a deadline: scatter generates on jobs
        for (const auto start = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - start < std::chrono::seconds(30);) {
            harness.frame();
            const auto& stats = harness.shell.scatter()->stats();
            if (stats.generated + stats.cached > 0 && stats.cells_waiting == 0 && stats.cells_loading == 0 && (!some || stats.items > 0))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    REQUIRE(scattered(false)); // its cells made, with nothing in them
    CHECK(harness.shell.scatter()->stats().items == 0);
    // Given a mesh, it places items.
    if (!scene.generator(rocks)) scene.open_generator(rocks, harness.shell.assets()->acquire(AssetRef<GeneratorAsset>{rocks}).lease.value());
    auto meshed = *scene.generator(rocks);
    meshed.mesh0 = {AssetId{0x6d617961, 5}};
    REQUIRE(scene.set_generator(rocks, meshed, "Edit scatter"));
    REQUIRE(scattered(true));
}
