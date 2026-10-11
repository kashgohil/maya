// Scatter streaming (#1093, docs/procedural.md#scatter): a scatter generator's items as entities around a
// source, skipping what authored entities stand for, regenerated when what makes them changes.

#include "maya/assets/generator_file.hpp"
#include "maya/physics/physics.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/streaming/scatter_streamer.hpp"
#include "maya/streaming/terrain_streamer.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <set>
#include <thread>

using namespace maya;
using namespace maya::test;

namespace {
constexpr auto rock = AssetRef<MeshAsset>{{0x6d617961, 2}};
constexpr auto stone = AssetRef<MaterialAsset>{{0x6d617961, 0x11}};
GeneratorAsset hills() { // 3 x 3 cells at W1's place
    auto generator = GeneratorAsset{};
    generator.region_min_x = 40;
    generator.region_min_z = 33;
    generator.region_max_x = 42;
    generator.region_max_z = 35;
    return generator;
}
GeneratorAsset rocks(AssetRef<GeneratorAsset> heights) {
    auto generator = hills();
    generator.kind = GeneratorKind::scatter;
    generator.version = scatter_version;
    generator.seed = 1093;
    generator.heights = heights;
    generator.spacing = 8.0f; // 256 candidates a cell
    generator.density = 0.5f;
    generator.max_slope = 40.0f;
    generator.mesh0 = rock;
    generator.material0 = stone;
    generator.collider0 = 0.5f;
    return generator;
}

struct Fixture {
    NullGraphicsDevice device;
    TestProject project{device, {}};
    AssetRef<GeneratorAsset> heights{{0x7465, 0x900}}, scatter{{0x7465, 0x901}};
    World world;
    EntityHandle runner;
    std::shared_ptr<CookCache> cache;
    WorldScatterSink sink{world, nullptr};
    Fixture() {
        REQUIRE(device.initialize(nullptr, {}));
        project.write("hills.generator", write_generator_file(hills()));
        project.write("rocks.generator", write_generator_file(rocks(heights)));
        REQUIRE_FALSE(project.registry->register_asset({heights.id, AssetKind::generator, "hills.generator"}));
        REQUIRE_FALSE(project.registry->register_asset({scatter.id, AssetKind::generator, "rocks.generator"}));
        cache = std::make_shared<CookCache>(project.root() / ".maya/cache");
        auto commands = world.commands();
        auto entity = commands.create(EntityId{0xabc, 1});
        commands.add(entity, NameComponent{"Rocks"});
        commands.add(entity, GeneratorComponent{scatter});
        runner = world.commit(commands).created.front();
    }
    ScatterSettings settings(double radius = 150.0) const {
        auto settings = ScatterSettings{};
        settings.streaming.activate_radius = radius;
        settings.streaming.hysteresis = 20.0;
        return settings;
    }
    /// What the streamer should make in a cell, made here directly.
    std::vector<ScatterItem> expected(TileIndex cell, const GeneratorAsset& generator, std::span<const ScatterExclusion> exclusions = {}) const {
        const auto tile = generate_height_tile(heightfield_noise(hills()), cell);
        return *scatter_cell({scatter.id.high, scatter.id.low}, scatter_rules(generator), cell, &*tile, exclusions);
    }
    void settle(ScatterStreamer& streamer, ScatterSink& into) { streamer.settle(world, *project.registry, into); }
    void settle(ScatterStreamer& streamer) { settle(streamer, sink); }
};
size_t made_by(const World& world, AssetRef<GeneratorAsset> generator) {
    auto count = size_t{0};
    world.for_each<GeneratedComponent>([&](EntityHandle, const GeneratedComponent& marker) { count += marker.generator == generator; });
    return count;
}
const math::DVec3 middle{41 * 128.0 + 64.0, 0.0, 34 * 128.0 + 64.0}; // the region's middle cell
} // namespace

