#include "maya/assets/generator_file.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/streaming/terrain_streamer.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <map>
#include <random>

using namespace maya;
using namespace maya::test;

namespace {
// A 6 x 6-cell region (768 m) at W1's place, 5 km from the origin (#1065).
GeneratorAsset region_generator(int32_t seed = 990) {
    auto generator = GeneratorAsset{};
    generator.seed = seed;
    generator.region_min_x = 40;
    generator.region_min_z = 33;
    generator.region_max_x = 45;
    generator.region_max_z = 38;
    return generator;
}

struct Fixture {
    NullGraphicsDevice device;
    TestProject project{device, {}};
    AssetRef<GeneratorAsset> generator{{0x7465, 0x900}};
    World world;
    EntityHandle terrain;
    std::shared_ptr<CookCache> cache;
    Fixture() {
        REQUIRE(device.initialize(nullptr, {}));
        project.write("hills.generator", write_generator_file(region_generator()));
        REQUIRE_FALSE(project.registry->register_asset({generator.id, AssetKind::generator, "hills.generator"}));
        cache = std::make_shared<CookCache>(project.root() / ".maya/cache");
        auto commands = world.commands();
        auto entity = commands.create();
        commands.add(entity, TransformComponent{});
        commands.add(entity, TerrainComponent{generator});
        terrain = world.commit(commands).created.front();
    }
    /// Updates until nothing is loading and an update changes nothing.
    void settle(TerrainStreamer& streamer, PhysicsWorld* physics = nullptr) {
        for (int i = 0; i < 400; ++i) {
            project.registry->update();
            project.registry->wait_idle();
            streamer.wait_idle();
            const auto& stats = streamer.update(world, *project.registry, physics);
            if (stats.loading == 0 && stats.activated == 0 && stats.deactivated == 0 && stats.uploaded == 0 && !streamer.terrains().empty() &&
                streamer.terrains().front().overview)
                return;
        }
        FAIL("the terrain did not settle");
    }
};
/// Tiles of the region within `radius` of a point (to the nearest point of each tile's square).
size_t tiles_within(const GeneratorAsset& g, math::DVec3 point, double radius) {
    auto count = size_t{0};
    for (auto z = g.region_min_z; z <= g.region_max_z; ++z)
        for (auto x = g.region_min_x; x <= g.region_max_x; ++x) {
            const auto dx = std::max({x * 128.0 - point.x, 0.0, point.x - (x + 1) * 128.0});
            const auto dz = std::max({z * 128.0 - point.z, 0.0, point.z - (z + 1) * 128.0});
            count += std::sqrt(dx * dx + dz * dz) <= radius;
        }
    return count;
}
} // namespace

