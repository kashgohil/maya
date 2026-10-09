#include "maya/renderer/visibility_index.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <random>
#include <set>
#include <unordered_set>

using namespace maya;
using namespace maya::test;

namespace {

// Meshes whose size can change between loads, to reload one with other bounds.
class GrowingProvider final : public AssetProvider {
public:
    GrowingProvider(GraphicsDevice& device, std::shared_ptr<float> scale) : m_device(device), m_scale(std::move(scale)) {}
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override {
        auto geometry = unit_cube();
        const auto scale = path.filename() == "grow.mesh" ? *m_scale : path.filename() == "wide.mesh" ? 40.0f : 1.0f;
        for (auto& vertex : geometry.vertices) vertex.position = vertex.position * scale;
        auto mesh = std::make_unique<Mesh>(m_device, geometry.vertices, geometry.indices);
        return {std::make_shared<const MeshAsset>(std::move(mesh), MeshGeometry::from(geometry.vertices, geometry.indices)), {}};
    }
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path&) override {
        return {{}, {AssetError::invalid_data, "no materials"}};
    }

private:
    GraphicsDevice& m_device;
    std::shared_ptr<float> m_scale;
};

struct Fixture {
    NullGraphicsDevice device;
    std::shared_ptr<float> scale = std::make_shared<float>(1.0f);
    std::filesystem::path root;
    std::unique_ptr<AssetRegistry> registry;
    AssetRef<MeshAsset> cube{{0x7669, 1}}, wide{{0x7669, 2}}, grow{{0x7669, 3}}, late{{0x7669, 4}}, missing{{0x7669, 5}};

