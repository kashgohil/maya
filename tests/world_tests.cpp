#include "maya/world/world.hpp"
#include "maya/world/components.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace maya;
struct Counter { int value = 0; };
struct Marker { int value = 0; };
struct Owned {
    std::unique_ptr<int> value;
    std::shared_ptr<int> lease;
};
struct ThrowingCopy {
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) { throw std::runtime_error("staging copy"); }
    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(ThrowingCopy&&) noexcept = default;
};
struct ThrowingMove {
    ThrowingMove(ThrowingMove&&) noexcept(false);
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false);
};
static_assert(Component<Owned> && Component<ThrowingCopy>);
static_assert(!Component<ThrowingMove> && !Component<const Counter>);
static_assert(!std::is_copy_constructible_v<World> && !std::is_move_constructible_v<World>);
static_assert(!std::is_convertible_v<EntityId, AssetId>);
static_assert(!std::is_convertible_v<AssetRef<MeshAsset>, AssetRef<MaterialAsset>>);

EntityHandle create(World& world, EntityId id = EntityId::generate()) {
    auto commands = world.commands();
    const auto pending = commands.create(id);
    auto result = world.commit(commands);
    REQUIRE(result);
    return result.created[pending.index];
}

TEST_CASE("World publishes entities and initial component schemas without a device", "[world]") {
    auto world = World{};
    auto commands = world.commands();
    const auto id = EntityId{42, 17};
    const auto pending = commands.create(id);
    commands.add(pending, NameComponent{"Camera"});
    commands.add(pending, TransformComponent{});
    commands.add(pending, CameraComponent{});
    commands.add(pending, MeshRendererComponent{{AssetId{3, 4}}, {AssetId{5, 6}}, true});
    commands.add(pending, LightComponent{});
    CHECK(world.size() == 0);
    CHECK_FALSE(world.find(id));
    const auto result = world.commit(commands);
    REQUIRE(result);
    const auto entity = result.created[pending.index];
    REQUIRE(world.alive(entity));
    CHECK(world.persistent_id(entity) == id);
    CHECK(world.find(id) == entity);
    CHECK(world.size() == 1);
    CHECK(world.has<TransformComponent>(entity));
    CHECK(world.with<NameComponent>(entity, [](auto& name) { name.value = "Main camera"; }));
    const auto& read = std::as_const(world);
    CHECK(read.with<NameComponent>(entity, [](const auto& name) { CHECK(name.value == "Main camera"); }));
    CHECK(read.with<TransformComponent>(entity, [](const auto& transform) {
        CHECK(transform.translation.x == 0.0f);
        CHECK(transform.rotation.w == 1.0f);
        CHECK(transform.scale.x == 1.0f);
    }));
    CHECK(read.with<CameraComponent>(entity, [](const auto& camera) {
        CHECK(camera.vertical_fov == math::PI / 3.0f);
        CHECK(camera.near_clip > 0.0f);
        CHECK(camera.far_clip > camera.near_clip);
    }));
    CHECK(read.with<MeshRendererComponent>(entity, [](const auto& mesh) {
        CHECK(mesh.mesh.id == AssetId{3, 4});
        CHECK(mesh.material.id == AssetId{5, 6});
    }));
    CHECK(read.with<LightComponent>(entity, [](const auto& light) {
        CHECK(light.kind == LightKind::directional);
        CHECK(light.enabled);
    }));
    CHECK_THROWS_AS(commands.create(), std::logic_error);
    CHECK(world.commit(commands).error == WorldError::wrong_world);
}

TEST_CASE("World identities reject invalid stale and cross-world handles", "[world]") {
    auto world = World{};
    auto other = World{};
    const auto id = EntityId{8, 9};
    const auto first = create(world, id);
    const auto foreign = create(other, id);
    CHECK_FALSE(world.alive({}));
    CHECK_FALSE(world.alive(foreign));
    CHECK_FALSE(other.alive(first));
    CHECK_FALSE(world.persistent_id(foreign));
    CHECK_FALSE(world.alive({world.token(), invalid_entity_slot, 1}));
    CHECK_FALSE(world.alive({world.token(), first.slot, 0}));
    auto remove = world.commands();
    remove.destroy(first);
    REQUIRE(world.commit(remove));
    CHECK_FALSE(world.alive(first));
    CHECK_FALSE(world.find(id));
    const auto replacement = create(world, id);
    CHECK(replacement.slot == first.slot);
    CHECK(replacement.generation != first.generation);
    CHECK_FALSE(world.alive(first));
    CHECK(world.find(id) == replacement);
    auto stale = world.commands();
    stale.add(first, Counter{99});
    CHECK(world.commit(stale).error == WorldError::invalid_entity);
    CHECK_FALSE(world.has<Counter>(replacement));
    auto cross = world.commands();
    cross.destroy(foreign);
    CHECK(world.commit(cross).error == WorldError::wrong_world);
    CHECK(other.alive(foreign));
}

