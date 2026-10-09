#include "maya/assets/cook_cache.hpp"
#include "maya/physics/physics.hpp"
#include "maya/scene/scene_binary.hpp"
#include "maya/scene/world_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/streaming/world_streamer.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/gltf.hpp"
#include "support/hdr.hpp"
#include "support/png.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>

// Worlds divided into cells, streamed in and out (#1064, docs/world.md).
using namespace maya;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {
const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
constexpr double cell = 128.0;

EntityId id(uint64_t high, uint64_t low) { return {0x5700000000000000ull | high, low}; }
TransformComponent at(double x, double y, double z) { return {{x, y, z}, {}, math::Vec3(1.0f)}; }

/// A grid world of (2r+1)² cells around the origin: in each, `roots` entities with a child each, one in five
/// with a static collider, a ground box, and a dynamic sphere above it. The persistent part holds a camera
/// and the physics settings.
SceneDocument grid_world(int r, int roots) {
    auto scene = SceneDocument{};
    auto camera = SceneEntity{id(0, 1), std::nullopt, {NameComponent{"Camera"}, at(0, 2, 0), CameraComponent{}}};
    scene.entities.push_back(camera);
    scene.entities.push_back({id(0, 2), std::nullopt, {NameComponent{"Settings"}, PhysicsSettingsComponent{}}});
    for (int cx = -r; cx <= r; ++cx)
        for (int cz = -r; cz <= r; ++cz) {
            const auto key = uint64_t(uint32_t(cx + 1000)) << 16 | uint32_t(cz + 1000);
            const auto x0 = cx * cell, z0 = cz * cell;
            auto ground = ColliderComponent{};
            ground.half_extents = {64.0f, 0.5f, 64.0f};
            scene.entities.push_back({id(key, 1), std::nullopt, {NameComponent{"Ground"}, at(x0 + 64, -0.5, z0 + 64), ground}});
            auto ball = ColliderComponent{};
            ball.shape = ColliderShape::sphere;
            ball.radius = 0.5f;
            scene.entities.push_back({id(key, 2), std::nullopt, {NameComponent{"Ball"}, at(x0 + 64, 3, z0 + 64), ball, RigidBodyComponent{}}});
            for (int i = 0; i < roots; ++i) {
                const auto root = id(key, 100 + uint64_t(i) * 2);
                auto components = std::vector<ComponentValue>{NameComponent{"Prop " + std::to_string(i)},
                                                              at(x0 + 4 + (i * 37) % 120, 0.5, z0 + 4 + (i * 53) % 120)};
                if (i % 5 == 0) components.push_back(ColliderComponent{});
                scene.entities.push_back({root, std::nullopt, components});
                scene.entities.push_back({id(key, 101 + uint64_t(i) * 2), root, {NameComponent{"Part"}, at(0, 1, 0)}});
            }
        }
    return scene;
}

struct Folder {
    fs::path path;
    Folder() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("maya-streaming-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~Folder() {
        auto error = std::error_code{};
        fs::remove_all(path, error);
    }
};

struct SavedWorld {
    Folder folder;
    fs::path file;
    WorldDocument document;
    SavedWorld(int r, int roots) {
        file = folder.path / "levels" / "grid.world";
        fs::create_directories(file.parent_path());
        auto diagnostics = save_world(file, grid_world(r, roots), any_asset);
        INFO((diagnostics.empty() ? std::string() : diagnostics.front().message));
        REQUIRE(diagnostics.empty());
        auto text = std::ifstream(file);
        auto read = read_world(std::string(std::istreambuf_iterator<char>(text), {}));
        INFO(read.error);
        REQUIRE(read);
        document = *read.document;
    }
    CellLoader loader() const {
        return cooked_cell_loader(file.parent_path(), std::make_shared<CookCache>(folder.path / "cache"), any_asset);
    }
    /// A play session with the persistent part, as the player starts one.
    std::unique_ptr<PlaySession> session() const {
        auto loaded = load_scene_file(file.parent_path() / document.persistent, any_asset);
        REQUIRE(loaded);
        auto started = PlaySession::start(std::move(loaded.document), any_asset, builtin_systems());
        INFO(started.error);
        REQUIRE(started);
        return std::move(started.session);
    }
};

/// Every visible entity's ID.
std::set<EntityId> ids_of(const World& world) {
    auto result = std::set<EntityId>{};
    world.for_each_entity([&](EntityHandle entity) { result.insert(*world.persistent_id(entity)); });
    return result;
}
} // namespace