TEST_CASE("Terrain tiles load and are drawn within the loading radius, and are active within the activation radius", "[terrain]") {
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.streaming.load_radius = 300.0;
    settings.tile_radius = 0.0; // the loading radius alone
    settings.streaming.activate_radius = 150.0;
    settings.streaming.hysteresis = 32.0;
    auto streamer = TerrainStreamer(&fixture.device, fixture.cache, settings);
    const auto here = math::DVec3{40 * 128.0 + 200.0, 50.0, 33 * 128.0 + 330.0};
    streamer.set_sources({here});
    fixture.settle(streamer);
    const auto generator = region_generator();
    const auto& stats = streamer.stats();
    CHECK(stats.terrains == 1);
    CHECK(stats.loaded == tiles_within(generator, here, 300.0));
    CHECK(stats.active == tiles_within(generator, here, 150.0));
    CHECK(stats.active > 0);
    CHECK(stats.generated == stats.loaded);
    CHECK(stats.cached == 0);
    REQUIRE(streamer.terrains().size() == 1);
    const auto& view = streamer.terrains().front();
    CHECK(view.entity == fixture.terrain);
    CHECK(view.generator == generator);
    REQUIRE(view.overview);
    CHECK(view.overview->width == 6 * 8 + 1);
    REQUIRE(view.overview_buffer);
    CHECK(view.overview_buffer->valid());
    CHECK(view.tiles.size() == stats.loaded); // every loaded tile is drawn
    CHECK(stats.drawn == stats.loaded);
    for (const auto& tile : view.tiles) {
        INFO(tile.index.x << ", " << tile.index.z);
        REQUIRE(tile.heights);
        REQUIRE(tile.buffer);
        CHECK(tile.buffer->valid());
        CHECK(tile.buffer->bytes() == size_t(tile_samples) * tile_samples * 8); // a float and a normal a sample
        // Exactly the generator's heights.
        const auto expected = generate_height_tile(heightfield_noise(generator), tile.index);
        REQUIRE(expected);
        CHECK(std::memcmp(tile.heights->heights.data(), expected->heights.data(), expected->heights.size() * sizeof(float)) == 0);
    }
    CHECK(stats.gpu_bytes == stats.loaded * tile_samples * tile_samples * 8 + view.overview_buffer->bytes());
    // Heights between samples follow the triangles drawn.
    const auto& tile = view.tiles.front();
    const auto x0 = tile.index.x * 128.0, z0 = tile.index.z * 128.0;
    CHECK(streamer.height_at(x0 + 7.0, z0 + 9.0) == tile.heights->height(7, 9));
    const auto between = streamer.height_at(x0 + 7.5, z0 + 9.25);
    REQUIRE(between);
    CHECK(*between >= std::min({tile.heights->height(7, 9), tile.heights->height(8, 9), tile.heights->height(8, 10)}) - 1e-4f);
    CHECK(*between <= std::max({tile.heights->height(7, 9), tile.heights->height(8, 9), tile.heights->height(8, 10)}) + 1e-4f);
    CHECK_FALSE(streamer.height_at(0.0, 0.0)); // outside the region

    // Moving away: tiles past the activation radius and its hysteresis are no longer drawn; past the
    // loading radius and its hysteresis they are released. Within the hysteresis nothing changes.
    const auto near = math::DVec3{here.x + 20.0, here.y, here.z};
    streamer.set_sources({near});
    fixture.settle(streamer);
    CHECK(streamer.stats().loaded >= stats.loaded - 1);
    const auto far = math::DVec3{45 * 128.0 + 100.0, 0.0, 38 * 128.0 + 100.0};
    streamer.set_sources({far});
    fixture.settle(streamer);
    // Those wanted now, and any still within the hysteresis beyond.
    CHECK(streamer.stats().loaded >= tiles_within(generator, far, 300.0));
    CHECK(streamer.stats().loaded <= tiles_within(generator, far, 332.0));
    CHECK(streamer.stats().active >= tiles_within(generator, far, 150.0));
    CHECK(streamer.stats().active <= tiles_within(generator, far, 182.0));
    for (const auto& moved : streamer.terrains().front().tiles) CHECK(moved.index.x >= 42);
    // No sources: nothing held but the overview.
    streamer.set_sources({});
    fixture.settle(streamer);
    CHECK(streamer.stats().loaded == 0);
    CHECK(streamer.stats().gpu_bytes == streamer.terrains().front().overview_buffer->bytes());
}

TEST_CASE("Terrain tiles join the physics world as height fields of the terrain entity, and leave it", "[terrain]") {
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.collide = true;
    settings.streaming.load_radius = 200.0;
    settings.tile_radius = 0.0; // the loading radius alone
    settings.streaming.activate_radius = 100.0;
    auto streamer = TerrainStreamer(nullptr, fixture.cache, settings); // no device: collision only
    PhysicsWorld physics;
    const auto here = math::DVec3{42 * 128.0 + 64.0, 0.0, 35 * 128.0 + 64.0};
    streamer.set_sources({here});
    fixture.settle(streamer, &physics);
    CHECK(streamer.stats().height_fields == streamer.stats().active);
    CHECK(physics.height_fields() == streamer.stats().active);
    CHECK(streamer.stats().gpu_bytes == 0);
    CHECK_FALSE(streamer.terrains().front().tiles.front().buffer);
    const auto ground = streamer.height_at(here.x, here.z);
    REQUIRE(ground);
    const auto hit = physics.raycast_nearest({here.x, 1000.0, here.z}, {0, -1, 0}, 2000.0f);
    REQUIRE(hit);
    CHECK(hit->entity == fixture.terrain);
    CHECK(std::abs(hit->point.y - double(*ground)) < 0.02);
    // The source moves 300 m east: the tiles behind it leave the physics world, those ahead join it.
    const auto before = physics.height_fields();
    const auto there = math::DVec3{here.x + 300.0, 0.0, here.z};
    streamer.set_sources({there});
    fixture.settle(streamer, &physics);
    CHECK(streamer.stats().active >= tiles_within(region_generator(), there, 100.0));
    CHECK(streamer.stats().active <= tiles_within(region_generator(), there, 100.0 + settings.streaming.hysteresis));
    CHECK(physics.height_fields() == streamer.stats().active);
    CHECK(physics.height_fields() == before);
    CHECK_FALSE(physics.raycast_nearest({here.x, 1000.0, here.z}, {0, -1, 0}, 2000.0f)); // nothing here now
    CHECK(physics.raycast_nearest({there.x, 1000.0, there.z}, {0, -1, 0}, 2000.0f));
    // The terrain removed from the World: its height fields go with it.
    auto commands = fixture.world.commands();
    commands.remove<TerrainComponent>(fixture.terrain);
    REQUIRE(fixture.world.commit(commands));
    streamer.update(fixture.world, *fixture.project.registry, &physics);
    CHECK(physics.height_fields() == 0);
    CHECK(streamer.terrains().empty());
    CHECK_FALSE(physics.raycast_nearest({here.x, 1000.0, here.z}, {0, -1, 0}, 2000.0f));
}

