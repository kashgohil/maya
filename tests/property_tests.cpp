#include "maya/assets/property_context.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <set>

using namespace maya;
using Catch::Approx;
namespace {
PropertyResult edit(ComponentValue& value, PropertyId id, PropertyValue input,
                    const PropertyValidationContext& context = {}) {
    const auto edits = std::array{PropertyEdit{id, std::move(input)}};
    return edit_properties(value, edits, context);
}
bool equal_value(const PropertyValue& left, const PropertyValue& right) {
    if (left.index() != right.index()) return false;
    return std::visit([&]<class T>(const T& value) {
        const auto& other = std::get<T>(right);
        if constexpr (std::same_as<T, math::Vec3>)
            return value.x == other.x && value.y == other.y && value.z == other.z;
        else if constexpr (std::same_as<T, math::Vec2>)
            return value.x == other.x && value.y == other.y;
        else if constexpr (std::same_as<T, math::Quat>)
            return value.x == other.x && value.y == other.y && value.z == other.z && value.w == other.w;
        else return value == other;
    }, left);
}
float scalar(const ComponentValue& value, PropertyId id) {
    return std::get<float>(*read_property(value, id));
}
EntityHandle create(World& world) {
    auto commands = world.commands();
    const auto entity = commands.create();
    commands.add(entity, TransformComponent{});
    commands.add(entity, CameraComponent{});
    commands.add(entity, LightComponent{});
    commands.add(entity, MeshRendererComponent{});
    commands.add(entity, NameComponent{"Camera"});
    const auto result = world.commit(commands);
    REQUIRE(result);
    return result.created[0];
}
class NoLoadProvider final : public AssetProvider {
public:
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path&) override {
        FAIL("Property validation must not load meshes"); return {};
    }
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path&) override {
        FAIL("Property validation must not load materials"); return {};
    }
};
}

TEST_CASE("Property schemas have stable identities, discoverable defaults, and typed access", "[properties]") {
    REQUIRE(component_schemas().size() == 14);
    auto ids = std::set<ComponentId>{};
    auto names = std::set<std::string_view>{};
    for (const auto& schema : component_schemas()) {
        REQUIRE(ids.insert(schema.id).second);
        REQUIRE(names.insert(schema.name).second);
        // Every schema is version 1 except the camera's (2: exposure and tone mapping, #1032) and the
        // light's (2: candela and shadows, #1034).
        REQUIRE(schema.version == (schema.id == ComponentId::camera || schema.id == ComponentId::light ? 2u : 1u));
        for (const auto& property : schema.properties) REQUIRE((property.since >= 1 && property.since <= schema.version));
        REQUIRE(component_schema(schema.name) == &schema);
        auto value = default_component(schema.id);
        REQUIRE(value);
        REQUIRE(component_id(*value) == schema.id);
        REQUIRE(validate_component(*value));
        auto property_ids = std::set<PropertyId>{};
        auto property_names = std::set<std::string_view>{};
        for (const auto& property : schema.properties) {
            REQUIRE(property.id != 0);
            REQUIRE(property_ids.insert(property.id).second);
            REQUIRE(property_names.insert(property.name).second);
            REQUIRE(property_schema(schema.id, property.id) == &property);
            REQUIRE(property_schema(schema.id, property.name) == &property);
            REQUIRE(equal_value(*read_property(*value, property.id), property.default_value));
            REQUIRE(edit(*value, property.id, property.default_value));
        }
    }
    REQUIRE(static_cast<uint32_t>(ComponentId::transform) == 2);
    REQUIRE(property_schema(ComponentId::transform, "rotation")->id == 2);
    REQUIRE(property_schema(ComponentId::camera, "far_clip")->id == 3);
    REQUIRE(property_schema(ComponentId::light, "enabled")->id == 7);
    REQUIRE(property_schema(ComponentId::mesh_renderer, "mesh")->encoding == PropertyEncoding::persistent_asset_id);
    REQUIRE(property_schema(ComponentId::light, "kind")->choices.size() == 3);
    // The built-in behaviors (#1003) keep their IDs, keys, and units.
    REQUIRE(static_cast<uint32_t>(ComponentId::spin) == 6);
    REQUIRE(static_cast<uint32_t>(ComponentId::fly_control) == 7);
    REQUIRE(component_schema("maya.spin")->id == ComponentId::spin);
    REQUIRE(component_schema("maya.fly_control")->id == ComponentId::fly_control);
    REQUIRE(property_schema(ComponentId::spin, "speed")->units == "rad/s");
    REQUIRE(property_schema(ComponentId::fly_control, "speed")->id == 1);
    REQUIRE(property_schema(ComponentId::fly_control, "look_sensitivity")->id == 2);
    auto fly = ComponentValue{FlyControlComponent{}};
    REQUIRE(edit(fly, 1, 0.0f).error == PropertyError::invalid_value); // speeds are positive
    REQUIRE_FALSE(component_schema(static_cast<ComponentId>(99)));
    REQUIRE_FALSE(component_schema("unknown"));
    REQUIRE_FALSE(default_component(static_cast<ComponentId>(99)));
    REQUIRE_FALSE(property_schema(ComponentId::camera, 99));
    REQUIRE_FALSE(property_schema(ComponentId::camera, "unknown"));
    REQUIRE_FALSE(read_property(ComponentValue{CameraComponent{}}, 0));
    REQUIRE(std::get<math::Vec3>(property_schema(ComponentId::transform, "scale")->default_value).x == 1);
    REQUIRE(std::get<math::Quat>(property_schema(ComponentId::transform, "rotation")->default_value).w == 1);
    REQUIRE(std::get<float>(property_schema(ComponentId::camera, "far_clip")->default_value) == 1000);
}