TEST_CASE("World reconstruction preserves persistent IDs without reviving runtime handles", "[world]") {
    auto first = std::make_unique<World>();
    const auto entity = create(*first, EntityId{123, 456});
    const auto saved = first->persistent_id(entity);
    REQUIRE(saved);
    auto obsolete = first->commands();
    obsolete.destroy(entity);
    first.reset();
    for (int session = 0; session < 10; ++session) {
        auto restored = World{};
        const auto loaded = create(restored, *saved);
        CHECK(restored.persistent_id(loaded) == saved);
        CHECK(restored.find(*saved) == loaded);
        CHECK_FALSE(restored.alive(entity));
        CHECK(restored.commit(obsolete).error == WorldError::wrong_world);
    }
}

TEST_CASE("World rejects invalid IDs and duplicate reconstruction atomically", "[world]") {
    auto world = World{};
    const auto existing = create(world, {1, 2});
    auto batch = world.commands();
    batch.destroy(existing);
    batch.create({3, 4});
    SECTION("zero ID") { batch.create({}); }
    SECTION("duplicate live ID") { batch.create({1, 2}); }
    SECTION("duplicate staged ID") { batch.create({3, 4}); }
    const auto result = world.commit(batch);
    CHECK_FALSE(result);
    CHECK(result.command_index == 2);
    CHECK(result.created.empty());
    CHECK(world.size() == 1);
    CHECK(world.alive(existing));
    CHECK_FALSE(world.find({3, 4}));
}

TEST_CASE("World component transactions validate sequential state before publication", "[world]") {
    auto world = World{};
    const auto entity = create(world);
    auto seed = world.commands();
    seed.add(entity, Counter{1});
    REQUIRE(world.commit(seed));
    auto replace = world.commands();
    replace.remove<Counter>(entity);
    replace.add(entity, Counter{2});
    REQUIRE(world.commit(replace));
    CHECK(world.with<Counter>(entity, [](const auto& counter) { CHECK(counter.value == 2); }));
    auto bad = world.commands();
    const auto new_entity = bad.create({7, 8});
    bad.add(new_entity, Counter{3});
    SECTION("duplicate add") { bad.add(entity, Counter{4}); }
    SECTION("missing remove") { bad.remove<Marker>(entity); }
    SECTION("duplicate remove") { bad.remove<Counter>(entity); bad.remove<Counter>(entity); }
    SECTION("operation after destroy") { bad.destroy(entity); bad.add(entity, Marker{}); }
    CHECK_FALSE(world.commit(bad));
    CHECK(world.size() == 1);
    CHECK_FALSE(world.find({7, 8}));
    CHECK(world.with<Counter>(entity, [](const auto& counter) { CHECK(counter.value == 2); }));
    CHECK_FALSE(world.with<Marker>(entity, [](auto&) { FAIL("Missing component callback ran"); }));
}

TEST_CASE("Pending entities cannot escape their command buffer or precede creation", "[world]") {
    auto world = World{};
    auto first = world.commands();
    const auto pending = first.create();
    auto second = world.commands();
    second.add(pending, Counter{});
    CHECK(world.commit(second).error == WorldError::invalid_pending_entity);
    SECTION("fabricated future target") {
        first.add(PendingEntity{pending.batch, pending.index + 1}, Counter{});
        first.create();
        CHECK(world.commit(first).error == WorldError::invalid_pending_entity);
        CHECK(world.size() == 0);
    }
    SECTION("buffer move keeps pending identity") {
        auto moved = std::move(first);
        moved.add(pending, Counter{5});
        CHECK_THROWS_AS(first.destroy(pending), std::logic_error);
        const auto result = world.commit(moved);
        REQUIRE(result);
        CHECK(world.has<Counter>(result.created[pending.index]));
    }
}