TEST_CASE("Scatter streams its items in as entities around a source, the same ones every time", "[scatter]") {
    Fixture fixture;
    auto streamer = ScatterStreamer(fixture.cache, fixture.settings(100.0));
    streamer.set_sources({middle});
    fixture.settle(streamer);
    // Within 100 m of the middle of the middle cell: all nine cells of the region (the corner cells start 90.5 m away).
    auto all = std::vector<ScatterItem>{};
    for (int z = 33; z <= 35; ++z)
        for (int x = 40; x <= 42; ++x) {
            const auto cell = fixture.expected({x, z}, rocks(fixture.heights));
            all.insert(all.end(), cell.begin(), cell.end());
        }
    const auto& stats = streamer.stats();
    CHECK(stats.generators == 1);
    CHECK(stats.cells_active == 9);
    CHECK(stats.items == all.size());
    CHECK(stats.generated == 9);
    CHECK(made_by(fixture.world, fixture.scatter) == all.size());
    for (const auto& item : all) {
        const auto handle = fixture.world.find(item.id);
        REQUIRE(handle);
        CHECK(streamer.made(item.id));
        const auto pose = fixture.world.world_matrix(*handle);
        CHECK(pose->translation == item.position);
        fixture.world.with<MeshRendererComponent>(*handle, [&](const MeshRendererComponent& renderer) {
            CHECK(renderer.mesh == rock);
            CHECK(renderer.material == stone);
        });
        CHECK(fixture.world.has<ColliderComponent>(*handle));
        fixture.world.with<NameComponent>(*handle, [&](const NameComponent& name) { CHECK(name.value == "rocks"); });
    }
    // Again, from the cook cache: the same entities.
    auto world2 = World{};
    auto commands = world2.commands();
    auto entity = commands.create(EntityId{0xabc, 1});
    commands.add(entity, GeneratorComponent{fixture.scatter});
    REQUIRE(world2.commit(commands));
    auto sink2 = WorldScatterSink(world2, nullptr);
    auto again = ScatterStreamer(fixture.cache, fixture.settings(100.0));
    again.set_sources({middle});
    again.settle(world2, *fixture.project.registry, sink2);
    CHECK(again.stats().cached == 9);
    CHECK(again.stats().generated == 0);
    for (const auto& item : all) CHECK(world2.find(item.id));
    // The source moves away: the cells go, and their items with them.
    streamer.set_sources({middle + math::DVec3{5000.0, 0.0, 0.0}});
    fixture.settle(streamer);
    CHECK(streamer.stats().items == 0);
    CHECK(made_by(fixture.world, fixture.scatter) == 0);
    CHECK(fixture.world.size() == 1);
}

TEST_CASE("Items authored entities stand for are not made, and overrides of items no longer made are orphans", "[scatter]") {
    Fixture fixture;
    const auto cell = TileIndex{41, 34};
    const auto items = fixture.expected(cell, rocks(fixture.heights));
    REQUIRE(items.size() > 10);
    // An override (moved), a removed item, and a baked one (no marker), all with their items' IDs; and an override
    // of an ID the generator never makes in this cell.
    auto commands = fixture.world.commands();
    auto moved = commands.create(items[0].id);
    commands.add(moved, TransformComponent{items[0].position + math::DVec3{1.0, 0.0, 0.0}, {}, math::Vec3(1.0f)});
    commands.add(moved, GeneratedComponent{fixture.scatter, false});
    auto removed = commands.create(items[1].id);
    commands.add(removed, TransformComponent{items[1].position, {}, math::Vec3(1.0f)});
    commands.add(removed, GeneratedComponent{fixture.scatter, true});
    auto baked = commands.create(items[2].id);
    commands.add(baked, TransformComponent{items[2].position, {}, math::Vec3(1.0f)});
    const auto stray = EntityId{0x51a7, 0x1};
    auto orphan = commands.create(stray);
    commands.add(orphan, TransformComponent{middle, {}, math::Vec3(1.0f)});
    commands.add(orphan, GeneratedComponent{fixture.scatter, false});
    REQUIRE(fixture.world.commit(commands));
    auto streamer = ScatterStreamer(fixture.cache, fixture.settings(1.0)); // the middle cell alone
    streamer.set_sources({middle});
    fixture.settle(streamer);
    CHECK(streamer.stats().cells_active == 1);
    CHECK(streamer.stats().skipped == 3);
    CHECK(streamer.stats().items == items.size() - 3);
    for (size_t i = 0; i < 3; ++i) CHECK_FALSE(streamer.made(items[i].id));
    CHECK(fixture.world.size() == 1 + 4 + items.size() - 3); // each ID once
    CHECK(streamer.orphans() == std::set<EntityId>{stray});
    CHECK(streamer.stats().orphans == 1);
    const auto reports = streamer.take_reports();
    REQUIRE(reports.size() == 1);
    CHECK(reports.front().find("no longer makes: it is kept as it is") != std::string::npos);
    CHECK(fixture.world.find(stray)); // kept
    CHECK(streamer.take_reports().empty()); // once
    // The generator changes so it no longer makes the moved item: its override becomes an orphan, and stays.
    auto sparser = rocks(fixture.heights);
    sparser.density = 0.05f;
    const auto survivors = fixture.expected(cell, sparser);
    REQUIRE(std::ranges::find(survivors, items[0]) == survivors.end());
    REQUIRE_FALSE(fixture.project.registry->publish(fixture.scatter, sparser));
    fixture.settle(streamer);
    CHECK(streamer.orphans().contains(items[0].id));
    CHECK(fixture.world.find(items[0].id));
}