TEST_CASE("A world saves as a world file, a persistent scene, and a scene per occupied cell", "[streaming][format]") {
    auto saved = SavedWorld(1, 4);
    const auto& world = saved.document;
    CHECK(world.cell_size == 128.0);
    CHECK(world.persistent == fs::path("grid/persistent.scene"));
    REQUIRE(world.cells.size() == 9);
    CHECK(world.cells.front().index == CellIndex{-1, -1});
    CHECK(world.cells.front().scene == fs::path("grid/cells/-1_-1.scene"));
    CHECK(world.cells.front().entities == 2 + 4 * 2);
    CHECK(world.cell({0, 0}));
    CHECK_FALSE(world.cell({5, 5}));
    // Whole hierarchies go to the cell of their root; the camera and settings stay persistent.
    auto loaded = load_world(saved.file, any_asset);
    INFO((loaded.diagnostics.empty() ? std::string() : loaded.diagnostics.front().message));
    REQUIRE(loaded);
    CHECK(loaded.persistent.entities.size() == 2);
    for (const auto& [index, scene] : loaded.cells)
        for (const auto& entity : scene.entities)
            if (!entity.parent) {
                const auto& transform = std::get<TransformComponent>(*std::ranges::find_if(entity.components, [](const ComponentValue& v) {
                    return std::holds_alternative<TransformComponent>(v);
                }));
                CHECK(cell_of(transform.translation) == index);
            }
    CHECK(loaded.whole().entities.size() == grid_world(1, 4).entities.size());
    // The file reads back as written; mistakes name their line.
    CHECK(write_world(world) == write_world(*read_world(write_world(world)).document));
    CHECK(read_world("maya-world 2\n").error == "line 1: world format version 2 is not one this build reads (1)");
    CHECK(read_world("maya-world 1\npersistent \"p.scene\"\ncell 0 0 \"a.scene\" 1\ncell 0 0 \"b.scene\" 1\n").error ==
          "line 4: cell 0_0 is listed twice");
    CHECK(read_world("maya-world 1\n").error == "the world names no persistent scene");
    CHECK(cell_of({-0.5, 0.0, 128.0}) == CellIndex{-1, 1});
}

TEST_CASE("Cells cook to a packed binary that decodes to the same scene, and refuse damage", "[streaming][format]") {
    const auto scene = grid_world(0, 20);
    const auto binary = encode_scene_binary(scene);
    auto decoded = decode_scene_binary(binary, any_asset);
    INFO((decoded.diagnostics.empty() ? std::string() : decoded.diagnostics.front().message));
    REQUIRE(decoded);
    CHECK(encode_scene_binary(decoded.document) == binary);
    CHECK(decoded.document.entities.size() == scene.entities.size());
    // Damage of any kind is a diagnostic, never a crash.
    for (const auto cut : {size_t{0}, size_t{7}, size_t{20}, binary.size() / 2, binary.size() - 1}) {
        INFO("cut at " << cut);
        CHECK_FALSE(decode_scene_binary(std::string_view(binary).substr(0, cut), any_asset));
    }
    auto foreign = binary;
    foreign[9] ^= 0x5a; // the schema fingerprint
    CHECK(decode_scene_binary(foreign, any_asset).diagnostics.front().message.find("other component schemas") != std::string::npos);
    auto flipped = binary;
    for (size_t i = 24; i < flipped.size(); i += 97) flipped[i] = char(flipped[i] ^ 0xff);
    (void)decode_scene_binary(flipped, any_asset); // refused or valid, but never undefined
}

