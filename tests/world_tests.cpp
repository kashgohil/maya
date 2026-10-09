#include "maya/world/world.hpp"
#include "maya/world/components.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
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

TEST_CASE("Command batches can drop their newest commands", "[world]") {
    World world;
    auto commands = world.commands();
    const auto kept = commands.create(EntityId{1, 1});
    commands.add(kept, NameComponent{"kept"});
    const auto size = commands.size();
    const auto dropped = commands.create(EntityId{1, 2});
    commands.add(dropped, NameComponent{"dropped"});
    commands.truncate(size);
    CHECK(commands.size() == size);
    commands.truncate(size + 10); // nothing beyond the end to drop
    const auto next = commands.create(EntityId{1, 3}); // the dropped create's index is reused
    CHECK(next.index == dropped.index);
    const auto result = world.commit(commands);
    REQUIRE(result);
    CHECK(world.size() == 2);
    CHECK(world.find(EntityId{1, 1}));
    CHECK_FALSE(world.find(EntityId{1, 2}));
    CHECK(world.find(EntityId{1, 3}));
}

TEST_CASE("The names revision counts commits that change names or the hierarchy, and no others", "[world]") {
    World world;
    const auto revision = [&] { return world.names_revision(); };
    auto commands = world.commands();
    const auto parent = commands.create();
    commands.add(parent, TransformComponent{});
    const auto child = commands.create();
    commands.add(child, TransformComponent{});
    auto created = world.commit(commands);
    REQUIRE(created);
    CHECK(revision() == 1); // created
    const auto a = created.created[parent.index], b = created.created[child.index];
    const auto changes = [&](auto&& stage) {
        const auto before = revision();
        auto batch = world.commands();
        stage(batch);
        REQUIRE(world.commit(batch));
        return revision() != before;
    };
    CHECK_FALSE(changes([&](WorldCommands& batch) { batch.set_transform(a, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}}); }));
    CHECK_FALSE(changes([&](WorldCommands& batch) { batch.replace(b, TransformComponent{{0, 2, 0}, {}, {1, 1, 1}}); }));
    CHECK_FALSE(changes([&](WorldCommands& batch) { batch.add(a, SpinComponent{}); }));
    CHECK(changes([&](WorldCommands& batch) { batch.add(a, NameComponent{"Arm"}); }));
    CHECK(changes([&](WorldCommands& batch) { batch.replace(a, NameComponent{"Leg"}); }));
    CHECK(changes([&](WorldCommands& batch) { batch.reparent(b, a, ReparentPolicy::keep_local); }));
    CHECK(changes([&](WorldCommands& batch) { batch.remove<NameComponent>(a); }));
    CHECK(changes([&](WorldCommands& batch) { batch.destroy(b); }));
    // A rejected commit changes nothing.
    const auto before = revision();
    auto rejected = world.commands();
    rejected.replace(b, NameComponent{"gone"});
    CHECK_FALSE(world.commit(rejected));
    CHECK(revision() == before);
}

TEST_CASE("Staged entities are committed but invisible until their group is published at once", "[world][stages]") {
    World world;
    const auto visible = create(world, EntityId{0x77, 1});
    // A cell's content arrives in two batches, staged under group 7.
    auto first = world.commands();
    const auto root = first.create_staged(EntityId{0x77, 10}, 7);
    first.add(root, TransformComponent{{100.0, 0.0, 0.0}});
    first.add(root, Counter{1});
    auto committed = world.commit(first);
    REQUIRE(committed);
    const auto root_handle = committed.created[root.index];
    // Nothing that reads the World sees it yet.
    CHECK_FALSE(world.find(EntityId{0x77, 10}));
    CHECK_FALSE(world.alive(root_handle));
    CHECK_FALSE(world.has<Counter>(root_handle));
    CHECK_FALSE(world.world_matrix(root_handle));
    CHECK(world.size() == 1);
    CHECK(world.staged_count() == 1);
    auto counted = 0;
    world.for_each<Counter>([&](EntityHandle, Counter&) { ++counted; });
    CHECK(counted == 0);
    world.for_each_entity([&](EntityHandle entity) { CHECK(entity == visible); });
    // A later batch of the same activation targets it: a child in the same group.
    auto second = world.commands();
    const auto child = second.create_staged(EntityId{0x77, 11}, 7);
    second.add(child, TransformComponent{{0.0, 1.0, 0.0}});
    second.add(child, Counter{2});
    second.reparent(child, root_handle, ReparentPolicy::keep_local);
    REQUIRE(world.commit(second));
    // Its ID is taken: no other entity can claim it meanwhile.
    auto clash = world.commands();
    clash.create(EntityId{0x77, 11});
    CHECK(world.commit(clash).error == WorldError::duplicate_id);
    // Hierarchies stay inside a group.
    auto across = world.commands();
    across.add(visible, TransformComponent{});
    across.reparent(visible, root_handle, ReparentPolicy::keep_local);
    CHECK(world.commit(across).error == WorldError::stage_mismatch);
    REQUIRE(world.staged(7).size() == 2);
    // Published, both appear together, with their hierarchy and poses.
    const auto published = world.publish(7);
    CHECK(published.size() == 2);
    CHECK(world.staged_count() == 0);
    CHECK(world.size() == 3);
    const auto found = world.find(EntityId{0x77, 11});
    REQUIRE(found);
    CHECK(world.parent(*found) == root_handle);
    CHECK(world.world_matrix(*found)->translation == math::DVec3{100.0, 1.0, 0.0});
    counted = 0;
    world.for_each<Counter>([&](EntityHandle, Counter&) { ++counted; });
    CHECK(counted == 2);
}