TEST_CASE("A changed generator remakes its items, the survivors keeping their IDs, the old staying until the new are ready", "[scatter]") {
    Fixture fixture;
    auto streamer = ScatterStreamer(fixture.cache, fixture.settings(1.0));
    streamer.set_sources({middle});
    fixture.settle(streamer);
    const auto before = fixture.expected({41, 34}, rocks(fixture.heights));
    REQUIRE(streamer.stats().items == before.size());
    auto sparser = rocks(fixture.heights);
    sparser.density = 0.25f;
    REQUIRE_FALSE(fixture.project.registry->publish(fixture.scatter, sparser));
    // One update: the job starts; the old items are still all in.
    streamer.update(fixture.world, *fixture.project.registry, fixture.sink);
    CHECK(streamer.stats().items == before.size());
    CHECK(streamer.stats().regenerating == 1);
    fixture.settle(streamer);
    const auto after = fixture.expected({41, 34}, sparser);
    CHECK(streamer.stats().items == after.size());
    CHECK(streamer.stats().regenerations == 1);
    CHECK(streamer.stats().regenerating == 0);
    for (const auto& item : after) {
        CHECK(std::ranges::find(before, item) != before.end()); // a survivor: the same ID and place
        CHECK(fixture.world.find(item.id));
    }
    // Two changes before the first is made: the first's job is cancelled, and only the last is shown.
    auto denser = rocks(fixture.heights);
    denser.density = 0.75f;
    REQUIRE_FALSE(fixture.project.registry->publish(fixture.scatter, sparser));
    auto busy = sparser;
    busy.density = 0.9f;
    REQUIRE_FALSE(fixture.project.registry->publish(fixture.scatter, busy));
    streamer.update(fixture.world, *fixture.project.registry, fixture.sink);
    REQUIRE_FALSE(fixture.project.registry->publish(fixture.scatter, denser));
    streamer.update(fixture.world, *fixture.project.registry, fixture.sink);
    fixture.settle(streamer);
    CHECK(streamer.stats().cancelled >= 1);
    CHECK(streamer.stats().items == fixture.expected({41, 34}, denser).size());
    // The generator component removed: every item goes.
    auto commands = fixture.world.commands();
    commands.remove<GeneratorComponent>(fixture.runner);
    REQUIRE(fixture.world.commit(commands));
    fixture.settle(streamer);
    CHECK(streamer.stats().items == 0);
    CHECK(fixture.world.size() == 1);
}