TEST_CASE("Cells stream in and out under a moving source, matching a whole-world load", "[streaming]") {
    auto saved = SavedWorld(2, 30); // 5 × 5 cells
    auto session = saved.session();
    auto settings = StreamingSettings{};
    settings.load_radius = 200.0;
    settings.activate_radius = 100.0;
    settings.hysteresis = 32.0;
    settings.frame_entities = 40; // a small budget: cells go in over several frames
    auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
    auto whole = load_world(saved.file, any_asset);
    REQUIRE(whole);
    // The source walks across the world along x, then back.
    auto path = std::vector<math::DVec3>{};
    for (double x = -300.0; x <= 300.0; x += 20.0) path.push_back({x, 0.0, 10.0});
    for (double x = 300.0; x >= -300.0; x -= 35.0) path.push_back({x, 0.0, 10.0});
    for (const auto& position : path) {
        streamer.set_sources({position});
        for (int frame = 0; frame < 4; ++frame) {
            const auto& stats = streamer.update(session->world(), &session->physics());
            INFO(stats.last_error);
            CHECK(stats.cells[size_t(CellState::failed)] == 0);
            REQUIRE(session->update(1.0 / 60.0).error.empty());
        }
        // Settled, the World holds exactly the persistent part and the active cells, as a whole load would.
        streamer.settle(session->world(), &session->physics());
        auto expected = std::set<EntityId>{};
        for (const auto& entity : whole.persistent.entities) expected.insert(entity.id);
        for (const auto& [index, scene] : whole.cells)
            if (streamer.state(index) == CellState::active)
                for (const auto& entity : scene.entities) expected.insert(entity.id);
        CHECK(ids_of(session->world()) == expected);
        CHECK(session->world().staged_count() == 0);
        // Cells within the activation radius are active; beyond the load radius plus hysteresis, unloaded.
        for (const auto& info : saved.document.cells) {
            const auto x0 = info.index.x * cell, z0 = info.index.z * cell;
            const auto dx = std::max({x0 - position.x, 0.0, position.x - (x0 + cell)});
            const auto dz = std::max({z0 - position.z, 0.0, position.z - (z0 + cell)});
            const auto d = std::hypot(dx, dz);
            if (d <= settings.activate_radius) CHECK(streamer.state(info.index) == CellState::active);
            if (d > settings.load_radius + settings.hysteresis) CHECK(streamer.state(info.index) == CellState::unloaded);
        }
    }
    const auto& stats = streamer.stats();
    CHECK(stats.activations > 10);
    CHECK(stats.deactivations > 10);
    CHECK(stats.pinned == 0);
}