    Fixture() {
        REQUIRE(device.initialize(nullptr, {}));
        static std::atomic<int> counter{0};
        root = std::filesystem::temp_directory_path() / ("maya-visibility-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(root);
        registry = std::make_unique<AssetRegistry>(root, std::make_unique<GrowingProvider>(device, scale));
        const std::pair<AssetRef<MeshAsset>, const char*> meshes[] = {
            {cube, "cube.mesh"}, {wide, "wide.mesh"}, {grow, "grow.mesh"}, {late, "late.mesh"}};
        for (const auto& [ref, name] : meshes) {
            std::ofstream(root / name) << "inline\n";
            REQUIRE_FALSE(registry->register_asset(ref, name));
        }
        // `late` stays unloaded until a test loads it, and `missing` is never registered.
        for (const auto ref : {cube, wide, grow}) {
            const auto loaded = registry->acquire(ref);
            INFO(loaded.diagnostic.message);
            REQUIRE(loaded);
        }
    }
    ~Fixture() {
        registry.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// A volume like a shadow cascade's: an orthographic box looking down -z from `eye`.
math::Mat4 orthographic(float half_width, float half_height, float depth) {
    auto result = math::Mat4::identity();
    result.at(0, 0) = 1.0f / half_width;
    result.at(1, 1) = 1.0f / half_height;
    result.at(2, 2) = -1.0f / depth;
    return result;
}

struct Sphere {
    math::DVec3 center;
    float radius = 0.0f;
};

// What the index must return, worked out from the World alone: every visible mesh renderer whose bounds
// reach a volume, and every one it cannot bound. Entities within `margin` of a volume's edge may go either
// way, so the comparison does not depend on rounding.
struct Expected {
    std::set<EntityHandle> must, may;
};

Expected brute_force(const World& world, const AssetRegistry& assets, std::span<const Frustum> volumes,
                     const math::DVec3& origin, float margin = 1e-3f) {
    auto expected = Expected{};
    world.for_each<MeshRendererComponent>([&](EntityHandle entity, const MeshRendererComponent& renderer) {
        if (!renderer.visible || !renderer.mesh.valid()) return;
        const auto pose = world.world_matrix(entity);
        const auto lease = assets.resident(renderer.mesh);
        if (!pose || !lease || lease.value().geometry().empty()) {
            expected.must.insert(entity);
            return;
        }
        const auto& geometry = lease.value().geometry();
        auto sphere = Sphere{};
        const auto local = (geometry.min + geometry.max) * 0.5f;
        const auto& m = pose->linear;
        sphere.center = {pose->translation.x + double(m.at(0, 0) * local.x + m.at(0, 1) * local.y + m.at(0, 2) * local.z),
                         pose->translation.y + double(m.at(1, 0) * local.x + m.at(1, 1) * local.y + m.at(1, 2) * local.z),
                         pose->translation.z + double(m.at(2, 0) * local.x + m.at(2, 1) * local.y + m.at(2, 2) * local.z)};
        auto widest = 0.0f;
        for (int i = 0; i < 3; ++i) widest = std::max(widest, math::Vec3{m.at(0, i), m.at(1, i), m.at(2, i)}.length());
        sphere.radius = (geometry.max - geometry.min).length() * 0.5f * widest;
        const auto center = math::Vec3{float(sphere.center.x - origin.x), float(sphere.center.y - origin.y), float(sphere.center.z - origin.z)};
        const auto reaches = [&](float radius) {
            return std::ranges::any_of(volumes, [&](const Frustum& volume) { return volume.reaches(center, radius); });
        };
        if (reaches(std::max(0.0f, sphere.radius - margin))) expected.must.insert(entity);
        else if (reaches(sphere.radius + margin)) expected.may.insert(entity);
    });
    return expected;
}

void check_query(VisibilityIndex& index, const World& world, const AssetRegistry& assets, std::span<const Frustum> volumes,
                 const math::DVec3& origin) {
    auto found = std::vector<EntityHandle>{};
    index.query(volumes, origin, found);
    const auto returned = std::set<EntityHandle>(found.begin(), found.end());
    REQUIRE(returned.size() == found.size()); // each once
    CHECK(index.stats().returned == found.size());
    const auto expected = brute_force(world, assets, volumes, origin);
    for (const auto entity : expected.must) {
        INFO("missing entity slot " << entity.slot);
        CHECK(returned.contains(entity));
    }
    for (const auto entity : returned) {
        INFO("extra entity slot " << entity.slot);
        CHECK((expected.must.contains(entity) || expected.may.contains(entity)));
    }
}

math::Mat4 random_view(std::mt19937& random, const math::DVec3& origin) {
    auto real = [&](float low, float high) { return std::uniform_real_distribution<float>(low, high)(random); };
    const auto eye = math::Vec3{real(-250, 250), real(-20, 60), real(-250, 250)};
    auto target = math::Vec3{real(-250, 250), real(-30, 30), real(-250, 250)};
    if ((target - eye).length() < 1.0f) target = eye + math::Vec3{1, 0, 0};
    const auto up = std::abs((target - eye).normalized().y) > 0.99f ? math::Vec3{1, 0, 0} : math::Vec3{0, 1, 0};
    const auto relative_eye = eye - math::Vec3{float(origin.x), float(origin.y), float(origin.z)};
    const auto relative_target = target - math::Vec3{float(origin.x), float(origin.y), float(origin.z)};
    const auto view = math::Mat4::look_at(relative_eye, relative_target, up);
    if (std::uniform_int_distribution<int>(0, 3)(random) == 0)
        return orthographic(real(5, 120), real(5, 120), real(10, 400)) * view;
    return math::Mat4::perspective(real(0.05f, 2.4f), real(0.5f, 2.5f), real(0.05f, 2.0f), real(20.0f, 600.0f)) * view;
}

} // namespace

TEST_CASE("The visibility index returns what brute-force culling does through moves, creation, destruction, reparenting, and staging", "[renderer][visibility]") {
    Fixture fixture;
    auto& assets = *fixture.registry;
    World world;
    auto index = VisibilityIndex(world);
    const auto seed = GENERATE(1066u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
    INFO("seed " << seed);
    std::mt19937 random(seed);
    auto real = [&](float low, float high) { return std::uniform_real_distribution<float>(low, high)(random); };
    auto integer = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
    const AssetRef<MeshAsset> meshes[] = {fixture.cube, fixture.cube, fixture.cube, fixture.wide, fixture.grow, fixture.late, fixture.missing};

    // Positions crowd the cells' edges: on, just either side of, and far from the 32 m and 128 m lines.
    auto coordinate = [&]() -> double {
        switch (integer(0, 3)) {
        case 0: return real(-300, 300);
        case 1: return 32.0 * integer(-9, 9) + (integer(0, 1) ? 1.0 : -1.0) * double(real(0.0f, 0.01f));
        case 2: return 128.0 * integer(-2, 2) + (integer(0, 1) ? 1.0 : -1.0) * double(real(0.0f, 9.0f));
        default: return 32.0 * integer(-9, 9);
        }
    };
    auto transform = [&] {
        auto result = TransformComponent{};
        result.translation = {coordinate(), double(real(-20, 20)), coordinate()};
        result.rotation = math::Quat::from_axis_angle(math::Vec3{real(-1, 1), real(-1, 1), real(0.1f, 1)}.normalized(), real(-3, 3));
        const auto size = integer(0, 9) == 0 ? real(5, 30) : real(0.1f, 4);
        result.scale = integer(0, 4) == 0 ? math::Vec3{size, real(0.1f, 3), real(0.5f, 2)} : math::Vec3{size};
        return result;
    };
    auto renderer = [&] { return MeshRendererComponent{meshes[integer(0, 6)], {}, integer(0, 9) != 0}; };
    auto live = std::vector<EntityHandle>{};
    auto pick = [&] { return live[size_t(integer(0, int(live.size()) - 1))]; };
    auto refresh = [&] {
        live.clear();
        world.for_each_entity([&](EntityHandle entity) { live.push_back(entity); });
    };

    // A first population, some of it in hierarchies, before the index has ever looked.
    build_world(world, [&](WorldCommands& commands) {
        auto previous = std::vector<PendingEntity>{};
        for (int i = 0; i < 400; ++i) {
            auto entity = commands.create();
            commands.add(entity, transform());
            if (integer(0, 7) != 0) commands.add(entity, renderer());
            if (!previous.empty() && integer(0, 3) == 0) commands.reparent(entity, previous[size_t(integer(0, int(previous.size()) - 1))], ReparentPolicy::keep_local);
            previous.push_back(entity);
        }
    });
    refresh();

    const auto origin_for = [&](int round) { return round % 3 == 0 ? math::DVec3{} : math::DVec3{coordinate(), 0.0, coordinate()}; };
    auto staged_groups = std::vector<StageGroup>{};
    StageGroup next_group = 1;
    for (int round = 0; round < 60; ++round) {
        INFO("round " << round);
        if (round > 0) {
            // Most edits commit one at a time; some rounds put them all in one commit. An invalid edit (a
            // cycle, a destroyed target) fails its commit, which changes nothing.
            const auto batched = integer(0, 3) == 0;
            auto commands = world.commands();
            const auto edits = integer(1, 40);
            for (int e = 0; e < edits && !live.empty(); ++e) {
                const auto entity = pick();
                if (!world.alive(entity)) continue;
                const auto drawn = world.has<MeshRendererComponent>(entity);
                switch (integer(0, 9)) {
                case 0: case 1: case 2: commands.set_transform(entity, transform()); break;
                case 3: {
                    auto created = commands.create();
                    commands.add(created, transform());
                    commands.add(created, renderer());
                    if (integer(0, 1)) commands.reparent(created, entity, ReparentPolicy::keep_local);
                    break;
                }
                case 4: if (!batched) commands.destroy(entity); break;
                case 5: commands.reparent(entity, integer(0, 2) ? std::optional<EntityTarget>(pick()) : std::nullopt,
                                          integer(0, 1) ? ReparentPolicy::keep_world : ReparentPolicy::keep_local); break;
                case 6: // shown, hidden, or another mesh
                    if (drawn) commands.replace(entity, renderer());
                    else commands.add(entity, renderer());
                    break;
                case 7: if (drawn) commands.remove<MeshRendererComponent>(entity); break;
                case 8: if (world.has<TransformComponent>(entity)) commands.remove<TransformComponent>(entity); break;
                default: if (world.has<TransformComponent>(entity)) commands.set_transform(entity, transform()); break;
                }
                if (!batched) {
                    (void)world.commit(commands);
                    commands = world.commands();
                    if (!world.alive(entity)) refresh();
                }
            }
            if (batched && !world.commit(commands)) { // a cycle or a destroyed parent: move a few instead
                for (int e = 0; e < 10; ++e) {
                    auto single = world.commands();
                    single.set_transform(pick(), transform());
                    (void)world.commit(single);
                }
            }
            // Staging: entities created into a group, existing ones taken out into one, and groups published or discarded.
            switch (integer(0, 5)) {
            case 0: {
                auto staged = world.commands();
                const auto group = next_group++;
                for (int i = 0; i < 20; ++i) {
                    auto entity = staged.create_staged(EntityId::generate(), group);
                    staged.add(entity, transform());
                    staged.add(entity, renderer());
                }
                REQUIRE(world.commit(staged));
                staged_groups.push_back(group);
                break;
            }
            case 1: {
                refresh();
                auto taken = std::vector<EntityHandle>{};
                for (const auto entity : live)
                    if (!world.parent(entity) && world.children(entity).empty() && integer(0, 19) == 0) taken.push_back(entity);
                const auto group = next_group++;
                REQUIRE(world.stage(taken, group) == WorldError::none);
                staged_groups.push_back(group);
                break;
            }
            case 2:
                if (!staged_groups.empty()) {
                    (void)world.publish(staged_groups.back());
                    staged_groups.pop_back();
                }
                break;
            case 3:
                if (!staged_groups.empty()) {
                    (void)world.discard(staged_groups.front());
                    staged_groups.erase(staged_groups.begin());
                }
                break;
            default: break;
            }
            // Meshes become resident, or are reloaded with other bounds.
            if (round == 20) REQUIRE(assets.acquire(fixture.late));
            if (round == 30 || round == 45) {
                *fixture.scale = round == 30 ? 12.0f : 0.5f;
                REQUIRE(assets.reload(fixture.grow));
            }
            refresh();
        }
        index.update(world, assets);
        auto renderers = size_t{0};
        world.for_each<MeshRendererComponent>([&](EntityHandle, const MeshRendererComponent&) { ++renderers; });
        CHECK(index.stats().renderers == renderers);
        CHECK(index.stats().gridded + index.stats().large + index.stats().unbounded + index.stats().hidden == renderers);
        for (int q = 0; q < 6; ++q) {
            const auto origin = origin_for(round);
            auto volumes = std::vector<Frustum>{};
            for (int v = integer(1, 4); v > 0; --v) volumes.push_back(Frustum::from(random_view(random, origin)));
            check_query(index, world, assets, volumes, origin);
        }
    }
    CHECK(index.stats().large > 0);
    CHECK(index.stats().gridded > 0);
}

TEST_CASE("The visibility index costs nothing for what did not change, and finds each moved entity's subtree", "[renderer][visibility]") {
    Fixture fixture;
    auto& assets = *fixture.registry;
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        auto parent = commands.create();
        commands.add(parent, TransformComponent{{0, 0, 0}, {}, {1.0f}});
        commands.add(parent, MeshRendererComponent{fixture.cube, {}, true});
        auto child = commands.create();
        commands.add(child, TransformComponent{{2, 0, 0}, {}, {1.0f}});
        commands.add(child, MeshRendererComponent{fixture.cube, {}, true});
        commands.reparent(child, parent, ReparentPolicy::keep_local);
        for (int i = 0; i < 1000; ++i) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{{double(i % 40) * 10.0, 0, double(i / 40) * 10.0}, {}, {1.0f}});
            commands.add(entity, MeshRendererComponent{fixture.cube, {}, true});
        }
    });
    auto index = VisibilityIndex(world);
    index.update(world, assets);
    CHECK(index.stats().renderers == 1002);
    CHECK(index.stats().gridded == 1002);
    CHECK(index.stats().updated == 1002);
    index.update(world, assets);
    CHECK(index.stats().updated == 0);

    // Moving the parent far away moves its child out of view with it.
    auto commands = world.commands();
    commands.set_transform(created[0], TransformComponent{{5000, 0, 5000}, {}, {1.0f}});
    REQUIRE(world.commit(commands));
    index.update(world, assets);
    CHECK(index.stats().updated == 2);
    const auto frustum = Frustum::from(math::Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f) * math::Mat4::look_at({0, 5, -20}, {0, 0, 0}, {0, 1, 0}));
    auto found = std::vector<EntityHandle>{};
    index.query(std::span(&frustum, 1), {}, found);
    CHECK(std::ranges::find(found, created[0]) == found.end());
    CHECK(std::ranges::find(found, created[1]) == found.end());
    CHECK_FALSE(found.empty());
    check_query(index, world, assets, std::span(&frustum, 1), {});

    // With no volumes, only what it cannot bound comes back.
    auto unbounded = world.commands();
    auto entity = unbounded.create();
    unbounded.add(entity, TransformComponent{});
    unbounded.add(entity, MeshRendererComponent{fixture.missing, {}, true});
    const auto added = world.commit(unbounded);
    REQUIRE(added);
    index.update(world, assets);
    found.clear();
    index.query({}, {}, found);
    CHECK(found == added.created);
    CHECK(index.stats().unbounded == 1);
}