TEST_CASE("Exclusion volumes clear scatter's items, and moving one remakes what it reaches", "[scatter]") {
    Fixture fixture;
    auto commands = fixture.world.commands();
    auto clearing = commands.create(EntityId{0xc1ea, 1});
    commands.add(clearing, TransformComponent{middle, {}, math::Vec3(1.0f)});
    auto sphere = ScatterExclusionComponent{};
    sphere.shape = ExclusionShape::sphere;
    sphere.radius = 30.0f;
    commands.add(clearing, sphere);
    const auto clearing_id = EntityId{0xc1ea, 1};
    REQUIRE(fixture.world.commit(commands));
    auto streamer = ScatterStreamer(fixture.cache, fixture.settings(1.0));
    streamer.set_sources({middle});
    fixture.settle(streamer);
    auto exclusion = ScatterExclusion{};
    exclusion.shape = ScatterExclusion::Shape::sphere;
    exclusion.pose.translation = middle;
    exclusion.radius = 30.0f;
    const auto cleared = fixture.expected({41, 34}, rocks(fixture.heights), std::vector{exclusion});
    CHECK(streamer.stats().items == cleared.size());
    CHECK(cleared.size() < fixture.expected({41, 34}, rocks(fixture.heights)).size());
    fixture.world.for_each<GeneratedComponent>([&](EntityHandle entity, const GeneratedComponent&) {
        CHECK_FALSE(exclusion.contains(fixture.world.world_matrix(entity)->translation));
    });
    // Moved 50 m east: the cell is made again, clear around the new place.
    auto moved = fixture.world.commands();
    moved.replace(*fixture.world.find(clearing_id), TransformComponent{middle + math::DVec3{50.0, 0.0, 0.0}, {}, math::Vec3(1.0f)});
    REQUIRE(fixture.world.commit(moved));
    fixture.settle(streamer);
    exclusion.pose.translation = middle + math::DVec3{50.0, 0.0, 0.0};
    CHECK(streamer.stats().items == fixture.expected({41, 34}, rocks(fixture.heights), std::vector{exclusion}).size());
    CHECK(streamer.stats().regenerations == 1);
}

TEST_CASE("Items wait for their cell's authored content, go when it goes, and collide in a physics world", "[scatter][physics]") {
    Fixture fixture;
    auto streamer = ScatterStreamer(fixture.cache, fixture.settings(1.0));
    auto ready = false;
    streamer.set_ready([&](CellIndex cell) { return ready && cell == CellIndex{41, 34}; });
    streamer.set_sources({middle});
    PhysicsWorld physics;
    auto sink = WorldScatterSink(fixture.world, &physics);
    fixture.settle(streamer, sink);
    CHECK(streamer.stats().items == 0); // made, waiting
    ready = true;
    fixture.settle(streamer, sink);
    const auto items = fixture.expected({41, 34}, rocks(fixture.heights));
    CHECK(streamer.stats().items == items.size());
    CHECK(physics.stats().static_bodies == items.size());
    // A ray down onto an item hits it.
    const auto& item = items.front();
    const auto hit = physics.raycast_nearest(item.position + math::DVec3{0.0, 50.0, 0.0}, {0, -1, 0}, 100.0f);
    REQUIRE(hit);
    CHECK(hit->entity == *fixture.world.find(item.id));
    ready = false;
    fixture.settle(streamer, sink);
    CHECK(streamer.stats().items == 0);
    CHECK(physics.stats().static_bodies == 0);
    ready = true;
    fixture.settle(streamer, sink);
    CHECK(streamer.stats().items == items.size());
    streamer.clear(sink);
    CHECK(physics.stats().static_bodies == 0);
    CHECK(fixture.world.size() == 1);
}

TEST_CASE("However small the frame's budget, scatter makes progress, and a cell shows all at once", "[scatter]") {
    Fixture fixture;
    auto settings = fixture.settings(1.0);
    settings.frame_budget = std::chrono::microseconds(1);
    settings.chunk_items = 16;
    auto streamer = ScatterStreamer(fixture.cache, settings);
    streamer.set_sources({middle});
    const auto expected = fixture.expected({41, 34}, rocks(fixture.heights)).size();
    auto frames = 0;
    for (; frames < 2000 && streamer.stats().items < expected; ++frames) {
        fixture.project.registry->update();
        streamer.update(fixture.world, *fixture.project.registry, fixture.sink);
        // Never part of a cell: none, or all of it.
        const auto shown = made_by(fixture.world, fixture.scatter);
        CHECK((shown == 0 || shown == expected));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(streamer.stats().items == expected);
    CHECK(frames > 3); // over several frames
}