TEST_CASE("Edits reject unknown keys, duplicate keys, and wrong value types atomically", "[properties]") {
    auto value = ComponentValue{CameraComponent{}};
    REQUIRE(edit(value, 99, 5.0f).error == PropertyError::unknown_property);
    REQUIRE(edit(value, 1, true).error == PropertyError::type_mismatch);
    const auto duplicate = std::array{PropertyEdit{2, 2.0f}, PropertyEdit{2, 3.0f}};
    REQUIRE(edit_properties(value, duplicate).error == PropertyError::duplicate_property);
    REQUIRE(scalar(value, 2) == 0.1f);
    const auto invalid = std::array{PropertyEdit{2, 2.0f}, PropertyEdit{3, -1.0f}};
    REQUIRE(edit_properties(value, invalid).error == PropertyError::invalid_value);
    REQUIRE(scalar(value, 2) == 0.1f);
    REQUIRE(scalar(value, 3) == 1000);
    auto name = ComponentValue{NameComponent{}};
    REQUIRE(edit(name, 1, std::string{"人物 / Camera"}));
    REQUIRE(std::get<NameComponent>(name).value == "人物 / Camera");
    REQUIRE(edit(name, 1, std::string{}));
}

TEST_CASE("Camera edits validate complete final ranges without transient invalid states", "[properties]") {
    auto value = ComponentValue{CameraComponent{}};
    REQUIRE(edit(value, 2, 2000.0f).error == PropertyError::invalid_value);
    const auto both = std::array{PropertyEdit{2, 2000.0f}, PropertyEdit{3, 3000.0f}};
    REQUIRE(edit_properties(value, both));
    REQUIRE(scalar(value, 2) == 2000);
    REQUIRE(scalar(value, 3) == 3000);
    REQUIRE(edit(value, 3, 2000.0f).error == PropertyError::invalid_value);
    for (const auto bad : {0.0f, -1.0f, math::PI, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        REQUIRE(edit(value, 1, bad).error == PropertyError::invalid_value);
        REQUIRE(scalar(value, 1) == math::PI / 3);
    }
    REQUIRE(edit(value, 1, math::PI / 2));
    REQUIRE(camera_matrices(std::get<CameraComponent>(value), math::Mat4::identity(), 1));
}

TEST_CASE("All numeric properties reject nonfinite values and expose useful ranges", "[properties]") {
    for (const auto& schema : component_schemas()) {
        for (const auto& property : schema.properties) {
            for (const auto bad : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                auto value = *default_component(schema.id);
                if (property.type == PropertyType::scalar)
                    REQUIRE(edit(value, property.id, bad).error == PropertyError::invalid_value);
                else if (property.type == PropertyType::vector3) {
                    for (const auto vector : {math::Vec3{bad, 1, 1}, math::Vec3{1, bad, 1}, math::Vec3{1, 1, bad}})
                        REQUIRE(edit(value, property.id, vector).error == PropertyError::invalid_value);
                } else if (property.type == PropertyType::quaternion) {
                    for (const auto q : {math::Quat{bad, 0, 0, 1}, math::Quat{0, bad, 0, 1}, math::Quat{0, 0, bad, 1}, math::Quat{0, 0, 0, bad}})
                        REQUIRE(edit(value, property.id, q).error == PropertyError::invalid_value);
                }
            }
        }
    }
    REQUIRE_FALSE(property_schema(ComponentId::transform, "scale")->range.minimum_inclusive);
    REQUIRE(property_schema(ComponentId::light, "intensity")->range.minimum_inclusive);
}

TEST_CASE("Transform edits normalize rotation and preserve positive-scale rules", "[properties]") {
    auto value = ComponentValue{TransformComponent{}};
    REQUIRE(edit(value, 2, math::Quat{0, 0, 0, 7}));
    REQUIRE(std::get<TransformComponent>(value).rotation.w == 1);
    REQUIRE(edit(value, 2, math::Quat{0, 0, 0, 0}).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 3, math::Vec3{1, -1, 1}).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 3, math::Vec3{1, 0, 1}).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 1, math::Vec3{4, 5, 6}));
    REQUIRE(std::get<TransformComponent>(value).translation.z == 6);
    REQUIRE(edit(value, 2, math::Quat{0, 0, 0, std::numeric_limits<float>::max()}));
    REQUIRE(std::get<TransformComponent>(value).rotation.w == 1);
}

