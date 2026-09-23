#include "maya/world/world.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <limits>
#include <random>
#include <type_traits>

namespace {
using namespace maya;
using Catch::Approx;

void matrix_close(const math::Mat4& actual, const math::Mat4& expected, float tolerance = 2e-4f) {
    for (int i = 0; i < 16; ++i)
        CHECK(actual.elements[i] == Approx(expected.elements[i]).margin(tolerance).epsilon(tolerance));
}
EntityHandle spatial_entity(World& world, TransformComponent local = {}) {
    auto commands = world.commands();
    const auto pending = commands.create();
    commands.add(pending, local);
    const auto result = world.commit(commands);
    REQUIRE(result);
    return result.created[pending.index];
}
void attach(World& world, EntityHandle child, std::optional<EntityTarget> parent,
            ReparentPolicy policy = ReparentPolicy::keep_local) {
    auto commands = world.commands();
    commands.reparent(child, parent, policy);
    REQUIRE(world.commit(commands));
}
math::Mat4 matrix(const World& world, EntityHandle entity) {
    const auto value = world.world_matrix(entity);
    REQUIRE(value);
    return *value;
}

TEST_CASE("Nested TRS preserves inherited shear and propagates edits", "[world][spatial]") {
    World world;
    const TransformComponent a{{3,4,5}, math::Quat::from_axis_angle({0,1,0}, 0.7f), {2,3,4}};
    const TransformComponent b{{1,2,3}, math::Quat::from_axis_angle({0,0,1}, 0.4f), {1,2,1}};
    const TransformComponent c{{-1,0,1}, {}, {1,1,2}};
    auto commands = world.commands();
    const auto pa = commands.create(), pb = commands.create(), pc = commands.create();
    commands.add(pa, a); commands.add(pb, b); commands.add(pc, c);
    commands.reparent(pb, pa, ReparentPolicy::keep_local);
    commands.reparent(pc, pb, ReparentPolicy::keep_local);
    const auto result = world.commit(commands);
    REQUIRE(result);
    const auto root = result.created[0], child = result.created[1], leaf = result.created[2];
    CHECK(world.parent(child) == root);
    CHECK(world.children(root) == std::vector{child});
    CHECK_FALSE(world.parent(root));
    matrix_close(matrix(world, leaf), local_matrix(a)*local_matrix(b)*local_matrix(c));
    CHECK_FALSE(decompose_transform(matrix(world, child))); // retained shear
    auto moved = a; moved.translation.x = 11;
    auto edits = world.commands(); edits.set_transform(root, moved);
    REQUIRE(world.commit(edits));
    matrix_close(matrix(world, leaf), local_matrix(moved)*local_matrix(b)*local_matrix(c));
    matrix_close(matrix(world, leaf), local_matrix(moved)*local_matrix(b)*local_matrix(c));
    world.with<TransformComponent>(leaf, [](auto& value) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(value)>>);
    });
    world.for_each<TransformComponent>([](auto, auto& value) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(value)>>);
    });
}

TEST_CASE("Reparent policies use the sequential transaction pose", "[world][spatial]") {
    World world;
    const auto a = spatial_entity(world, {{4,0,0}, {}, {2,2,2}});
    const auto b = spatial_entity(world, {{0,3,0}, math::Quat::from_axis_angle({0,1,0}, 1), {1,1,1}});
    const auto child = spatial_entity(world, {{1,0,0}});
    const auto leaf = spatial_entity(world, {{0,0,-2}});
    attach(world, child, a); attach(world, leaf, child);
    const auto original = matrix(world, leaf);
    attach(world, child, b, ReparentPolicy::keep_world);
    matrix_close(matrix(world, leaf), original);
    attach(world, child, std::nullopt, ReparentPolicy::keep_world);
    matrix_close(matrix(world, leaf), original);
    auto edits = world.commands();
    edits.set_transform(child, {{7,8,9}});
    edits.reparent(child, a, ReparentPolicy::keep_world);
    edits.set_transform(a, {{10,0,0}, {}, {2,2,2}});
    REQUIRE(world.commit(edits));
    CHECK(matrix(world, child).at(0,3) == Approx(13));
    CHECK(matrix(world, child).at(1,3) == Approx(8));
    CHECK(world.children(b).empty());
    CHECK(world.children(a) == std::vector{child});
}