TEST_CASE("Tiles loaded before a physics world collide once one is given", "[terrain]") {
    // The editor draws terrain with no physics; Play then hands it one, and its loaded tiles join it.
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.collide = true;
    settings.streaming.load_radius = 200.0;
    settings.tile_radius = 0.0;
    settings.streaming.activate_radius = 100.0;
    auto streamer = TerrainStreamer(nullptr, fixture.cache, settings);
    const auto here = math::DVec3{42 * 128.0 + 64.0, 0.0, 35 * 128.0 + 64.0};
    streamer.set_sources({here});
    fixture.settle(streamer);
    CHECK(streamer.stats().height_fields == 0);
    const auto generated = streamer.stats().generated;
    PhysicsWorld physics;
    fixture.settle(streamer, &physics);
    CHECK(streamer.stats().generated == generated); // the same tiles, not loaded again
    CHECK(streamer.stats().height_fields == tiles_within(region_generator(), here, 100.0));
    CHECK(physics.height_fields() == streamer.stats().height_fields);
    CHECK(physics.raycast_nearest({here.x, 1000.0, here.z}, {0, -1, 0}, 2000.0f));
    streamer.clear(&physics);
    CHECK(physics.height_fields() == 0);
}

TEST_CASE("Tiles wanted but not yet started that fall behind a moving source are dropped, not left loading", "[terrain]") {
    // #1069: tiles wait to start while others generate (tiles_in_flight). One the source left behind before it
    // started, past the loading radius but within the hysteresis, was kept loading with no job, forever.
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.streaming.load_radius = 300.0;
    settings.tile_radius = 0.0;
    settings.streaming.activate_radius = 100.0;
    settings.streaming.hysteresis = 64.0;
    settings.tiles_in_flight = 1;
    auto streamer = TerrainStreamer(nullptr, fixture.cache, settings);
    const auto here = math::DVec3{42 * 128.0 + 64.0, 0.0, 35 * 128.0 + 64.0};
    streamer.set_sources({here});
    for (int i = 0; i < 50 && streamer.stats().loading <= 1; ++i) { // the generator loads, then its tiles are wanted, one started
        fixture.project.registry->update();
        fixture.project.registry->wait_idle();
        streamer.update(fixture.world, *fixture.project.registry, nullptr);
    }
    REQUIRE(streamer.stats().loading > 1);
    // 40 m west: the east side's farthest tiles are no longer wanted, but within the hysteresis.
    streamer.set_sources({{here.x - 40.0, 0.0, here.z}});
    fixture.settle(streamer);
    CHECK(streamer.stats().loading == 0);
    CHECK(streamer.stats().loaded >= tiles_within(region_generator(), {here.x - 40.0, 0.0, here.z}, 300.0));
    CHECK(streamer.stats().loaded <= tiles_within(region_generator(), {here.x - 40.0, 0.0, here.z}, 364.0));
}

