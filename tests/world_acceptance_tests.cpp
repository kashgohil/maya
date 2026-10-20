// Milestone acceptance for world scale (#1069, docs/acceptance.md#the-world-workflow-automated): a creator's
// workflow end to end in the editor, without engine C++, on a fresh copy of the sample project in
// build/acceptance: a world created; terrain and scatter added from the Create menu and edited; content placed
// in several cells, one entity's script pointing at another in a far cell; the cells streamed around the camera,
// with the Streaming and Residency panels showing them; played; saved; reopened. package_tests.cpp then
// packages the world and runs it outside the checkout (CTest maya_world_acceptance_package).

#include "editor_harness.hpp"
#include "maya/assets/generator_file.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/world_io.hpp"
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
namespace fs = std::filesystem;

namespace {
constexpr auto cube_mesh = AssetId{0x6d617961, 2};
constexpr auto sphere_mesh = AssetId{0x6d617961, 5};
constexpr auto red = AssetId{0x6d617961, 0x11};
constexpr auto grass = AssetId{0x6d617961, 0x91};
constexpr auto rock = AssetId{0x6d617961, 0x92};
constexpr auto beacon_script = AssetId{0x6d617961, 0x3c};
/// The creator's script, written in a text editor: it turns its entity each tick toward the one it points at.
constexpr auto beacon = R"(-- The world workflow's beacon (#1069): turns each tick toward its target, an entity in another cell. While
-- that cell is not in, the reference does not resolve and the beacon holds still.
local Beacon = { properties = { target = { type = "entity", label = "Target" } } }

function Beacon:fixed_update(dt: number)
    local target = self.target
    if target == nil or not target:alive() then
        return
    end
    local f = target:world_position() - self.entity:world_position()
    self.entity:set_rotation(maya.quaternion.from_axis_angle(vector.create(0, 1, 0), math.atan2(-f.x, -f.z)))
end

return Beacon
)";