TEST_CASE("Invalid hierarchy transactions preserve components topology and caches", "[world][spatial]") {
    World world, other;
    const auto root = spatial_entity(world);
    const auto child = spatial_entity(world, {{1,0,0}});
    const auto foreign = spatial_entity(other);
    attach(world, child, root);
    const auto original = matrix(world, child);
    SECTION("cycle") {
        auto edits = world.commands();
        edits.set_transform(root, {{99,0,0}});
        edits.reparent(root, child, ReparentPolicy::keep_local);
        const auto result = world.commit(edits);
        CHECK(result.error == WorldError::hierarchy_cycle);
        CHECK(result.command_index == 1);
    }
    SECTION("self") {
        auto edits = world.commands(); edits.reparent(root, root, ReparentPolicy::keep_local);
        CHECK(world.commit(edits).error == WorldError::hierarchy_cycle);
    }
    SECTION("foreign") {
        auto edits = world.commands(); edits.reparent(child, foreign, ReparentPolicy::keep_local);
        CHECK(world.commit(edits).error == WorldError::wrong_world);
    }
    SECTION("stale parent") {
        const auto dead = spatial_entity(world);
        auto destroy = world.commands(); destroy.destroy(dead); REQUIRE(world.commit(destroy));
        const auto reused = spatial_entity(world); CHECK(reused.slot == dead.slot);
        auto edits = world.commands(); edits.reparent(child, dead, ReparentPolicy::keep_local);
        CHECK(world.commit(edits).error == WorldError::invalid_entity);
    }
    SECTION("wrong pending batch") {
        auto unused = world.commands(); const auto pending = unused.create();
        auto edits = world.commands(); edits.reparent(child, pending, ReparentPolicy::keep_local);
        CHECK(world.commit(edits).error == WorldError::invalid_pending_entity);
    }
    SECTION("remove parent transform") {
        auto edits = world.commands(); edits.remove<TransformComponent>(root);
        CHECK(world.commit(edits).error == WorldError::hierarchy_in_use);
    }
    SECTION("remove child transform") {
        auto edits = world.commands(); edits.remove<TransformComponent>(child);
        CHECK(world.commit(edits).error == WorldError::hierarchy_in_use);
    }
    SECTION("shear on keep world") {
        const auto scaled = spatial_entity(world, {{}, {}, {2,1,1}});
        auto edits = world.commands();
        edits.set_transform(child, {{}, math::Quat::from_axis_angle({0,0,1}, .7f)});
        edits.reparent(child, scaled, ReparentPolicy::keep_world);
        CHECK(world.commit(edits).error == WorldError::unrepresentable_transform);
        CHECK(world.children(scaled).empty());
    }
    SECTION("destroyed child referenced later") {
        auto edits = world.commands(); edits.destroy(root); edits.set_transform(child, {});
        CHECK(world.commit(edits).error == WorldError::invalid_entity);
    }
    matrix_close(matrix(world, child), original);
    CHECK(world.parent(child) == root);
    CHECK(world.children(root) == std::vector{child});
}

TEST_CASE("Subtree destruction releases resources and explicit detach preserves descendants", "[world][spatial]") {
    struct Resource { std::shared_ptr<int> lease; };
    auto lease = std::make_shared<int>(1);
    World world;
    const auto root = spatial_entity(world), a = spatial_entity(world), b = spatial_entity(world);
    const auto leaf = spatial_entity(world);
    attach(world, a, root); attach(world, b, root); attach(world, leaf, a);
    auto add = world.commands(); add.add(a, Resource{lease}); add.add(leaf, Resource{lease});
    REQUIRE(world.commit(add)); CHECK(lease.use_count() == 3);
    const auto old_id = world.persistent_id(a);
    auto edits = world.commands();
    edits.reparent(b, std::nullopt, ReparentPolicy::keep_world);
    edits.destroy(root);
    REQUIRE(world.commit(edits));
    CHECK(world.size() == 1); CHECK(world.alive(b));
    CHECK_FALSE(world.alive(root)); CHECK_FALSE(world.alive(a)); CHECK_FALSE(world.alive(leaf));
    CHECK_FALSE(world.find(*old_id)); CHECK_FALSE(world.world_matrix(a));
    CHECK_FALSE(world.parent(b)); CHECK(lease.use_count() == 1);
    const auto fresh = spatial_entity(world);
    CHECK(world.children(fresh).empty()); CHECK_FALSE(world.parent(fresh));
    matrix_close(matrix(world, fresh), math::Mat4::identity());
}