TEST_CASE("Light edits enforce HDR color, physical ranges, enum identity and cone relationships", "[properties]") {
    auto value = ComponentValue{LightComponent{}};
    REQUIRE(edit(value, 1, ChoiceValue{uint32_t(LightKind::spot)}));
    REQUIRE(std::get<LightComponent>(value).kind == LightKind::spot);
    REQUIRE(edit(value, 1, ChoiceValue{99}).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 2, math::Vec3{2, 0, 10}));
    REQUIRE(edit(value, 2, math::Vec3{-1, 0, 0}).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 3, 0.0f));
    REQUIRE(edit(value, 3, -1.0f).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 4, 0.0f).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 5, 0.0f));
    REQUIRE(edit(value, 6, 0.0f).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 6, math::PI).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 5, 2.0f).error == PropertyError::invalid_value);
    const auto cones = std::array{PropertyEdit{5, 2.0f}, PropertyEdit{6, 2.0f}};
    REQUIRE(edit_properties(value, cones));
    REQUIRE(edit(value, 7, false));
    REQUIRE_FALSE(std::get<LightComponent>(value).enabled);
}

TEST_CASE("Reference validation checks project identity and kind without requiring residency", "[properties]") {
    auto registry = AssetRegistry{std::filesystem::current_path(), std::make_unique<NoLoadProvider>()};
    const auto mesh = AssetRef<MeshAsset>{{994, 1}};
    const auto material = AssetRef<MaterialAsset>{{994, 2}};
    REQUIRE_FALSE(registry.register_asset(mesh, "missing-994.obj"));
    REQUIRE_FALSE(registry.register_asset(material, "missing-994.mat"));
    const auto context = asset_property_context(registry);
    auto value = ComponentValue{MeshRendererComponent{}};
    REQUIRE(edit(value, 1, mesh).error == PropertyError::validation_context_required);
    REQUIRE_FALSE(std::get<MeshRendererComponent>(value).mesh.valid());
    REQUIRE(edit(value, 1, mesh, context));
    REQUIRE(edit(value, 2, material, context));
    REQUIRE(registry.info(mesh.id)->state == AssetState::unloaded);
    REQUIRE(edit(value, 1, AssetRef<MeshAsset>{material.id}, context).error == PropertyError::wrong_reference_type);
    REQUIRE(edit(value, 1, material, context).error == PropertyError::type_mismatch);
    REQUIRE(edit(value, 1, AssetRef<MeshAsset>{{994, 99}}, context).error == PropertyError::missing_reference);
    REQUIRE(std::get<MeshRendererComponent>(value).mesh == mesh);
    const auto clears = std::array{PropertyEdit{1, AssetRef<MeshAsset>{}}, PropertyEdit{2, AssetRef<MaterialAsset>{}}};
    REQUIRE(edit_properties(value, clears));
    REQUIRE_FALSE(std::get<MeshRendererComponent>(value).mesh.valid());
    REQUIRE(edit(value, 3, false));
    REQUIRE_FALSE(std::get<MeshRendererComponent>(value).visible);
    const auto throwing = PropertyValidationContext{[](AssetId, ReferenceKind) -> ReferenceStatus { throw std::runtime_error("resolver failure"); }};
    REQUIRE_THROWS_AS(edit(value, 1, mesh, throwing), std::runtime_error);
    REQUIRE_FALSE(std::get<MeshRendererComponent>(value).mesh.valid());
}