TEST_CASE("A world's cells wait for the terrain under them to collide before they activate", "[terrain]") {
    // #1069: a cell's bodies join the physics world when it activates; its ground must be there first, or a
    // dynamic body falls through. The world streamer asks the terrain streamer (link_terrain), as the player does.
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.collide = true;
    settings.streaming.load_radius = 200.0;
    settings.tile_radius = 0.0;
    settings.streaming.activate_radius = 100.0;
    settings.frame_tiles = 1; // one tile joins physics a frame, so the cell has to wait for its own
    auto terrain = TerrainStreamer(nullptr, fixture.cache, settings);
    PhysicsWorld physics;
    const auto cell = CellIndex{42, 35};
    const auto middle = math::DVec3{42 * 128.0 + 64.0, 0.0, 35 * 128.0 + 64.0};
    // One cell holding a ball 2 m over the ground.
    auto world = WorldDocument{};
    world.cells.push_back({cell, "42_35.scene", 1});
    auto ball = SceneEntity{EntityId{0x7465, 0x77}, std::nullopt, {}};
    const auto ground = generate_height_tile(heightfield_noise(region_generator()), {cell.x, cell.z});
    REQUIRE(ground);
    ball.components.push_back(TransformComponent{{middle.x, double(tile_height_at(*ground, 64.0, 64.0)) + 2.0, middle.z}, {}, {1.0f}});
    ball.components.push_back(RigidBodyComponent{});
    auto collider = ColliderComponent{};
    collider.shape = ColliderShape::sphere;
    ball.components.push_back(collider);
    const auto loader = CellLoader([ball](const WorldCell&, const JobContext&) { return CellLoad{SceneDocument{{ball}}, {}, 64}; });
    auto cells = WorldStreamer(world, loader, settings.streaming);
    link_terrain(cells, terrain);
    const auto square = math::DVec3{cell.x * 128.0, 0.0, cell.z * 128.0};
    // Outside the region, or without collision, nothing is waited for.
    CHECK(terrain.collides({}, 128.0));
    CHECK(TerrainStreamer(nullptr, nullptr, {}).collides(square, 128.0));
    terrain.set_sources({middle});
    cells.set_sources({middle});
    // The terrain found but not yet generated: the cell loads and waits, and settling the cells does not hang on it.
    terrain.update(fixture.world, *fixture.project.registry, &physics);
    CHECK_FALSE(terrain.collides(square, 128.0));
    cells.settle(fixture.world, &physics);
    CHECK(cells.state(cell) == CellState::ready);
    CHECK(cells.stats().waiting == 1);
    CHECK(physics.stats().dynamic_bodies == 0);
    // Frame by frame, terrain first: whenever the cell is active, the ground under the ball collides.
    auto waited = 0;
    for (int frame = 0; frame < 400 && cells.state(cell) != CellState::active; ++frame) {
        fixture.project.registry->update();
        terrain.wait_idle();
        terrain.update(fixture.world, *fixture.project.registry, &physics);
        cells.update(fixture.world, &physics);
        waited += cells.stats().waiting;
    }
    REQUIRE(cells.state(cell) == CellState::active);
    CHECK(waited > 0);
    CHECK(cells.stats().waiting == 0);
    CHECK(terrain.collides(square, 128.0));
    CHECK(physics.stats().dynamic_bodies == 1);
    auto filter = QueryFilter{};
    filter.ignore = fixture.world.find(ball.id);
    const auto hit = physics.raycast_nearest({middle.x, 1000.0, middle.z}, {0, -1, 0}, 2000.0f, filter);
    REQUIRE(hit);
    CHECK(hit->entity == fixture.terrain);
    // The source leaves. The terrain goes first, frame by frame, but keeps the height field the ball stands on
    // while its cell is in, whenever the cell goes out (its frame budget can put that off).
    const auto away = math::DVec3{middle.x + 900.0, 0.0, middle.z};
    terrain.set_sources({away});
    cells.set_sources({away});
    const auto fields = physics.height_fields();
    terrain.update(fixture.world, *fixture.project.registry, &physics);
    REQUIRE(fields >= 1);
    CHECK(physics.height_fields() == 1); // only the tile under the active cell is left
    CHECK(physics.raycast_nearest({middle.x, 1000.0, middle.z}, {0, -1, 0}, 2000.0f, filter));
    for (int frame = 0; frame < 50 && cells.state(cell) != CellState::unloaded; ++frame) {
        terrain.update(fixture.world, *fixture.project.registry, &physics);
        cells.update(fixture.world, &physics);
    }
    REQUIRE(cells.state(cell) == CellState::unloaded);
    terrain.update(fixture.world, *fixture.project.registry, &physics);
    CHECK(physics.height_fields() == 0);
    cells.unload_all(fixture.world, &physics);
    terrain.clear(&physics);
}