TEST_CASE("Move-only component resources release on remove destruction rollback and World teardown", "[world]") {
    auto lease = std::make_shared<int>(7);
    auto weak = std::weak_ptr<int>{lease};
    {
        auto world = World{};
        const auto entity = create(world);
        auto add = world.commands();
        add.add(entity, Owned{std::make_unique<int>(42), std::move(lease)});
        REQUIRE(world.commit(add));
        CHECK_FALSE(weak.expired());
        CHECK(world.with<Owned>(entity, [](const auto& owned) { CHECK(*owned.value == 42); }));
        SECTION("component removal") {
            auto remove = world.commands();
            remove.remove<Owned>(entity);
            REQUIRE(world.commit(remove));
            CHECK(weak.expired());
            CHECK(world.alive(entity));
        }
        SECTION("entity destruction") {
            auto remove = world.commands();
            remove.destroy(entity);
            REQUIRE(world.commit(remove));
            CHECK(weak.expired());
        }
        SECTION("World teardown") { CHECK_FALSE(weak.expired()); }
    }
    CHECK(weak.expired());
    auto world = World{};
    auto staged = std::weak_ptr<int>{};
    {
        auto payload = std::make_shared<int>(3);
        staged = payload;
        auto invalid = world.commands();
        const auto pending = invalid.create();
        invalid.add(pending, Owned{std::make_unique<int>(1), std::move(payload)});
        invalid.remove<Marker>(pending);
        CHECK_FALSE(world.commit(invalid));
        CHECK(world.size() == 0);
        CHECK_FALSE(staged.expired());
    }
    CHECK(staged.expired());
}

TEST_CASE("Throwing component construction and callbacks leave the World usable", "[world]") {
    auto world = World{};
    const auto entity = create(world);
    auto batch = world.commands();
    const auto value = ThrowingCopy{};
    CHECK_THROWS_AS(batch.add(entity, value), std::runtime_error);
    CHECK(batch.size() == 0);
    batch.add(entity, Counter{1});
    REQUIRE(world.commit(batch));
    CHECK_THROWS_AS(world.with<Counter>(entity, [](auto&) { throw std::runtime_error("callback"); }),
        std::runtime_error);
    CHECK_THROWS_AS(world.for_each<Counter>([](auto, auto&) { throw std::runtime_error("query"); }),
        std::runtime_error);
    auto remove = world.commands();
    remove.destroy(entity);
    REQUIRE(world.commit(remove));
}

TEST_CASE("Queries borrow components and defer structural changes until an explicit commit", "[world]") {
    auto world = World{};
    auto batch = world.commands();
    for (int i = 0; i < 20; ++i) {
        const auto entity = batch.create();
        batch.add(entity, Counter{i});
        if (i % 2 == 0) batch.add(entity, Marker{i});
    }
    REQUIRE(world.commit(batch));
    auto remove = world.commands();
    auto visited = std::set<EntityHandle>{};
    world.for_each<Counter, Marker>([&](auto entity, auto& counter, auto& marker) {
        CHECK(visited.insert(entity).second);
        CHECK(counter.value == marker.value);
        counter.value += 100;
        remove.destroy(entity);
        CHECK(world.commit(remove).error == WorldError::busy);
        CHECK(world.alive(entity));
    });
    CHECK(visited.size() == 10);
    CHECK(world.size() == 20);
    REQUIRE(world.commit(remove));
    CHECK(world.size() == 10);
    CHECK(world.component_count<Marker>() == 0);
    auto sum = 0;
    std::as_const(world).for_each<Counter>([&](auto, auto& counter) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(counter)>>);
        sum += counter.value;
    });
    CHECK(sum == 100);
    world.for_each<Marker>([](auto, auto&) { FAIL("Empty pool was visited"); });
}