TEST_CASE("A discarded stage group leaves nothing behind, and its IDs can be used again", "[world][stages]") {
    World world;
    auto commands = world.commands();
    const auto a = commands.create_staged(EntityId{0x78, 1}, 3);
    commands.add(a, TransformComponent{});
    const auto b = commands.create_staged(EntityId{0x78, 2}, 3);
    commands.add(b, Counter{5});
    const auto other = commands.create_staged(EntityId{0x78, 3}, 4);
    REQUIRE(world.commit(commands));
    world.discard(3);
    CHECK(world.staged_count() == 1);
    CHECK(world.staged(3).empty());
    CHECK(world.component_count<Counter>() == 0);
    // The same IDs come back, as when a cancelled cell loads again.
    auto again = world.commands();
    again.create_staged(EntityId{0x78, 1}, 3);
    REQUIRE(world.commit(again));
    world.publish(3);
    CHECK(world.find(EntityId{0x78, 1}));
    CHECK_FALSE(world.find(EntityId{0x78, 3})); // group 4 is still staged
    (void)other;
}

TEST_CASE("Commits list what they destroyed, and the transform journal what moved", "[world][stages]") {
    World world;
    world.record_transform_changes(true);
    auto commands = world.commands();
    const auto parent = commands.create(EntityId{0x79, 1});
    commands.add(parent, TransformComponent{});
    const auto child = commands.create(EntityId{0x79, 2});
    commands.add(child, TransformComponent{});
    commands.reparent(child, parent, ReparentPolicy::keep_local);
    auto created = world.commit(commands);
    REQUIRE(created);
    CHECK(world.take_transform_changes().size() >= 2);
    CHECK(world.take_transform_changes().empty());
    auto move = world.commands();
    move.set_transform(created.created[parent.index], TransformComponent{{1, 0, 0}});
    REQUIRE(world.commit(move));
    const auto moved = world.take_transform_changes();
    REQUIRE(moved.size() == 1);
    CHECK(moved.front() == created.created[parent.index]); // its descendants moved with it
    auto remove = world.commands();
    remove.destroy(created.created[parent.index]);
    const auto removed = world.commit(remove);
    REQUIRE(removed);
    REQUIRE(removed.destroyed.size() == 2); // the subtree
    CHECK(std::ranges::count(removed.destroyed, created.created[child.index]) == 1);
    world.record_transform_changes(false);
}

TEST_CASE("Staging refuses a split hierarchy and changes nothing; edits name the group an entity came from", "[world][stages]") {
    World world;
    world.reserve(64); // room ahead: commits that follow do not grow the tables
    auto commands = world.commands();
    const auto parent = commands.create_staged(EntityId{0x7a, 1}, 5);
    commands.add(parent, TransformComponent{});
    const auto child = commands.create_staged(EntityId{0x7a, 2}, 5);
    commands.add(child, TransformComponent{{0, 1, 0}});
    commands.reparent(child, parent, ReparentPolicy::keep_local);
    const auto loose = commands.create(EntityId{0x7a, 3});
    commands.add(loose, Counter{1});
    const auto created = world.commit(commands);
    REQUIRE(created);
    world.publish(5);
    const auto p = created.created[parent.index], c = created.created[child.index], l = created.created[loose.index];
    // The child alone would leave its parent's hierarchy split: refused, and nothing is hidden.
    const auto alone = std::vector<EntityHandle>{c};
    CHECK(world.stage(alone, 9) == WorldError::stage_mismatch);
    CHECK(world.alive(c));
    CHECK(world.staged_count() == 0);
    CHECK(world.staged(9).empty());
    // Edits name the group an entity was published from; entities created visible are not recorded.
    world.record_edits(true);
    auto edit = world.commands();
    edit.set_transform(c, TransformComponent{{0, 2, 0}});
    edit.replace(l, Counter{2});
    REQUIRE(world.commit(edit));
    const auto edits = world.take_edits();
    REQUIRE(edits.size() == 1);
    CHECK(edits.front().id == EntityId{0x7a, 2});
    CHECK(edits.front().origin == 5);
    // The whole hierarchy, listed twice, stages; its destruction while staged is not an edit.
    const auto whole = std::vector<EntityHandle>{c, p, c};
    REQUIRE(world.stage(whole, 9) == WorldError::none);
    CHECK(world.staged(9).size() == 2);
    CHECK(world.size() == 1);
    CHECK(world.discard(9) == 0);
    CHECK(world.take_edits().empty());
    world.record_edits(false);
}
