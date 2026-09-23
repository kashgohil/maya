#include "maya/assets/property_context.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <set>

using namespace maya;
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
    REQUIRE(component_schemas().size() == 5);
    auto ids = std::set<ComponentId>{};
    auto names = std::set<std::string_view>{};
    for (const auto& schema : component_schemas()) {
        REQUIRE(ids.insert(schema.id).second);
        REQUIRE(names.insert(schema.name).second);
        REQUIRE(schema.version == 1);
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
    REQUIRE(edit(value, 1, LightKind::spot));
    REQUIRE(edit(value, 1, static_cast<LightKind>(99)).error == PropertyError::invalid_value);
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