TEST_CASE("A visibility index outlives its World, and refuses another World", "[renderer][visibility]") {
    Fixture fixture;
    auto world = std::make_unique<World>();
    auto index = std::make_unique<VisibilityIndex>(*world);
    index->update(*world, *fixture.registry);
    World other;
    CHECK_THROWS_AS(index->update(other, *fixture.registry), std::invalid_argument);
    world.reset();
    index.reset(); // no use of the World it indexed
}

TEST_CASE("A visibility index starts again from the World when its change log overflowed", "[renderer][visibility]") {
    Fixture fixture;
    auto& assets = *fixture.registry;
    World world;
    auto index = VisibilityIndex(world);
    index.update(world, assets);
    CHECK(index.stats().rebuilds == 1);
    // More changes than the World has slots, unread: the log reports everything instead.
    for (int batch = 0; batch < 4; ++batch) {
        build_world(world, [&](WorldCommands& commands) {
            for (int i = 0; i < 400; ++i) {
                auto entity = commands.create();
                commands.add(entity, TransformComponent{{double(i) * 3.0, 0.0, double(batch) * 40.0}, {}, {1.0f}});
                commands.add(entity, MeshRendererComponent{fixture.cube, {}, true});
            }
        });
    }
    index.update(world, assets);
    CHECK(index.stats().rebuilds == 2);
    CHECK(index.stats().renderers == 1600);
    const auto frustum = Frustum::from(math::Mat4::perspective(1.2f, 1.5f, 0.1f, 500.0f) * math::Mat4::look_at({600, 30, 60}, {600, 0, 0}, {0, 1, 0}));
    check_query(index, world, assets, std::span(&frustum, 1), {});
    index.update(world, assets);
    CHECK(index.stats().rebuilds == 2);
    CHECK(index.stats().updated == 0);
}