TEST_CASE("World property edits retain identity and hierarchy and invalidate descendant matrices", "[properties]") {
    auto world = World{};
    const auto parent = create(world);
    const auto child = create(world);
    const auto id = world.persistent_id(parent);
    auto commands = world.commands();
    commands.reparent(child, parent, ReparentPolicy::keep_local);
    REQUIRE(world.commit(commands));
    REQUIRE(world.world_matrix(child)->at(0, 3) == 0);
    const auto move = std::array{PropertyEdit{1, math::Vec3{5, 0, 0}}};
    REQUIRE(edit_properties(world, parent, ComponentId::transform, move));
    REQUIRE(world.world_matrix(child)->at(0, 3) == 5);
    REQUIRE(world.parent(child) == parent);
    REQUIRE(world.persistent_id(parent) == id);
    const auto clips = std::array{PropertyEdit{2, 2.0f}, PropertyEdit{3, 500.0f}};
    REQUIRE(edit_properties(world, parent, ComponentId::camera, clips));
    REQUIRE(scalar(*read_component(world, parent, ComponentId::camera), 2) == 2);
    REQUIRE(world.camera(parent, 1));
    const auto bad = std::array{PropertyEdit{3, 1.0f}};
    REQUIRE_FALSE(edit_properties(world, parent, ComponentId::camera, bad));
    REQUIRE(scalar(*read_component(world, parent, ComponentId::camera), 3) == 500);
    REQUIRE(world.component_count<CameraComponent>() == 2);
    const auto rename = std::array{PropertyEdit{1, std::string{"New name"}}};
    REQUIRE(edit_properties(world, parent, ComponentId::name, rename));
    REQUIRE(std::get<NameComponent>(*read_component(world, parent, ComponentId::name)).value == "New name");
}

TEST_CASE("World property publication respects scoped borrows and stale or missing components", "[properties]") {
    auto world = World{};
    const auto entity = create(world);
    const auto edits = std::array{PropertyEdit{1, 1.0f}};
    REQUIRE(world.with<CameraComponent>(entity, [&](const auto&) {
        const auto result = edit_properties(world, entity, ComponentId::camera, edits);
        REQUIRE(result.error == PropertyError::world_rejected);
        REQUIRE(result.world_error == WorldError::busy);
    }));
    REQUIRE(scalar(*read_component(world, entity, ComponentId::camera), 1) == math::PI / 3);
    auto foreign = World{};
    REQUIRE(edit_properties(foreign, entity, ComponentId::camera, edits).error == PropertyError::invalid_entity);
    REQUIRE(edit_properties(world, entity, static_cast<ComponentId>(99), edits).error == PropertyError::unknown_component);
    auto remove = world.commands();
    remove.remove<CameraComponent>(entity);
    REQUIRE(world.commit(remove));
    REQUIRE(edit_properties(world, entity, ComponentId::camera, edits).error == PropertyError::component_missing);
    REQUIRE_FALSE(read_component(world, entity, ComponentId::camera));
    auto destroy = world.commands();
    destroy.destroy(entity);
    REQUIRE(world.commit(destroy));
    REQUIRE(edit_properties(world, entity, ComponentId::camera, edits).error == PropertyError::invalid_entity);
}