TEST_CASE("Sibling relinks removal and replacement respect command order", "[world][spatial]") {
    World world;
    const auto root = spatial_entity(world);
    const auto a = spatial_entity(world), b = spatial_entity(world), c = spatial_entity(world);
    attach(world, a, root); attach(world, b, root); attach(world, c, root);
    auto edits = world.commands();
    edits.reparent(b, std::nullopt, ReparentPolicy::keep_local); // middle
    edits.destroy(c); // head
    edits.reparent(a, b, ReparentPolicy::keep_local); // tail
    edits.remove<TransformComponent>(root);
    edits.add(root, TransformComponent{{9,0,0}});
    edits.reparent(b, root, ReparentPolicy::keep_local);
    REQUIRE(world.commit(edits));
    CHECK(world.children(root) == std::vector{b}); CHECK(world.children(b) == std::vector{a});
    CHECK(matrix(world, a).at(0,3) == Approx(9));
    auto remove = world.commands(); remove.reparent(a, std::nullopt, ReparentPolicy::keep_local);
    remove.remove<TransformComponent>(a); REQUIRE(world.commit(remove));
    CHECK_FALSE(world.world_matrix(a));
    CHECK(world.children(b).empty());
}

TEST_CASE("Transform validation handles extreme quaternions and rejects invalid authored data", "[world][spatial]") {
    const auto huge = std::numeric_limits<float>::max();
    const auto tiny = std::numeric_limits<float>::denorm_min();
    for (const auto length : {huge, tiny, 2.0f}) {
        const auto value = validated_transform({{}, {0,0,0,length}, {1,1,1}});
        REQUIRE(value); CHECK(value->rotation.w == 1);
    }
    World world;
    const auto entity = spatial_entity(world);
    auto invalids = std::vector<TransformComponent>{};
    auto value = TransformComponent{}; value.rotation = {0,0,0,0}; invalids.push_back(value);
    value = {}; value.scale.x = 0; invalids.push_back(value);
    value.scale.x = -1; invalids.push_back(value);
    value.scale.x = tiny; invalids.push_back(value); // inverse exceeds float range
    value = {}; value.translation.x = std::numeric_limits<float>::infinity(); invalids.push_back(value);
    value = {}; value.rotation.y = std::numeric_limits<float>::quiet_NaN(); invalids.push_back(value);
    for (const auto& invalid : invalids) {
        auto edits = world.commands(); edits.set_transform(entity, invalid);
        CHECK(world.commit(edits).error == WorldError::invalid_transform);
        auto add = world.commands(); const auto pending = add.create(); add.add(pending, invalid);
        CHECK(world.commit(add).error == WorldError::invalid_transform);
        CHECK(world.size() == 1);
        matrix_close(matrix(world, entity), math::Mat4::identity());
    }
    auto normalize = world.commands(); normalize.set_transform(entity, {{}, {0,0,0,2}});
    REQUIRE(world.commit(normalize));
    world.with<TransformComponent>(entity, [](const auto& local) { CHECK(local.rotation.w == 1); });
}

TEST_CASE("Affine decomposition covers half turns inverse and shear tolerance", "[world][spatial]") {
    for (const auto axis : {math::Vec3{1,0,0}, math::Vec3{0,1,0}, math::Vec3{0,0,1}})
        for (float angle : {0.0f, .3f, math::PI, 4.7f}) {
            const auto original = local_matrix({{2,3,4}, math::Quat::from_axis_angle(axis,angle), {2,3,4}});
            const auto trs = decompose_transform(original); REQUIRE(trs);
            matrix_close(local_matrix(*trs), original);
            const auto inverse = inverse_affine(original); REQUIRE(inverse);
            matrix_close(*inverse * original, math::Mat4::identity());
        }
    auto shear = math::Mat4::identity(); shear.at(0,1) = spatial_tolerance*.1f;
    CHECK(decompose_transform(shear));
    shear.at(0,1) = spatial_tolerance*10;
    CHECK_FALSE(decompose_transform(shear)); CHECK(inverse_affine(shear));
    auto degenerate = math::Mat4::identity(); degenerate.at(0,1) = 1; degenerate.at(1,1) = 1e-10f;
    CHECK_FALSE(inverse_affine(degenerate));
    CHECK_FALSE(decompose_transform(math::Mat4::scale({-1,1,1})));
}