TEST_CASE("Activation never takes more than the frame's entities, and a cell appears all at once", "[streaming]") {
    auto saved = SavedWorld(0, 200); // one cell of 402 entities
    auto session = saved.session();
    auto settings = StreamingSettings{};
    settings.frame_entities = 50;
    auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
    streamer.set_sources({{64.0, 0.0, 64.0}});
    const auto persistent = session->world().size();
    auto frames = 0;
    while (streamer.state({0, 0}) != CellState::active && frames < 1000) {
        const auto staged_before = session->world().staged_count();
        streamer.update(session->world(), &session->physics());
        ++frames;
        const auto staged = session->world().staged_count();
        if (streamer.state({0, 0}) == CellState::activating) {
            CHECK(staged - staged_before <= settings.frame_entities); // no more than the budget a frame
            CHECK(session->world().size() == persistent); // nothing of it visible until it is complete
        }
        if (streamer.state({0, 0}) == CellState::loading) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(streamer.state({0, 0}) == CellState::active);
    CHECK(frames >= 402 / 50);
    CHECK(session->world().size() == persistent + 402);
    // Its bodies joined in one batch: 1 ground, 1 ball, 40 props.
    CHECK(session->physics().stats().bodies_created == 42);
}

TEST_CASE("Delayed, failed, and cancelled loads, a world closed mid-load, and fast crossings leave nothing behind", "[streaming]") {
    auto saved = SavedWorld(1, 10);
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = true;
        std::set<CellIndex> failing;
        std::atomic<int> loads{0};
    };
    auto gate = std::make_shared<Gate>();
    const auto cooked = saved.loader();
    const auto loader = [gate, cooked](const WorldCell& info, const JobContext& job) -> CellLoad {
        ++gate->loads;
        {
            auto lock = std::unique_lock(gate->mutex);
            gate->changed.wait(lock, [&] { return gate->open || job.cancelled(); });
        }
        if (gate->failing.contains(info.index)) return {std::nullopt, "the disk is gone", 0};
        return cooked(info, job);
    };
    const auto release = [&] {
        {
            auto lock = std::lock_guard(gate->mutex);
            gate->open = true;
        }
        gate->changed.notify_all();
    };
    const auto close = [&] {
        auto lock = std::lock_guard(gate->mutex);
        gate->open = false;
    };
    auto settings = StreamingSettings{};
    settings.load_radius = 60.0;
    settings.activate_radius = 30.0;
    settings.hysteresis = 8.0;
    SECTION("a delayed load waits, and a cancelled one is discarded when it finishes") {
        auto session = saved.session();
        auto streamer = WorldStreamer(saved.document, loader, settings);
        close();
        streamer.set_sources({{64.0, 0.0, 64.0}});
        streamer.update(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::loading);
        // Gone before it finished: cancelled.
        streamer.set_sources({{5000.0, 0.0, 5000.0}});
        streamer.update(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::unloaded);
        CHECK(streamer.stats().loads_cancelled >= 1);
        release();
        streamer.set_sources({{64.0, 0.0, 64.0}});
        streamer.settle(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::active);
        CHECK(session->world().staged_count() == 0);
    }
    SECTION("a failed load is reported, nothing of it enters the World, and it is tried again when the source returns") {
        auto session = saved.session();
        gate->failing.insert({0, 0});
        auto streamer = WorldStreamer(saved.document, loader, settings);
        streamer.set_sources({{64.0, 0.0, 64.0}});
        streamer.settle(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::failed);
        CHECK(streamer.stats().last_error == "the disk is gone");
        CHECK(session->world().size() == 2);
        gate->failing.clear();
        streamer.set_sources({{5000.0, 0.0, 5000.0}});
        streamer.update(session->world(), &session->physics());
        streamer.set_sources({{64.0, 0.0, 64.0}});
        streamer.settle(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::active);
    }
    SECTION("closing the world with loads in flight waits for the jobs and touches nothing") {
        auto session = saved.session();
        close();
        {
            auto streamer = WorldStreamer(saved.document, loader, settings);
            streamer.set_sources({{64.0, 0.0, 64.0}});
            streamer.update(session->world(), &session->physics());
            CHECK(streamer.stats().cells[size_t(CellState::loading)] >= 1);
            release(); // the jobs finish into a streamer that is going away
        }
        CHECK(session->world().size() == 2);
        session.reset(); // and the session, after it
    }
    SECTION("crossing back and forth at a boundary every frame neither thrashes nor leaks") {
        auto session = saved.session();
        settings.frame_entities = 16;
        auto streamer = WorldStreamer(saved.document, loader, settings);
        for (int frame = 0; frame < 400; ++frame) {
            const auto x = frame % 2 ? 127.0 : 129.0; // straddling the line between cells 0 and 1
            streamer.set_sources({{x, 0.0, 64.0}});
            streamer.update(session->world(), &session->physics());
            REQUIRE(session->update(1.0 / 60.0).error.empty());
        }
        streamer.settle(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::active);
        CHECK(streamer.state({1, 0}) == CellState::active);
        CHECK(streamer.stats().deactivations == 0); // hysteresis: nothing went back down
        // Then away: everything unloads, and the World is back to its persistent part.
        streamer.set_sources({{9000.0, 0.0, 9000.0}});
        streamer.settle(session->world(), &session->physics());
        CHECK(session->world().size() == 2);
        CHECK(session->world().staged_count() == 0);
        CHECK(session->physics().stats().bodies_created == session->physics().stats().bodies_removed);
    }
}