TEST_CASE("Component replacement participates in atomic ordered World command batches", "[properties]") {
    auto world = World{};
    const auto entity = create(world);
    auto bad = world.commands();
    bad.replace(entity, CameraComponent{1, 2, 20});
    bad.remove<NameComponent>(entity);
    bad.replace(entity, NameComponent{"missing"});
    REQUIRE(world.commit(bad).error == WorldError::component_missing);
    REQUIRE(scalar(*read_component(world, entity, ComponentId::camera), 2) == 0.1f);
    REQUIRE(world.has<NameComponent>(entity));
    auto commands = world.commands();
    const auto pending = commands.create();
    commands.add(pending, NameComponent{"first"});
    commands.replace(pending, NameComponent{"second"});
    commands.replace(pending, NameComponent{"third"});
    commands.remove<NameComponent>(pending);
    commands.add(pending, NameComponent{"fourth"});
    commands.replace(entity, TransformComponent{{3, 0, 0}, {}, {1, 1, 1}});
    const auto result = world.commit(commands);
    REQUIRE(result);
    REQUIRE(std::get<NameComponent>(*read_component(world, result.created[0], ComponentId::name)).value == "fourth");
    REQUIRE(world.world_matrix(entity)->at(0, 3) == 3);
    auto missing = world.commands();
    missing.replace(result.created[0], CameraComponent{});
    REQUIRE(world.commit(missing).error == WorldError::component_missing);
}

TEST_CASE("Complete-component validation is atomic even when normalization or catalog resolution fails", "[properties]") {
    auto value = ComponentValue{TransformComponent{{}, {0, 0, 0, 5}, {1, 1, 1}}};
    const auto tiny = std::array{PropertyEdit{3, math::Vec3{std::numeric_limits<float>::denorm_min()}}};
    REQUIRE_FALSE(edit_properties(value, tiny));
    REQUIRE(std::get<TransformComponent>(value).rotation.w == 5);
    REQUIRE(std::get<TransformComponent>(value).scale.x == 1);
    REQUIRE(validate_component(value));
    REQUIRE(std::get<TransformComponent>(value).rotation.w == 1);
    auto camera = ComponentValue{CameraComponent{1, 5, 2}};
    REQUIRE_FALSE(validate_component(camera));
    REQUIRE(scalar(camera, 2) == 5);
    auto mesh = ComponentValue{MeshRendererComponent{AssetRef<MeshAsset>{{994, 8}}, {}, true}};
    const auto missing = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::missing; }};
    REQUIRE(edit(mesh, 3, false, missing).error == PropertyError::missing_reference);
    REQUIRE(std::get<MeshRendererComponent>(mesh).visible);
}

TEST_CASE("Replacement handles move-only ownership and releases staged resources on failure", "[properties]") {
    struct Owned { std::unique_ptr<int> value; };
    auto world = World{};
    auto commands = world.commands();
    const auto pending = commands.create();
    commands.add(pending, Owned{std::make_unique<int>(1)});
    const auto result = world.commit(commands);
    REQUIRE(result);
    const auto entity = result.created[0];
    auto replace = world.commands();
    replace.replace(entity, Owned{std::make_unique<int>(2)});
    REQUIRE(world.commit(replace));
    REQUIRE(world.with<Owned>(entity, [](const auto& value) { REQUIRE(*value.value == 2); }));
    auto bad = world.commands();
    bad.replace(entity, Owned{std::make_unique<int>(3)});
    bad.remove<CameraComponent>(entity);
    REQUIRE_FALSE(world.commit(bad));
    REQUIRE(world.with<Owned>(entity, [](const auto& value) { REQUIRE(*value.value == 2); }));
}