TEST_CASE("A world's larger cells wait for every terrain tile under them within the activation radius", "[terrain]") {
    // #1069: a world's cells need not be the terrain's 128 m tiles. A 256 m cell over four tiles goes in once each
    // of them the terrain collides with there holds its height field.
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.collide = true;
    settings.streaming.load_radius = 300.0;
    settings.tile_radius = 0.0;
    settings.streaming.activate_radius = 150.0;
    settings.frame_tiles = 1;
    auto terrain = TerrainStreamer(nullptr, fixture.cache, settings);
    PhysicsWorld physics;
    auto world = WorldDocument{};
    world.cell_size = 256.0;
    const auto cell = CellIndex{21, 17}; // x 5,376 to 5,632 and z 4,352 to 4,608: tiles 42 to 43 and 34 to 35
    world.cells.push_back({cell, "21_17.scene", 1});
    const auto loader = CellLoader([](const WorldCell&, const JobContext&) {
        return CellLoad{SceneDocument{{SceneEntity{EntityId{0x7465, 0x78}, std::nullopt, {NameComponent{"Marker"}}}}}, {}, 64};
    });
    auto cells = WorldStreamer(world, loader, settings.streaming);
    link_terrain(cells, terrain);
    const auto source = math::DVec3{42 * 128.0 + 64.0, 0.0, 34 * 128.0 + 64.0}; // over tile 42_34
    terrain.set_sources({source});
    cells.set_sources({source});
    auto checked = 0;
    for (int frame = 0; frame < 400 && cells.state(cell) != CellState::active; ++frame) {
        fixture.project.registry->update();
        terrain.wait_idle();
        terrain.update(fixture.world, *fixture.project.registry, &physics);
        cells.update(fixture.world, &physics);
        if (cells.state(cell) == CellState::ready && cells.stats().waiting == 1) ++checked;
    }
    REQUIRE(cells.state(cell) == CellState::active);
    CHECK(checked > 0);
    // Every tile under it within 150 m holds its height field: here all four (the farthest corner is 64 m away).
    CHECK(physics.height_fields() >= 4);
    for (const auto& [x, z] : {std::pair{42, 34}, {43, 34}, {42, 35}, {43, 35}})
        CHECK(physics.raycast_nearest({x * 128.0 + 64.0, 1000.0, z * 128.0 + 64.0}, {0, -1, 0}, 2000.0f));
    cells.unload_all(fixture.world, &physics);
    terrain.clear(&physics);
}