TEST_CASE("A cross-cell reference resolves, becomes unresolved, and resolves again, and never keeps its cell loaded", "[streaming]") {
    auto saved = SavedWorld(1, 4);
    auto session = saved.session();
    auto settings = StreamingSettings{};
    settings.load_radius = 60.0;
    settings.activate_radius = 30.0;
    auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
    // The persistent camera watches a prop in cell (0, 0): a weak reference by EntityId.
    const auto target = id(uint64_t(uint32_t(1000)) << 16 | uint32_t(1000), 100);
    auto watch = session->world().commands();
    watch.add(*session->world().find(id(0, 1)),
              ScriptComponent{AssetRef<ScriptAsset>{}, {ScriptValue{"target", ScriptValueType::entity, target}}});
    REQUIRE(session->world().commit(watch));
    const auto resolve = [&] {
        auto value = EntityId{};
        session->world().with<ScriptComponent>(*session->world().find(id(0, 1)),
                                               [&](const ScriptComponent& script) { value = std::get<EntityId>(script.values.front().data); });
        return session->world().find(value);
    };
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    CHECK(resolve());
    streamer.set_sources({{5000.0, 0.0, 5000.0}});
    streamer.settle(session->world(), &session->physics());
    CHECK(streamer.state({0, 0}) == CellState::unloaded); // the reference did not keep it
    CHECK_FALSE(resolve()); // unresolved, never dangling
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    CHECK(resolve()); // a fresh handle to the same entity
}

TEST_CASE("Play's changes to a cell survive it unloading and loading again; a new session starts from the authored world", "[streaming]") {
    auto saved = SavedWorld(1, 4);
    auto settings = StreamingSettings{};
    settings.load_radius = 60.0;
    settings.activate_radius = 30.0;
    const auto key = uint64_t(uint32_t(1000)) << 16 | uint32_t(1000);
    const auto prop = id(key, 102), gone = id(key, 104);
    auto session = saved.session();
    auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    // Play moves one prop and destroys another (with its child).
    auto play = session->world().commands();
    play.set_transform(*session->world().find(prop), at(10, 20, 30));
    play.destroy(*session->world().find(gone));
    REQUIRE(session->world().commit(play));
    streamer.set_sources({{5000.0, 0.0, 5000.0}});
    streamer.settle(session->world(), &session->physics());
    REQUIRE(streamer.delta({0, 0}));
    CHECK(streamer.delta({0, 0})->destroyed.contains(gone));
    CHECK(streamer.delta({0, 0})->changed.contains(prop));
    CHECK(streamer.delta({0, 0})->changed.size() == 1); // only what changed
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    const auto back = session->world().find(prop);
    REQUIRE(back);
    CHECK(session->world().world_matrix(*back)->translation == math::DVec3{10, 20, 30});
    CHECK_FALSE(session->world().find(gone));
    CHECK_FALSE(session->world().find(id(key, 105))); // its child went with it
    // Play reset: a new session and streamer start from the authored world.
    session = saved.session();
    auto fresh = WorldStreamer(saved.document, saved.loader(), settings);
    fresh.set_sources({{64.0, 0.0, 64.0}});
    fresh.settle(session->world(), &session->physics());
    CHECK(session->world().find(gone));
    CHECK(session->world().world_matrix(*session->world().find(prop))->translation != math::DVec3{10, 20, 30});
}