fs::path project_folder() { return fs::path(MAYA_ACCEPTANCE_DIR) / "World Game"; }

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
void type(Harness& harness, const std::string& text) {
    for (const auto c : text) harness.frame({TextEvent{uint32_t(c)}});
}
/// The Assets panel's filter replaced, as ⌘A and typing do: it lists only the rows that fit.
void refilter_assets(Harness& harness, const std::string& text) {
    press(harness, control(harness, "assets.filter"));
    chord(harness, {KeyCode::LeftSuper}, KeyCode::A);
    type(harness, text);
    harness.frame(key(KeyCode::Enter, true));
    harness.frame(key(KeyCode::Enter, false));
    harness.frames(2);
}
TransformComponent at(double x, double y, double z) { return {{x, y, z}, {}, math::Vec3(1.0f)}; }
/// Frames until `done`, by a deadline rather than a count of frames: cells and scatter load and generate on jobs,
/// which other tests running beside this one (or a sanitizer) slow.
template<class Done> bool frames_until(Harness& harness, Done done, std::chrono::seconds limit = std::chrono::seconds(60)) {
    for (const auto start = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - start < limit;) {
        harness.frame();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
/// Frames until the editor's cells are what its camera wants, and scatter has made its items in them.
void settle(Harness& harness) {
    auto frames = 0;
    const auto settled = frames_until(harness, [&] {
        const auto& cells = harness.shell.cells()->stats();
        const auto* scatter = harness.shell.scatter();
        return ++frames > 2 && cells.cells[size_t(EditorCellState::loading)] == 0 && cells.loads_in_flight == 0 && scatter &&
               scatter->stats().cells_waiting == 0 && scatter->stats().cells_loading == 0;
    });
    if (!settled) FAIL("the world did not settle");
}
AssetId cataloged(Harness& harness, const fs::path& path) {
    const auto& records = harness.shell.assets()->records();
    const auto found = std::ranges::find(records, path, &AssetRecord::path);
    INFO(path);
    REQUIRE(found != records.end());
    return found->id;
}
size_t generated_items(const World& world) {
    auto count = size_t{0};
    world.for_each<GeneratedComponent>([&](EntityHandle, const GeneratedComponent&) { ++count; });
    return count;
}
template<class T> std::optional<T> component_of(const World& world, EntityId id) {
    auto found = std::optional<T>{};
    if (const auto handle = world.find(id)) world.with<T>(*handle, [&](const T& value) { found = value; });
    return found;
}
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
GeneratorAsset generator_on_disk(const fs::path& file) {
    auto input = std::ifstream(file);
    auto read = read_generator_file(input);
    INFO(file << ": " << read.error);
    REQUIRE(read);
    return read.generator;
}
} // namespace

TEST_CASE("A world is made in the editor: terrain, scatter, and content across cells, streamed, inspected, played, and saved",
          "[acceptance][world][author]") {
    auto error = std::error_code{};
    fs::remove_all(project_folder(), error);
    fs::create_directories(project_folder());
    fs::create_directories(fs::path(MAYA_ACCEPTANCE_DIR) / "elsewhere");
    fs::copy(sample_project().parent_path() / "assets", project_folder() / "assets", fs::copy_options::recursive);
    fs::copy_file(sample_project(), project_folder() / "project.maya");
    fs::remove_all(project_folder() / "assets/.maya", error); // an earlier run's cook cache
    // The creator's script and its catalog line, written in a text editor as docs/scripting.md says.
    std::ofstream(project_folder() / "assets/scripts/beacon.luau") << beacon;
    std::ofstream(project_folder() / "assets/catalog.maya", std::ios::app) << "script 6d617961 3c \"scripts/beacon.luau\"\n";
    const auto file = project_folder() / "assets/levels/valley.world";

    auto expected = std::map<std::string, std::string>{};
    auto tower_id = EntityId{};
    {
        Harness harness(false);
        REQUIRE(harness.shell.open_project(project_folder()));
        harness.frames(3);
        // Create: New world, named in its prompt. Its camera goes up over the valley.
        harness.shell.request_new_world();
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        type(harness, "levels/valley");
        press(harness, control(harness, "dialog.save"));
        harness.frames(2);
        REQUIRE(harness.shell.cells());
        REQUIRE(harness.shell.scene_path() == fs::canonical(file));
        auto& scene = *harness.shell.scene();
        auto& cells = *harness.shell.cells();
        const auto camera = find_named(scene, "Camera");
        REQUIRE(scene.set_component(camera, TransformComponent{{0.0, 30.0, 70.0}, math::Quat::from_axis_angle({1, 0, 0}, -0.35f), math::Vec3(1.0f)}));
        harness.shell.camera() = EditorCamera::looking_at({0, 40, 90}, {0, 0, 0});
        harness.frames(2);

        // Terrain from the Create menu: a new generator, cataloged, and an entity in the persistent part naming it.
        press(harness, control(harness, "hierarchy.create"));
        harness.frames(2);
        press(harness, control(harness, "create.terrain"));
        harness.frames(2);
        const auto terrain = find_named(scene, "Terrain");
        const auto heights = cataloged(harness, "terrain/terrain.generator");
        CHECK(fs::is_regular_file(project_folder() / "assets/terrain/terrain.generator"));
        CHECK(component_of<TerrainComponent>(scene.world(), terrain)->heights.id == heights);
        CHECK(cells.part_of_root(terrain) == WorldPart(std::nullopt));
        // Edited: a gentle valley in grass. The generator, opened from the Assets panel, in the Inspector.
        filter_assets(harness, "terrain.generator");
        press(harness, control(harness, "asset.terrain/terrain.generator"));
        harness.frames(2);
        REQUIRE(scene.generator(heights));
        auto valley = *scene.generator(heights);
        CHECK(valley.region_min_x == -2); // 4 x 4 cells around where the view looks
        CHECK(valley.region_max_x == 1);
        valley.octaves = 4;
        valley.wavelength = 160.0f;
        valley.amplitude = 6.0f;
        valley.warp = 0.0f;
        REQUIRE(scene.set_generator(heights, valley, "Edit terrain"));
        auto ground = *component_of<TerrainComponent>(scene.world(), terrain);
        ground.layer0 = {grass};
        REQUIRE(scene.set_component(terrain, ground));

        // Scatter from the Create menu: over the terrain, then given rocks to place.
        press(harness, control(harness, "hierarchy.create"));
        harness.frames(2);
        press(harness, control(harness, "create.scatter"));
        harness.frames(2);
        const auto scatter = find_named(scene, "Scatter");
        const auto rocks = cataloged(harness, "terrain/scatter.generator");
        CHECK(component_of<GeneratorComponent>(scene.world(), scatter)->generator.id == rocks);
        CHECK(cells.part_of_root(scatter) == WorldPart(std::nullopt));
        refilter_assets(harness, "scatter.generator");
        press(harness, control(harness, "asset.terrain/scatter.generator"));
        harness.frames(2);
        REQUIRE(scene.generator(rocks));
        auto scattered = *scene.generator(rocks);
        CHECK(scattered.kind == GeneratorKind::scatter);
        CHECK(scattered.heights.id == heights); // over the scene's terrain
        scattered.mesh0 = {sphere_mesh};
        scattered.material0 = {rock};
        scattered.collider0 = 0.5f;
        scattered.spacing = 8.0f;
        scattered.density = 0.3f;
        REQUIRE(scene.set_generator(rocks, scattered, "Edit scatter"));
        settle(harness);
        CHECK(harness.shell.scatter()->stats().items > 0);
        CHECK(generated_items(scene.world()) == harness.shell.scatter()->stats().items);

        // Content in three cells: a crate dropped in view (0_0), a tower far east (2_0), a post south (0_-3), and
        // a beacon by the crate whose script points at the tower: a reference across cells.
        refilter_assets(harness, "cube");
        drag(harness, control(harness, "asset.cube.obj"), on_screen(harness, {10.0, 0.0, 10.0}));
        const auto crate = *scene.primary();
        REQUIRE(scene.rename(crate, "Crate"));
        REQUIRE(scene.create("Tower", std::nullopt, {TransformComponent{{300, 0, 40}, {}, {4, 20, 4}},
                                                     MeshRendererComponent{AssetRef<MeshAsset>{cube_mesh}, AssetRef<MaterialAsset>{red}, true}}));
        const auto tower = *scene.primary();
        tower_id = tower;
        REQUIRE(scene.create("Post", std::nullopt, {at(20, 0, -350), MeshRendererComponent{AssetRef<MeshAsset>{cube_mesh}, AssetRef<MaterialAsset>{red}, true}}));
        const auto post = *scene.primary();
        REQUIRE(scene.create("Beacon", std::nullopt, {at(5, 4, 5), MeshRendererComponent{AssetRef<MeshAsset>{cube_mesh}, AssetRef<MaterialAsset>{red}, true},
                                                      ScriptComponent{AssetRef<ScriptAsset>{beacon_script}, {{"target", ScriptValueType::entity, tower}}}}));
        const auto lighthouse = *scene.primary();
        harness.frames(2);
        CHECK(cells.part_of_root(crate) == WorldPart(CellIndex{0, 0}));
        CHECK(cells.part_of_root(lighthouse) == WorldPart(CellIndex{0, 0}));
        CHECK(cells.part_of_root(tower) == WorldPart(CellIndex{2, 0}));
        CHECK(cells.part_of_root(post) == WorldPart(CellIndex{0, -3}));

        // Save: the world in its parts, and both generators' files with their edits.
        chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
        harness.frames(2);
        CHECK_FALSE(scene.dirty());
        CHECK(cells.changed_cells().empty());
        CHECK(scene.dirty_generators().empty());
        CHECK(generator_on_disk(project_folder() / "assets/terrain/terrain.generator") == valley);
        CHECK(generator_on_disk(project_folder() / "assets/terrain/scatter.generator") == scattered);
        expected = {{"Camera", "persistent"}, {"Sun", "persistent"}, {"Terrain", "persistent"}, {"Scatter", "persistent"},
                    {"Crate", "0_0"},         {"Beacon", "0_0"},     {"Tower", "2_0"},         {"Post", "0_-3"}};
        CHECK(parts_on_disk(file, asset_property_context(*harness.shell.assets())) == expected);

        // Stream: the camera goes far away, and every cell leaves, saved, with the scatter's items and the cells'
        // cooked bytes in the Residency panel.
        harness.shell.show_residency();
        harness.shell.camera() = EditorCamera::looking_at({5000, 20, 5000}, {5000, 0, 4990});
        settle(harness);
        harness.frames(20); // the Residency panel refreshes four times a second, while it shows
        CHECK(cells.state({0, 0}) == EditorCellState::unloaded);
        CHECK(cells.state({2, 0}) == EditorCellState::unloaded);
        CHECK_FALSE(scene.record(lighthouse));
        CHECK(generated_items(scene.world()) == 0);
        REQUIRE(harness.shell.layout().control("residency.cells"));
        CHECK(harness.shell.residency_figures().report.of(ResidencyCategory::cells).total() == 0);
        // Back by the beacon, with a smaller radius: its cell loads from disk, the tower's does not. The Residency
        // panel shows the loaded cells' cooked bytes, and the Streaming panel the cells.
        harness.shell.cells()->settings().load_radius = 150.0;
        harness.shell.camera() = EditorCamera::looking_at({-60, 40, -60}, {5, 0, 5});
        settle(harness);
        harness.frames(20);
        REQUIRE(cells.state({0, 0}) == EditorCellState::active);
        REQUIRE(cells.state({2, 0}) == EditorCellState::unloaded); // 256 m away
        CHECK(generated_items(scene.world()) > 0);
        const auto near_bytes = harness.shell.residency_figures().report.of(ResidencyCategory::cells).total();
        CHECK(near_bytes == cells.stats().bytes_loaded);
        CHECK(near_bytes > 0);
        harness.shell.show_streaming();
        harness.frames(2);
        REQUIRE(harness.shell.layout().control("streaming.panel"));
        // The Inspector follows the beacon's reference, loading the tower's cell, and selects the tower.
        REQUIRE(frames_until(harness, [&] { return cells.index_ready(); }));
        scene.select(lighthouse);
        harness.frames(2);
        press(harness, control(harness, "reference.follow.target"));
        harness.frames(2);
        CHECK(cells.state({2, 0}) == EditorCellState::active);
        REQUIRE(scene.primary());
        CHECK(*scene.primary() == tower);
        harness.shell.show_residency();
        harness.frames(20);
        CHECK(harness.shell.residency_figures().report.of(ResidencyCategory::cells).total() > near_bytes);

        // Play: the play World streams around the world's camera, with terrain, scatter, and both cells; the beacon
        // turns toward the tower across cells. Stop leaves the editor's world as it was, and nothing unsaved.
        const auto loaded = scene.world().size();
        REQUIRE(harness.shell.start_play());
        REQUIRE(frames_until(harness, [&] {
            const auto* play = harness.shell.play_streamer();
            return play && play->stats().cells[size_t(CellState::loading)] == 0 && play->stats().waiting == 0 &&
                   play->stats().cells[size_t(CellState::active)] > 0 && harness.shell.play_session()->clock().tick() > 30;
        }));
        CHECK(harness.shell.play_streamer()->stats().cells[size_t(CellState::active)] >= 2);
        const auto& played = harness.shell.play_session()->world();
        CHECK(played.find(tower));
        CHECK(played.find(lighthouse));
        CHECK(generated_items(played) > 0);
        CHECK(component_of<TransformComponent>(played, lighthouse)->rotation.w < 0.9f); // it turned, toward +x
        harness.shell.stop_play();
        harness.frames(2);
        CHECK(scene.world().size() == loaded);
        CHECK_FALSE(scene.dirty());
        CHECK(component_of<TransformComponent>(scene.world(), lighthouse)->rotation.w == 1.0f);
    }
    // Reopened in a fresh editor: the world as saved, its terrain and scatter running again.
    Harness reopened(false);
    REQUIRE(reopened.shell.open_project(project_folder()));
    reopened.shell.camera() = EditorCamera::looking_at({0, 40, 90}, {0, 0, 0});
    REQUIRE(reopened.shell.open_scene("levels/valley.world"));
    settle(reopened);
    auto& scene = *reopened.shell.scene();
    CHECK_FALSE(scene.dirty());
    auto& cells = *reopened.shell.cells();
    CHECK(cells.state({0, 0}) == EditorCellState::active);
    CHECK(generated_items(scene.world()) > 0);
    // What it loaded is where it was saved: the terrain and scatter in the persistent part, the crate and the
    // beacon in 0_0, its script still pointing at the tower.
    CHECK(cells.part_of_root(find_named(scene, "Terrain")) == WorldPart(std::nullopt));
    CHECK(cells.part_of_root(find_named(scene, "Scatter")) == WorldPart(std::nullopt));
    CHECK(cells.part_of_root(find_named(scene, "Crate")) == WorldPart(CellIndex{0, 0}));
    const auto lighthouse = find_named(scene, "Beacon");
    CHECK(cells.part_of_root(lighthouse) == WorldPart(CellIndex{0, 0}));
    const auto script = component_of<ScriptComponent>(scene.world(), lighthouse);
    REQUIRE(script);
    REQUIRE(script->values.size() == 1);
    CHECK(script->values.front().name == "target");
    CHECK(std::get<EntityId>(script->values.front().data) == tower_id);
    CHECK(parts_on_disk(file, asset_property_context(*reopened.shell.assets())) == expected);
}