TEST_CASE("Physics components have stable schemas and validate their values", "[properties][physics]") {
    // Stable identities for the physics milestone (#1019).
    REQUIRE(static_cast<uint32_t>(ComponentId::collider) == 8);
    REQUIRE(static_cast<uint32_t>(ComponentId::rigid_body) == 9);
    REQUIRE(static_cast<uint32_t>(ComponentId::physics_settings) == 10);
    REQUIRE(component_schema("maya.collider")->id == ComponentId::collider);
    REQUIRE(component_schema("maya.rigid_body")->id == ComponentId::rigid_body);
    REQUIRE(component_schema("maya.physics_settings")->id == ComponentId::physics_settings);
    const auto* shape = property_schema(ComponentId::collider, "shape");
    REQUIRE(shape->type == PropertyType::choice);
    REQUIRE(shape->choices.size() == 3);
    REQUIRE(property_schema(ComponentId::collider, "group")->type == PropertyType::integer);
    REQUIRE(property_schema(ComponentId::collider, "group")->presentation == PropertyPresentation::collision_group);
    REQUIRE(property_schema(ComponentId::collider, "mask")->type == PropertyType::flags);
    REQUIRE(property_schema(ComponentId::collider, "mask")->presentation == PropertyPresentation::collision_mask);
    REQUIRE(property_schema(ComponentId::rigid_body, "motion")->choices.size() == 2);

    auto collider = ComponentValue{ColliderComponent{}};
    REQUIRE(edit(collider, 1, ChoiceValue{uint32_t(ColliderShape::capsule)}));
    REQUIRE(std::get<ColliderComponent>(collider).shape == ColliderShape::capsule);
    REQUIRE(edit(collider, 1, ChoiceValue{7}).error == PropertyError::invalid_value);
    REQUIRE(edit(collider, 1, 1.0f).error == PropertyError::type_mismatch);
    REQUIRE(edit(collider, 2, math::Vec3{1, 0, 1}).error == PropertyError::invalid_value); // half extents are positive
    REQUIRE(edit(collider, 3, 0.0f).error == PropertyError::invalid_value); // so is the radius
    REQUIRE(edit(collider, 4, -1.0f).error == PropertyError::invalid_value);
    REQUIRE(edit(collider, 6, math::Quat{0, 0, 0, 3}));
    REQUIRE(std::get<ColliderComponent>(collider).rotation.w == 1.0f); // normalized
    REQUIRE(edit(collider, 6, math::Quat{0, 0, 0, 0}).error == PropertyError::invalid_value);
    REQUIRE(edit(collider, 7, -0.1f).error == PropertyError::invalid_value);
    REQUIRE(edit(collider, 8, 1.0f));
    REQUIRE(edit(collider, 8, 1.5f).error == PropertyError::invalid_value); // restitution is 0 to 1
    REQUIRE(edit(collider, 10, int32_t{15}));
    REQUIRE(edit(collider, 10, int32_t{16}).error == PropertyError::invalid_value);
    REQUIRE(edit(collider, 10, int32_t{-1}).error == PropertyError::invalid_value);
    REQUIRE(edit(collider, 10, 3.0f).error == PropertyError::type_mismatch);
    REQUIRE(edit(collider, 11, uint32_t{0}));
    REQUIRE(edit(collider, 11, uint32_t{0xFFFF}));
    REQUIRE(edit(collider, 11, uint32_t{0x10000}).error == PropertyError::invalid_value);

    auto body = ComponentValue{RigidBodyComponent{}};
    REQUIRE(edit(body, 7, math::Vec3{1, 2, 3}));
    // A kinematic body has no initial velocity: switching with one set is refused as a whole.
    REQUIRE(edit(body, 1, ChoiceValue{uint32_t(BodyMotion::kinematic)}).error == PropertyError::invalid_value);
    const auto both = std::array{PropertyEdit{1, ChoiceValue{uint32_t(BodyMotion::kinematic)}}, PropertyEdit{7, math::Vec3{0, 0, 0}}};
    REQUIRE(edit_properties(body, both));
    REQUIRE(std::get<RigidBodyComponent>(body).motion == BodyMotion::kinematic);
    REQUIRE(edit(body, 2, -1.0f).error == PropertyError::invalid_value);
    REQUIRE(edit(body, 3, 0.0f).error == PropertyError::invalid_value); // density is positive
    REQUIRE(edit(body, 6, -2.0f)); // a negative gravity factor floats upward
    REQUIRE(edit(body, 6, std::numeric_limits<float>::infinity()).error == PropertyError::invalid_value);

    auto settings = ComponentValue{PhysicsSettingsComponent{}};
    REQUIRE(std::get<PhysicsSettingsComponent>(settings).gravity.y == Approx(-9.81f));
    REQUIRE(edit(settings, 1, math::Vec3{0, -1.62f, 0}));
    REQUIRE(edit(settings, 1, math::Vec3{0, std::nanf(""), 0}).error == PropertyError::invalid_value);
}