TEST_CASE("Camera data computes independent rigid view and Metal depth", "[world][spatial]") {
    const CameraComponent camera{math::PI/2, .5f, 100};
    const auto pose = local_matrix({{4,5,6}, math::Quat::from_axis_angle({0,1,0}, .4f)});
    const auto result = camera_matrices(camera, pose, 2);
    REQUIRE(result);
    matrix_close(result->view * pose, math::Mat4::identity());
    CHECK(result->projection.at(0,0) == Approx(.5f));
    CHECK(result->projection.at(1,1) == Approx(1));
    auto near = result->projection * math::Vec4{0,0,-camera.near_clip,1};
    auto far = result->projection * math::Vec4{0,0,-camera.far_clip,1};
    CHECK(near.z/near.w == Approx(0).margin(1e-6)); CHECK(far.z/far.w == Approx(1));
    matrix_close(result->view_projection, result->projection * result->view);
    auto ndc = result->projection * math::Vec4{2,1,-1,1};
    CHECK(ndc.x/ndc.w == Approx(1)); CHECK(ndc.y/ndc.w == Approx(1));
    CHECK_FALSE(camera_matrices(camera, math::Mat4::scale({1,2,1}), 1));
    for (float aspect : {0.0f, -1.0f, std::numeric_limits<float>::infinity()})
        CHECK_FALSE(camera_matrices(camera, pose, aspect));
    for (float fov : {0.0f, math::PI, std::numeric_limits<float>::quiet_NaN()})
        CHECK_FALSE(camera_matrices({fov,.1f,100}, pose, 1));
    CHECK_FALSE(camera_matrices({1,0,10}, pose, 1));
    CHECK_FALSE(camera_matrices({1,1,1}, pose, 1));
    CHECK_FALSE(camera_matrices({1,2,1}, pose, 1));
}

TEST_CASE("World camera rejects scaled ancestry even when scales cancel", "[world][spatial]") {
    World world;
    const auto root = spatial_entity(world, {{3,0,0}});
    const auto child = spatial_entity(world, {{0,0,2}});
    attach(world, child, root);
    auto add = world.commands(); add.add(child, CameraComponent{}); REQUIRE(world.commit(add));
    auto camera = world.camera(child, 1); REQUIRE(camera);
    CHECK(camera->view.at(0,3) == Approx(-3)); CHECK(camera->view.at(2,3) == Approx(-2));
    auto edits = world.commands();
    edits.set_transform(root, {{3,0,0}, {}, {2,2,2}});
    edits.set_transform(child, {{0,0,2}, {}, {.5f,.5f,.5f}});
    REQUIRE(world.commit(edits));
    CHECK(world.world_matrix(child)); CHECK_FALSE(world.camera(child,1));
    edits = world.commands(); edits.set_transform(root, {{3,0,0}}); edits.set_transform(child, {{0,0,2}});
    REQUIRE(world.commit(edits)); CHECK(world.camera(child,1));
    world.with<CameraComponent>(child, [](auto& value) { value.near_clip = -1; });
    CHECK_FALSE(world.camera(child,1));
}

TEST_CASE("Dirty branches moved between parents remain correct", "[world][spatial]") {
    World world;
    const auto a = spatial_entity(world), b = spatial_entity(world, {{10,0,0}});
    const auto c = spatial_entity(world, {{1,0,0}}), d = spatial_entity(world, {{2,0,0}});
    attach(world,c,a); attach(world,d,c); matrix(world,d);
    auto edits = world.commands(); edits.set_transform(a, {{20,0,0}});
    edits.reparent(c,b,ReparentPolicy::keep_local); REQUIRE(world.commit(edits));
    CHECK(matrix(world,d).at(0,3) == Approx(13));
    edits = world.commands(); edits.set_transform(b, {{30,0,0}}); REQUIRE(world.commit(edits));
    attach(world,c,a); CHECK(matrix(world,d).at(0,3) == Approx(23));
}

TEST_CASE("Spatial operations reject missing components and defer during scoped queries", "[world][spatial]") {
    World world;
    const auto entity = spatial_entity(world);
    auto create = world.commands(); const auto pending = create.create();
    const auto created = world.commit(create); REQUIRE(created);
    const auto empty = created.created[pending.index];
    CHECK_FALSE(world.world_matrix(empty)); CHECK_FALSE(world.camera(entity,1));
    auto missing = world.commands(); missing.set_transform(empty, {});
    CHECK(world.commit(missing).error == WorldError::component_missing);
    missing = world.commands(); missing.reparent(entity,empty,ReparentPolicy::keep_local);
    CHECK(world.commit(missing).error == WorldError::component_missing);
    auto edits = world.commands(); edits.set_transform(entity, {{5,0,0}});
    world.with<TransformComponent>(entity, [&](const auto&) {
        CHECK(world.commit(edits).error == WorldError::busy);
        CHECK(matrix(world,entity).at(0,3) == 0);
    });
    REQUIRE(world.commit(edits)); CHECK(matrix(world,entity).at(0,3) == 5);
    auto invalid = world.commands();
    invalid.reparent(entity, std::nullopt, static_cast<ReparentPolicy>(99));
    CHECK(world.commit(invalid).error == WorldError::invalid_policy);
}