TEST_CASE("Nested borrows and entity enumeration block commits until the outer callback returns", "[world]") {
    auto world = World{};
    const auto entity = create(world);
    auto seed = world.commands();
    seed.add(entity, Counter{});
    REQUIRE(world.commit(seed));
    auto remove = world.commands();
    remove.destroy(entity);
    world.for_each_entity([&](auto current) {
        CHECK(current == entity);
        REQUIRE(world.with<Counter>(current, [&](auto&) {
            CHECK(world.commit(remove).error == WorldError::busy);
        }));
        CHECK(world.commit(remove).error == WorldError::busy);
    });
    REQUIRE(world.commit(remove));
}

TEST_CASE("Swap removal preserves component ownership for surviving handles", "[world]") {
    auto world = World{};
    auto seed = world.commands();
    for (int value : {10, 20, 30}) {
        const auto entity = seed.create();
        seed.add(entity, Counter{value});
        seed.add(entity, Marker{value});
    }
    const auto result = world.commit(seed);
    REQUIRE(result);
    const auto first = result.created[0];
    const auto middle = result.created[1];
    const auto last = result.created[2];
    auto remove = world.commands();
    remove.remove<Counter>(middle);
    remove.destroy(first);
    REQUIRE(world.commit(remove));
    CHECK_FALSE(world.has<Counter>(middle));
    CHECK(world.with<Marker>(middle, [](const auto& value) { CHECK(value.value == 20); }));
    CHECK(world.with<Counter>(last, [](const auto& value) { CHECK(value.value == 30); }));
    CHECK(world.with<Marker>(last, [](const auto& value) { CHECK(value.value == 30); }));
    auto add = world.commands();
    const auto pending = add.create();
    add.add(pending, Counter{40});
    const auto added = world.commit(add);
    REQUIRE(added);
    CHECK(added.created[0].slot == first.slot);
    CHECK_FALSE(world.alive(first));
    CHECK(world.with<Counter>(last, [](const auto& value) { CHECK(value.value == 30); }));
    CHECK(world.with<Counter>(added.created[0], [](const auto& value) { CHECK(value.value == 40); }));
}

TEST_CASE("Transient creations and abandoned buffers do not leave live state", "[world]") {
    auto world = World{};
    {
        auto abandoned = world.commands();
        const auto pending = abandoned.create({1, 1});
        abandoned.add(pending, Counter{1});
    }
    CHECK(world.size() == 0);
    CHECK_FALSE(world.find({1, 1}));
    auto batch = world.commands();
    const auto pending = batch.create({1, 1});
    batch.add(pending, Counter{2});
    batch.destroy(pending);
    const auto result = world.commit(batch);
    REQUIRE(result);
    CHECK_FALSE(world.alive(result.created[0]));
    CHECK_FALSE(world.find({1, 1}));
    CHECK(world.component_count<Counter>() == 0);
    const auto restored = create(world, {1, 1});
    CHECK(world.alive(restored));
    CHECK(restored != result.created[0]);
}

TEST_CASE("Packed storage stays consistent across growth removal and slot reuse", "[world][scale]") {
    auto world = World{};
    constexpr auto count = 10000;
    auto all_ids = std::set<EntityId>{};
    auto old_handles = std::vector<EntityHandle>{};
    for (int cycle = 0; cycle < 3; ++cycle) {
        auto spawn = world.commands();
        for (int i = 0; i < count; ++i) {
            const auto id = EntityId::generate();
            REQUIRE(all_ids.insert(id).second);
            const auto entity = spawn.create(id);
            spawn.add(entity, Counter{i});
            if (i % 3 == 0) spawn.add(entity, Marker{i});
        }
        const auto result = world.commit(spawn);
        REQUIRE(result);
        CHECK(world.size() == count);
        for (const auto stale : old_handles) REQUIRE_FALSE(world.alive(stale));
        auto sum = int64_t{0};
        world.for_each<Counter>([&](auto entity, auto& counter) {
            REQUIRE(world.find(*world.persistent_id(entity)) == entity);
            sum += counter.value;
        });
        CHECK(sum == int64_t{count} * (count - 1) / 2);
        auto remove = world.commands();
        for (int parity = 0; parity < 2; ++parity)
            for (int i = parity; i < count; i += 2) remove.destroy(result.created[i]);
        REQUIRE(world.commit(remove));
        CHECK(world.size() == 0);
        CHECK(world.component_count<Counter>() == 0);
        CHECK(world.component_count<Marker>() == 0);
        old_handles = result.created;
    }
}
} // namespace