TEST_CASE("Script components hold a script and named values for its properties", "[properties][scripting]") {
    REQUIRE(static_cast<uint32_t>(ComponentId::script) == 11);
    REQUIRE(component_schema("maya.script")->id == ComponentId::script);
    REQUIRE(property_schema(ComponentId::script, "script")->type == PropertyType::script_ref);
    REQUIRE(property_schema(ComponentId::script, "script")->encoding == PropertyEncoding::persistent_asset_id);
    REQUIRE(property_schema(ComponentId::script, "values")->type == PropertyType::script_values);
    CHECK(std::string(script_value_type_name(ScriptValueType::color)) == "color");
    CHECK(script_value_type("entity") == ScriptValueType::entity);
    CHECK_FALSE(script_value_type("table"));

    auto value = ComponentValue{ScriptComponent{}};
    const auto good = std::vector<ScriptValue>{{"speed", ScriptValueType::number, 3.0f}, {"name", ScriptValueType::string, std::string("x")},
                                               {"_target2", ScriptValueType::entity, EntityId{}}, {"tint", ScriptValueType::color, math::Vec3(1.0f)}};
    REQUIRE(edit(value, 2, good));
    CHECK(std::get<ScriptComponent>(value).values == good);
    const auto refused = [&](std::vector<ScriptValue> values) { return edit(value, 2, std::move(values)).error; };
    CHECK(refused({{"two words", ScriptValueType::number, 1.0f}}) == PropertyError::invalid_value);
    CHECK(refused({{"9lives", ScriptValueType::number, 1.0f}}) == PropertyError::invalid_value);
    CHECK(refused({{"a", ScriptValueType::number, 1.0f}, {"a", ScriptValueType::number, 2.0f}}) == PropertyError::invalid_value);
    CHECK(refused({{"a", ScriptValueType::number, std::string("x")}}) == PropertyError::invalid_value);
    CHECK(refused({{"a", ScriptValueType::number, std::numeric_limits<float>::infinity()}}) == PropertyError::invalid_value);
    CHECK(refused({{"a", ScriptValueType::vector, 1.0f}}) == PropertyError::invalid_value);
    CHECK(script_values_problem({{"a", ScriptValueType::number, 1.0f}, {"a", ScriptValueType::number, 2.0f}}) ==
          "Script property 'a' has more than one value");
    // The script reference is checked against the catalog like any asset.
    const auto kinds = PropertyValidationContext{[](AssetId id, ReferenceKind kind) {
        return id.low == 1 && kind == ReferenceKind::script ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
    }};
    CHECK(edit(value, 1, AssetRef<ScriptAsset>{{9, 1}}, kinds));
    CHECK(edit(value, 1, AssetRef<ScriptAsset>{{9, 2}}, kinds).error == PropertyError::wrong_reference_type);
    CHECK(edit(value, 1, AssetRef<MeshAsset>{{9, 1}}, kinds).error == PropertyError::type_mismatch);
}