TEST_CASE("A cell listing children before their parents activates whole, and an unloaded delta drops what a destroyed parent held", "[streaming]") {
    // One cell, written by hand: 100 trees of root, child, and grandchild, listed grandchildren first, so
    // parents arrive after their children in the file and across activation's commits.
    auto content = SceneDocument{};
    for (int level = 2; level >= 0; --level)
        for (uint64_t tree = 0; tree < 100; ++tree) {
            const auto self = id(1, tree * 3 + uint64_t(level));
            const auto parent = level == 0 ? std::nullopt : std::optional(id(1, tree * 3 + uint64_t(level) - 1));
            content.entities.push_back({self, parent, {NameComponent{"Node"}, at(level == 0 ? 10.0 + double(tree) : 0.0, 1.0, 0.0)}});
        }
    auto world_file = WorldDocument{};
    world_file.cells.push_back({{0, 0}, "cells/0_0.scene", uint32_t(content.entities.size())});
    const auto loader = [content](const WorldCell&, const JobContext&) { return CellLoad{content, {}, 1000}; };
    auto settings = StreamingSettings{};
    settings.frame_entities = 40; // over several commits and frames
    settings.load_radius = 60.0;
    settings.activate_radius = 30.0;
    World world;
    auto streamer = WorldStreamer(world_file, loader, settings);
    const auto stream_to = [&](math::DVec3 source, CellState wanted) {
        streamer.set_sources({source});
        for (int frame = 0; frame < 2000 && streamer.state({0, 0}) != wanted; ++frame) {
            streamer.update(world, nullptr);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        REQUIRE(streamer.state({0, 0}) == wanted);
    };
    stream_to({64.0, 0.0, 64.0}, CellState::active);
    REQUIRE(world.size() == 300);
    for (uint64_t tree = 0; tree < 100; ++tree) {
        const auto grandchild = world.find(id(1, tree * 3 + 2));
        REQUIRE(grandchild);
        CHECK(world.parent(*world.parent(*grandchild)) == world.find(id(1, tree * 3)));
        CHECK(world.world_matrix(*grandchild)->translation == math::DVec3{10.0 + double(tree), 3.0, 0.0});
    }
    // Play destroys a root; the cell unloads and comes back without the root or anything below it.
    auto play = world.commands();
    play.destroy(*world.find(id(1, 0)));
    REQUIRE(world.commit(play));
    stream_to({5000.0, 0.0, 5000.0}, CellState::unloaded);
    REQUIRE(streamer.delta({0, 0}));
    stream_to({64.0, 0.0, 64.0}, CellState::active);
    CHECK(world.size() == 297);
    CHECK_FALSE(world.find(id(1, 1)));
    CHECK_FALSE(world.find(id(1, 2)));
    streamer.unload_all(world, nullptr);
    CHECK(world.size() == 0);
}

TEST_CASE("Physics adds a cell's bodies in one batch when it activates and removes them when it goes", "[streaming][physics]") {
    auto saved = SavedWorld(1, 10);
    auto session = saved.session();
    auto settings = StreamingSettings{};
    settings.load_radius = 60.0;
    settings.activate_radius = 30.0;
    auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    const auto per_cell = size_t{2 + 2}; // ground, ball, and props 0 and 5
    CHECK(session->physics().stats().bodies_created == per_cell);
    // The ball falls onto its cell's ground and rests there.
    for (int tick = 0; tick < 180; ++tick) REQUIRE(session->update(1.0 / 60.0).error.empty());
    const auto ball = session->world().find(id(uint64_t(uint32_t(1000)) << 16 | uint32_t(1000), 2));
    REQUIRE(ball);
    CHECK(session->world().world_matrix(*ball)->translation.y == Approx(0.5).margin(0.05)); // within Jolt's penetration slop
    streamer.set_sources({{5000.0, 0.0, 5000.0}});
    streamer.settle(session->world(), &session->physics());
    CHECK(session->physics().stats().bodies_removed == per_cell);
    // The ball's resting place came back with the cell.
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    const auto again = session->world().find(id(uint64_t(uint32_t(1000)) << 16 | uint32_t(1000), 2));
    REQUIRE(again);
    CHECK(session->world().world_matrix(*again)->translation.y == Approx(0.5).margin(0.05)); // within Jolt's penetration slop
    REQUIRE(session->update(1.0 / 60.0).error.empty());
}

TEST_CASE("Pruning the cook cache keeps what loading reads now and removes the rest", "[streaming][cache]") {
    // A project with a texture, an environment, an imported model, and a world of 3 x 3 cells.
    const Folder folder;
    const auto write = [&](const std::string& name, const std::string& text) {
        fs::create_directories((folder.path / name).parent_path());
        std::ofstream(folder.path / name, std::ios::binary) << text;
    };
    const auto image = [](uint8_t seed) {
        auto rgba = std::vector<uint8_t>{};
        for (uint32_t i = 0; i < 16 * 8; ++i) rgba.insert(rgba.end(), {uint8_t(i * 7 + seed), uint8_t(i * 3), uint8_t(255 - i), 255});
        return test::encode_png_rgba(16, 8, rgba);
    };
    write("wood.png", image(1));
    write("wood.texture", "maya-texture 1\nsource \"wood.png\"\nusage color\ncompression rgba8\nmips on\n"
                          "filter linear linear\nmip_filter linear\nanisotropy 8\naddress repeat repeat\n");
    write("sky.hdr", test::radiance_file(test::environment_image(32, [](const math::Vec3& d) { return math::Vec3{1.0f + d.y}; })));
    write("sky.environment", "maya-environment 1\nsource \"sky.hdr\"\nspecular_size 16\nsamples 16\n");
    write("models/props.gltf", test::props_gltf());
    write("models/textures/normal.png", test::flat_normal_png());
    write("project.maya", "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n");
    write("catalog.maya", "maya-assets 1\ntexture 1 1 \"wood.texture\"\nenvironment 1 2 \"sky.environment\"\n");
    const auto world_file = folder.path / "levels/grid.world";
    fs::create_directories(world_file.parent_path());
    REQUIRE(save_world(world_file, grid_world(1, 2), any_asset).empty());
    auto opened = open_project(folder.path);
    REQUIRE(opened);
    const auto& project = opened.project;
    REQUIRE(import_gltf(project, "models/props.gltf"));
    opened = open_project(folder.path); // the import added its parts to the catalog
    REQUIRE(opened);

    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto limits = cook_limits(device); // what the device samples changes how textures cook
    const auto cache_folder = cook_cache_folder(project);
    const auto load_everything = [&] {
        auto cache = std::make_shared<CookCache>(cache_folder);
        auto assets = open_project_assets(opened.project, std::make_unique<FileAssetProvider>(device, cache));
        REQUIRE(assets);
        for (const auto& record : assets.registry->records()) {
            INFO(record.path.generic_string());
            if (record.kind == AssetKind::texture) CHECK(assets.registry->acquire(AssetRef<TextureAsset>{record.id}));
            else if (record.kind == AssetKind::environment) CHECK(assets.registry->acquire(AssetRef<EnvironmentAsset>{record.id}));
            else if (record.kind == AssetKind::mesh) CHECK(assets.registry->acquire(AssetRef<MeshAsset>{record.id}));
        }
        auto text = std::ifstream(world_file);
        const auto world = read_world(std::string(std::istreambuf_iterator<char>(text), {}));
        REQUIRE(world);
        const auto loader = cooked_cell_loader(world_file.parent_path(), cache, any_asset);
        for (const auto& cell : world.document->cells) {
            auto load = CellLoad{};
            job_system().submit(JobTier::frame, [&](JobContext& job) { load = loader(cell, job); }).wait();
            CHECK(load.document);
        }
        return std::pair{cache->stats(), assets.registry->records()};
    };
    const auto entries = [&] {
        auto names = std::set<std::string>{};
        for (const auto& entry : fs::recursive_directory_iterator(cache_folder))
            if (entry.is_regular_file() && entry.path().filename() != ".gitignore" && entry.path().filename() != "notes.txt")
                names.insert(entry.path().filename().string());
        return names;
    };
    // Cold: a texture, an environment, the model's parts, and 9 cells, each written once.
    const auto [cold, records] = load_everything();
    const auto written = entries();
    CHECK(cold.writes == written.size());
    CHECK(std::ranges::count_if(written, [](const std::string& name) { return name.ends_with(".cell"); }) == 9);
    // Everything there is reachable: pruning keeps it all, and leaves files that are not entries alone.
    write(".maya/cache/notes.txt", "not an entry");
    auto cache = std::make_shared<CookCache>(cache_folder);
    const auto keys = reachable_cook_keys(project, records, cache, limits);
    CHECK(keys.size() == written.size());
    auto kept = cache->prune(keys);
    CHECK(kept.removed == 0);
    CHECK(kept.kept == written.size());
    CHECK(cache->usage().entries == written.size());
    CHECK(cache->usage().bytes == kept.kept_bytes);
    // A changed image and a changed cell: loading cooks them again, and their old entries are unreachable.
    write("wood.png", image(2));
    auto cell_scene = std::ifstream(world_file.parent_path() / "grid/cells/0_0.scene");
    auto changed_cell = std::string(std::istreambuf_iterator<char>(cell_scene), {});
    cell_scene.close();
    changed_cell.replace(changed_cell.find("Prop 0"), 6, "Prop A");
    write("levels/grid/cells/0_0.scene", changed_cell);
    const auto [recooked, unused] = load_everything();
    CHECK(recooked.writes == 2);
    CHECK(entries().size() == written.size() + 2);
    cache = std::make_shared<CookCache>(cache_folder);
    const auto now = reachable_cook_keys(project, records, cache, limits);
    const auto dry = cache->prune(now, true);
    CHECK(dry.removed == 2);
    CHECK(entries().size() == written.size() + 2); // a dry run removes nothing
    const auto pruned = cache->prune(now);
    CHECK(pruned.removed == 2);
    CHECK(pruned.errors.empty());
    CHECK(entries().size() == written.size());
    CHECK(fs::exists(cache_folder / "notes.txt"));
    // Still warm: every load reads its entry, and nothing is cooked again.
    const auto [warm, again] = load_everything();
    CHECK(warm.writes == 0);
    CHECK(warm.hits == written.size());
    // Within its limit the cache is left as it is; past it, pruned; a limit of 0 never prunes.
    CHECK_FALSE(prune_cook_cache_over(project, records, cache, uint64_t{1} << 40, limits));
    CHECK_FALSE(prune_cook_cache_over(project, records, cache, 0, limits));
    const auto over = prune_cook_cache_over(project, records, cache, 1, limits);
    REQUIRE(over);
    CHECK(over->removed == 0);
    CHECK(project_cook_cache_limit(project.settings) == default_cook_cache_limit);
    device.shutdown();
}

TEST_CASE("Past the cells budget only cells that must activate load; the rest of the load radius waits", "[streaming][residency]") {
    auto saved = SavedWorld(2, 4); // 5 x 5 cells
    auto session = saved.session();
    auto settings = StreamingSettings{};
    settings.load_radius = 300.0; // most of the world
    settings.activate_radius = 30.0; // the middle cell
    settings.resident_bytes = 1; // already full
    {
        auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
        streamer.set_sources({{64.0, 0.0, 64.0}});
        streamer.settle(session->world(), &session->physics());
        CHECK(streamer.state({0, 0}) == CellState::active); // activating never waits on the budget
        CHECK(streamer.stats().cells[size_t(CellState::active)] == 1);
        CHECK(streamer.stats().cells[size_t(CellState::ready)] == 0); // nothing loaded ahead
        streamer.unload_all(session->world(), &session->physics());
    }
    settings.resident_bytes = size_t{64} << 20;
    auto streamer = WorldStreamer(saved.document, saved.loader(), settings);
    streamer.set_sources({{64.0, 0.0, 64.0}});
    streamer.settle(session->world(), &session->physics());
    CHECK(streamer.stats().cells[size_t(CellState::active)] == 1);
    CHECK(streamer.stats().cells[size_t(CellState::ready)] > 0); // within the budget, the ring loads ahead
    // The project's cells budget reaches the streamer.
    auto project = ProjectSettings{};
    project.resident[4] = 16; // resident_cells, MiB
    CHECK(project_streaming_settings(project).resident_bytes == size_t{16} << 20);
    CHECK(project_streaming_settings(ProjectSettings{}).resident_bytes == size_t{64} << 20);
}