TEST_CASE("Terrain tiles come from the cook cache the second time, the same bytes, and change with their generator", "[terrain]") {
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.streaming.load_radius = 200.0;
    settings.tile_radius = 0.0; // the loading radius alone
    settings.streaming.activate_radius = 100.0;
    const auto here = math::DVec3{41 * 128.0, 0.0, 34 * 128.0};
    auto first = std::vector<std::vector<std::byte>>{};
    {
        auto streamer = TerrainStreamer(&fixture.device, fixture.cache, settings);
        streamer.set_sources({here});
        fixture.settle(streamer);
        CHECK(streamer.stats().cached == 0);
        for (const auto& tile : streamer.terrains().front().tiles) first.push_back(encode_height_tile(*tile.heights));
    }
    auto streamer = TerrainStreamer(&fixture.device, fixture.cache, settings);
    streamer.set_sources({here});
    fixture.settle(streamer);
    CHECK(streamer.stats().generated == 0);
    CHECK(streamer.stats().cached == streamer.stats().loaded);
    auto second = std::vector<std::vector<std::byte>>{};
    for (const auto& tile : streamer.terrains().front().tiles) second.push_back(encode_height_tile(*tile.heights));
    CHECK(second == first);
    // A new seed, saved and reloaded: the terrain starts again from the new generator.
    fixture.project.write("hills.generator", write_generator_file(region_generator(4242)));
    REQUIRE(fixture.project.registry->reload(fixture.generator));
    // Until its new tiles are ready, the terrain is drawn as it was (#1093): never a frame without it.
    const auto drawn_before = streamer.terrains().front().tiles.size();
    streamer.update(fixture.world, *fixture.project.registry, nullptr);
    REQUIRE(streamer.terrains().size() == 1);
    CHECK(streamer.terrains().front().generator.seed == 990);
    CHECK(streamer.terrains().front().tiles.size() == drawn_before);
    CHECK(streamer.stats().regenerating == 1);
    fixture.settle(streamer);
    CHECK(streamer.stats().regenerating == 0);
    CHECK(streamer.terrains().front().generator.seed == 4242);
    CHECK(streamer.stats().generated == streamer.stats().loaded);
    const auto expected = generate_height_tile(heightfield_noise(region_generator(4242)), streamer.terrains().front().tiles.front().index);
    REQUIRE(expected);
    CHECK(encode_height_tile(*streamer.terrains().front().tiles.front().heights) == encode_height_tile(*expected));
    // The cook keys: the noise and the tile, not the region.
    auto wider = region_generator();
    wider.region_max_x = 60;
    CHECK(height_tile_cook_key(wider, {41, 34}).digest() == height_tile_cook_key(region_generator(), {41, 34}).digest());
    CHECK(height_overview_cook_key(wider).digest() != height_overview_cook_key(region_generator()).digest());
    CHECK(height_tile_cook_key(region_generator(4242), {41, 34}).digest() != height_tile_cook_key(region_generator(), {41, 34}).digest());
    auto keys = std::vector<CookKey>{};
    append_generator_cook_keys(region_generator(), keys);
    CHECK(keys.size() == 1 + 36);
}

TEST_CASE("Terrain activates a bounded number of tiles a frame, nearest first", "[terrain]") {
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.streaming.load_radius = 400.0;
    settings.tile_radius = 0.0; // the loading radius alone
    settings.streaming.activate_radius = 400.0;
    settings.frame_tiles = 2;
    auto streamer = TerrainStreamer(&fixture.device, fixture.cache, settings);
    const auto here = math::DVec3{42 * 128.0 + 64.0, 0.0, 35 * 128.0 + 64.0};
    streamer.set_sources({here});
    // Everything loaded first, then activation spread over frames.
    for (int i = 0; i < 50; ++i) {
        fixture.project.registry->update();
        fixture.project.registry->wait_idle();
        streamer.wait_idle();
        const auto& stats = streamer.update(fixture.world, *fixture.project.registry, nullptr);
        CHECK(stats.activated <= 2);
    }
    CHECK(streamer.stats().active == tiles_within(region_generator(), here, 400.0));
    // The first tile drawn was the one the source stands on.
    auto again = TerrainStreamer(&fixture.device, fixture.cache, settings);
    again.set_sources({here});
    for (int i = 0; i < 20 && again.stats().active == 0; ++i) {
        fixture.project.registry->update();
        again.wait_idle();
        again.update(fixture.world, *fixture.project.registry, nullptr);
    }
    REQUIRE_FALSE(again.terrains().empty());
    const auto& first = again.terrains().front().tiles;
    REQUIRE_FALSE(first.empty());
    CHECK(std::ranges::any_of(first, [](const TerrainTileView& tile) { return tile.index.x == 42 && tile.index.z == 35; }));
}

TEST_CASE("A terrain streamer destroyed while tiles generate cancels them", "[terrain]") {
    Fixture fixture;
    auto settings = TerrainSettings{};
    settings.streaming.load_radius = 2000.0;
    settings.tile_radius = 0.0; // the loading radius alone
    for (int i = 0; i < 5; ++i) {
        auto streamer = TerrainStreamer(&fixture.device, nullptr, settings);
        streamer.set_sources({{42 * 128.0, 0.0, 35 * 128.0}});
        for (int frame = 0; frame < 3; ++frame) { // the generator becomes resident, then its tiles start
            fixture.project.registry->update();
            fixture.project.registry->wait_idle();
            streamer.update(fixture.world, *fixture.project.registry, nullptr);
        }
        CHECK(streamer.stats().loading > 0); // 36 tiles wanted, 16 at a time
    } // destroyed with jobs in flight
}