TEST_CASE("Unrepresentable derived poses fail explicitly and recover after an edit", "[world][spatial]") {
    World world;
    const auto root = spatial_entity(world, {{}, {}, {1e30f,1e30f,1e30f}});
    const auto child = spatial_entity(world, {{}, {}, {1e30f,1e30f,1e30f}});
    attach(world,child,root);
    CHECK_FALSE(world.world_matrix(child));
    CHECK_FALSE(world.world_matrix(child)); // cached failure
    auto keep = world.commands(); keep.reparent(child,std::nullopt,ReparentPolicy::keep_world);
    CHECK(world.commit(keep).error == WorldError::unrepresentable_transform);
    auto fix = world.commands(); fix.set_transform(root, {}); REQUIRE(world.commit(fix));
    CHECK(world.world_matrix(child));
    auto projective = math::Mat4::identity(); projective.at(3,0) = 1;
    CHECK_FALSE(compose_affine(projective,math::Mat4::identity()));
}

TEST_CASE("Repeated forest edits agree with an independent translation model", "[world][spatial]") {
    World world;
    constexpr int count = 64;
    std::vector<EntityHandle> entities;
    std::vector<int> parents(count,-1);
    std::vector<float> local(count,0);
    for (int i = 0; i < count; ++i) entities.push_back(spatial_entity(world));
    const auto position = [&](int index) {
        float result = 0;
        for (; index >= 0; index = parents[index]) result += local[index];
        return result;
    };
    auto random = std::mt19937{992};
    for (int step = 0; step < 512; ++step) {
        const auto child = static_cast<int>(random()%count);
        const auto parent = static_cast<int>(random()%static_cast<unsigned>(child+1))-1;
        const auto preserve = random()%2 == 0;
        const auto old_position = position(child);
        auto edit = world.commands();
        const auto target = parent < 0 ? std::optional<EntityTarget>{} : std::optional<EntityTarget>{entities[parent]};
        edit.reparent(entities[child],target,preserve ? ReparentPolicy::keep_world : ReparentPolicy::keep_local);
        REQUIRE(world.commit(edit));
        if (preserve) local[child] = old_position - (parent < 0 ? 0 : position(parent));
        parents[child] = parent;
        if (step%3 == 0) {
            local[child] = static_cast<float>(random()%20)-10;
            edit = world.commands(); edit.set_transform(entities[child], {{local[child],0,0}});
            REQUIRE(world.commit(edit));
        }
        // Vary query order: clean descendants must not hide an ancestor's pending edit.
        for (int j = 0; j < count; ++j) {
            const auto i = (step+j)%count;
            CHECK(matrix(world,entities[i]).at(0,3) == Approx(position(i)).margin(1e-5));
        }
    }
}

TEST_CASE("Deep hierarchies update and delete without recursive stack growth", "[world][spatial]") {
    World world;
    auto commands = world.commands();
    constexpr int count = 4096;
    auto previous = commands.create(); commands.add(previous, TransformComponent{{1,0,0}});
    for (int i = 1; i < count; ++i) {
        const auto current = commands.create(); commands.add(current, TransformComponent{{1,0,0}});
        commands.reparent(previous, current, ReparentPolicy::keep_local); // constant-depth cycle check
        previous = current;
    }
    const auto result = world.commit(commands); REQUIRE(result);
    const auto leaf = result.created.front(), root = result.created.back();
    CHECK(matrix(world,leaf).at(0,3) == Approx(count));
    auto edit = world.commands(); edit.set_transform(root, {{2,0,0}}); REQUIRE(world.commit(edit));
    CHECK(matrix(world,leaf).at(0,3) == Approx(count+1));
    auto destroy = world.commands(); destroy.destroy(root); REQUIRE(world.commit(destroy));
    CHECK(world.size() == 0); CHECK_FALSE(world.alive(leaf));
}
} // namespace