TEST_CASE("Environment components name an environment asset, its intensity and rotation, and whether the sky shows", "[properties][environments]") {
    REQUIRE(static_cast<uint32_t>(ComponentId::environment) == 12); // #1035
    const auto* schema = component_schema("maya.environment");
    REQUIRE(schema);
    REQUIRE(schema->id == ComponentId::environment);
    REQUIRE(schema->version == 1);
    REQUIRE(property_schema(ComponentId::environment, "environment")->type == PropertyType::environment_ref);
    REQUIRE(property_schema(ComponentId::environment, "environment")->encoding == PropertyEncoding::persistent_asset_id);
    REQUIRE(property_schema(ComponentId::environment, "rotation")->units == "rad");
    auto value = ComponentValue{EnvironmentComponent{}};
    REQUIRE(edit(value, 2, 0.0f));
    REQUIRE(edit(value, 2, -1.0f).error == PropertyError::invalid_value);
    REQUIRE(edit(value, 3, math::PI));
    REQUIRE(edit(value, 3, 4.0f).error == PropertyError::invalid_value); // within half a turn either way
    REQUIRE(edit(value, 4, false));
    REQUIRE_FALSE(std::get<EnvironmentComponent>(value).background);
    // The reference is checked against the catalog as an environment.
    const auto environment = AssetId{0x656e, 1}, texture = AssetId{0x656e, 2};
    const auto context = PropertyValidationContext{[&](AssetId id, ReferenceKind kind) {
        if (id == environment) return kind == ReferenceKind::environment ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
        if (id == texture) return kind == ReferenceKind::texture ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
        return ReferenceStatus::missing;
    }};
    REQUIRE(edit(value, 1, AssetRef<EnvironmentAsset>{environment}, context));
    REQUIRE(edit(value, 1, AssetRef<EnvironmentAsset>{texture}, context).error == PropertyError::wrong_reference_type);
    REQUIRE(edit(value, 1, AssetRef<EnvironmentAsset>{{9, 9}}, context).error == PropertyError::missing_reference);
    REQUIRE(edit(value, 1, AssetRef<TextureAsset>{environment}, context).error == PropertyError::type_mismatch);
}

TEST_CASE("Skin and animation components name a skin and a clip, and how the clip plays", "[properties][animation]") {
    REQUIRE(static_cast<uint32_t>(ComponentId::skin) == 13); // #1038
    REQUIRE(static_cast<uint32_t>(ComponentId::animation) == 14);
    REQUIRE(component_schema("maya.skin")->id == ComponentId::skin);
    REQUIRE(component_schema("maya.animation")->id == ComponentId::animation);
    REQUIRE(property_schema(ComponentId::skin, "skin")->type == PropertyType::skin_ref);
    REQUIRE(property_schema(ComponentId::skin, "skin")->encoding == PropertyEncoding::persistent_asset_id);
    REQUIRE(property_schema(ComponentId::animation, "clip")->type == PropertyType::animation_ref);
    REQUIRE(property_schema(ComponentId::animation, "clip")->encoding == PropertyEncoding::persistent_asset_id);
    REQUIRE(property_schema(ComponentId::animation, "start")->units == "s");
    // Defaults: no clip, playing and looping at normal speed from the start.
    const auto defaults = AnimationComponent{};
    CHECK(defaults.playing);
    CHECK(defaults.loop);
    CHECK(defaults.speed == 1.0f);
    CHECK(defaults.start == 0.0f);
    auto value = ComponentValue{AnimationComponent{}};
    CHECK(edit(value, 4, -2.5f));
    CHECK(edit(value, 4, 101.0f).error == PropertyError::invalid_value);
    CHECK(edit(value, 4, NAN).error == PropertyError::invalid_value);
    CHECK(edit(value, 5, 3.0f));
    CHECK(edit(value, 5, -0.1f).error == PropertyError::invalid_value);
    CHECK(edit(value, 2, false));
    CHECK_FALSE(std::get<AnimationComponent>(value).playing);
    // References are checked against the catalog as a skin and a clip.
    const auto skin = AssetId{0x616e, 1}, clip = AssetId{0x616e, 2};
    const auto context = PropertyValidationContext{[&](AssetId id, ReferenceKind kind) {
        if (id == skin) return kind == ReferenceKind::skin ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
        if (id == clip) return kind == ReferenceKind::animation ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
        return ReferenceStatus::missing;
    }};
    CHECK(edit(value, 1, AssetRef<AnimationAsset>{clip}, context));
    CHECK(edit(value, 1, AssetRef<AnimationAsset>{skin}, context).error == PropertyError::wrong_reference_type);
    CHECK(edit(value, 1, AssetRef<SkinAsset>{clip}, context).error == PropertyError::type_mismatch);
    auto skinned = ComponentValue{SkinComponent{}};
    CHECK(edit(skinned, 1, AssetRef<SkinAsset>{skin}, context));
    CHECK(edit(skinned, 1, AssetRef<SkinAsset>{{9, 9}}, context).error == PropertyError::missing_reference);
}