TEST_CASE("Terrain patches cover the region once, neighbours differ by a level at most, and meet where morphs agree", "[terrain][cdlod]") {
    // A region 11 x 7 cells, from the world's origin's far side, with uneven tiles; eyes all over it and above it.
    auto tiles = std::vector<TileIndex>{};
    auto ranges = std::vector<std::pair<float, float>>{};
    for (int32_t x = -3; x <= 7; ++x)
        for (int32_t z = 2; z <= 8; ++z) {
            tiles.push_back({x, z});
            ranges.push_back({float(-20 - (x * 7 + z * 3) % 40), float(30 + (x * 11 + z * 5) % 90)});
        }
    std::ranges::sort(tiles); // the extent wants them sorted, and ranges in the same order
    for (size_t i = 0; i < tiles.size(); ++i)
        ranges[i] = {float(-20 - (tiles[i].x * 7 + tiles[i].z * 3 + 400) % 40), float(30 + (tiles[i].x * 11 + tiles[i].z * 5 + 400) % 90)};
    const auto extent = TerrainExtent{{-3, 2}, {7, 8}, -40.0f, 150.0f, tiles, ranges};
    const auto [lowest, highest] = terrain_band(extent);
    std::mt19937 random(1068);
    auto real = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(random); };
    for (int round = 0; round < 60; ++round) {
        const auto eye = math::DVec3{real(-3 * 128.0 - 300, 8 * 128.0 + 300), real(-60.0, 900.0), real(2 * 128.0 - 300, 9 * 128.0 + 300)};
        INFO("eye " << eye.x << ", " << eye.y << ", " << eye.z);
        auto patches = std::vector<TerrainPatchChoice>{};
        select_terrain_patches(extent, eye, nullptr, {}, patches);
        // The metric the vertex shader uses: horizontal, with the band's height gap.
        const auto gap = terrain_height_gap(eye.y, lowest, highest);
        const auto metric = [&](double x, double z) { return std::sqrt((x - eye.x) * (x - eye.x) + (z - eye.z) * (z - eye.z) + gap * gap); };
        // Coverage: the patches' areas inside the region add up to it, and none overlap (sampled at cell centres).
        auto covered = std::map<std::pair<int64_t, int64_t>, int>{};
        for (const auto& p : patches)
            for (double z = p.z + 8.0; z < p.z + p.size(); z += 16.0)
                for (double x = p.x + 8.0; x < p.x + p.size(); x += 16.0)
                    if (x > -3 * 128.0 && x < 8 * 128.0 && z > 2 * 128.0 && z < 9 * 128.0) ++covered[{int64_t(x), int64_t(z)}];
        CHECK(covered.size() == size_t(11 * 8) * size_t(7 * 8));
        CHECK(std::ranges::all_of(covered, [](const auto& entry) { return entry.second == 1; }));
        // Tile levels read tiles; every one is in the region here.
        for (const auto& p : patches) CHECK((p.level < terrain_tile_levels) == (p.tile >= 0));
        // Neighbours: a level apart at most; along a level change the finer side has finished morphing and
        // the coarser has not begun.
        auto jumps = 0, unfinished = 0, begun = 0;
        for (const auto& a : patches)
            for (const auto& b : patches) {
                if (b.level != a.level + 1 && b.level <= a.level + 1) continue;
                const auto as = a.size(), bs = b.size();
                const auto along_x = (a.x + as == b.x || b.x + bs == a.x) && a.z < b.z + bs && b.z < a.z + as;
                const auto along_z = (a.z + as == b.z || b.z + bs == a.z) && a.x < b.x + bs && b.x < a.x + as;
                if (!along_x && !along_z) continue;
                if (b.level > a.level + 1) {
                    ++jumps;
                    continue;
                }
                for (int i = 0; i <= 32; ++i) {
                    const auto x = along_x ? (a.x + as == b.x ? a.x + as : a.x) : a.x + as * i / 32.0;
                    const auto z = along_x ? a.z + as * i / 32.0 : (a.z + as == b.z ? a.z + as : a.z);
                    if (x < b.x || x > b.x + bs || z < b.z || z > b.z + bs) continue;
                    unfinished += metric(x, z) < terrain_range(a.level);
                    begun += metric(x, z) > terrain_morph_start(b.level);
                }
            }
        CHECK(jumps == 0);
        CHECK(unfinished == 0);
        CHECK(begun == 0);
    }
}
