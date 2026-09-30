#include "maya/properties/schema.hpp"
#include <array>
#include <cmath>
#include <utility>

namespace maya {
namespace {
struct Binding {
    PropertyDescriptor descriptor;
    PropertyValue (*read)(const ComponentValue&);
    bool (*write)(ComponentValue&, const PropertyValue&);
};
template<class> struct MemberTraits;
template<class C, class V> struct MemberTraits<V C::*> { using Owner = C; using Value = V; };
template<class T> constexpr PropertyType property_type() {
    if constexpr (std::same_as<T, std::string>) return PropertyType::text;
    else if constexpr (std::same_as<T, bool>) return PropertyType::boolean;
    else if constexpr (std::same_as<T, float>) return PropertyType::scalar;
    else if constexpr (std::same_as<T, math::Vec3>) return PropertyType::vector3;
    else if constexpr (std::same_as<T, math::Quat>) return PropertyType::quaternion;
    else if constexpr (std::is_enum_v<T>) return PropertyType::choice;
    else if constexpr (std::same_as<T, AssetRef<MeshAsset>>) return PropertyType::mesh_ref;
    else if constexpr (std::same_as<T, int32_t>) return PropertyType::integer;
    else if constexpr (std::same_as<T, uint32_t>) return PropertyType::flags;
    else { static_assert(std::same_as<T, AssetRef<MaterialAsset>>); return PropertyType::material_ref; }
}
/// Enumerations are held in PropertyValue as ChoiceValue; everything else as itself.
template<class V> PropertyValue property_value(const V& value) {
    if constexpr (std::is_enum_v<V>) return ChoiceValue{static_cast<uint32_t>(value)};
    else return value;
}
template<auto Member>
Binding bind(PropertyId id, std::string_view name, std::string_view label,
             PropertyPresentation presentation, NumericRange range = {}, std::string_view units = {},
             std::string_view description = {}, std::span<const EnumOption> choices = {}) {
    using C = typename MemberTraits<decltype(Member)>::Owner;
    using V = typename MemberTraits<decltype(Member)>::Value;
    constexpr auto type = property_type<V>();
    constexpr auto encoding = type == PropertyType::mesh_ref || type == PropertyType::material_ref
        ? PropertyEncoding::persistent_asset_id : PropertyEncoding::value;
    return {{id, name, label, type, property_value(C{}.*Member), range, units, presentation, encoding, choices, description},
        [](const ComponentValue& value) -> PropertyValue { return property_value(std::get<C>(value).*Member); },
        [](ComponentValue& value, const PropertyValue& input) {
            if constexpr (std::is_enum_v<V>) {
                const auto choice = std::get_if<ChoiceValue>(&input);
                if (!choice) return false;
                std::get<C>(value).*Member = static_cast<V>(choice->value); // validation checks the choices
            } else {
                const auto typed = std::get_if<V>(&input);
                if (!typed) return false;
                std::get<C>(value).*Member = *typed;
            }
            return true;
        }};
}
using Hint = PropertyPresentation;
constexpr auto positive = NumericRange{0.0f, {}, false, true};
constexpr auto nonnegative = NumericRange{0.0f, {}, true, true};
constexpr auto angle = NumericRange{0.0f, math::PI, false, false};
constexpr auto cone = NumericRange{0.0f, math::PI, true, false};
constexpr auto unit_interval = NumericRange{0.0f, 1.0f, true, true};
constexpr auto collision_groups = NumericRange{0.0f, 15.0f, true, true};
constexpr auto collision_mask = NumericRange{0.0f, 65535.0f, true, true};
template<class E> constexpr EnumOption option(E value, std::string_view name, std::string_view label) {
    return {static_cast<uint32_t>(value), name, label};
}
constexpr auto light_options = std::array{
    option(LightKind::directional, "directional", "Directional"),
    option(LightKind::point, "point", "Point"),
    option(LightKind::spot, "spot", "Spot")};
constexpr auto shape_options = std::array{
    option(ColliderShape::box, "box", "Box"),
    option(ColliderShape::sphere, "sphere", "Sphere"),
    option(ColliderShape::capsule, "capsule", "Capsule")};
constexpr auto motion_options = std::array{
    option(BodyMotion::dynamic, "dynamic", "Dynamic"),
    option(BodyMotion::kinematic, "kinematic", "Kinematic")};
const auto& name_bindings() {
    static const auto values = std::array{
        bind<&NameComponent::value>(1, "value", "Name", Hint::text)};
    return values;
}
const auto& transform_bindings() {
    static const auto values = std::array{
        bind<&TransformComponent::translation>(1, "translation", "Translation", Hint::vector, {}, "m"),
        bind<&TransformComponent::rotation>(2, "rotation", "Rotation", Hint::rotation, {}, {}, "Normalized quaternion in x,y,z,w order."),
        bind<&TransformComponent::scale>(3, "scale", "Scale", Hint::vector, positive)};
    return values;
}
const auto& mesh_bindings() {
    static const auto values = std::array{
        bind<&MeshRendererComponent::mesh>(1, "mesh", "Mesh", Hint::asset),
        bind<&MeshRendererComponent::material>(2, "material", "Material", Hint::asset),
        bind<&MeshRendererComponent::visible>(3, "visible", "Visible", Hint::toggle)};
    return values;
}
const auto& camera_bindings() {
    static const auto values = std::array{
        bind<&CameraComponent::vertical_fov>(1, "vertical_fov", "Vertical field of view", Hint::number, angle, "rad"),
        bind<&CameraComponent::near_clip>(2, "near_clip", "Near clip", Hint::number, positive, "m"),
        bind<&CameraComponent::far_clip>(3, "far_clip", "Far clip", Hint::number, positive, "m", "Must exceed near clip.")};
    return values;
}
const auto& light_bindings() {
    static const auto values = std::array{
        bind<&LightComponent::kind>(1, "kind", "Kind", Hint::choice, {}, {}, {}, light_options),
        bind<&LightComponent::color>(2, "color", "Color", Hint::color, nonnegative, "linear RGB", "HDR values above one are allowed."),
        bind<&LightComponent::intensity>(3, "intensity", "Intensity", Hint::number, nonnegative, "lux / lm", "Lux for directional lights; lumens for point and spot lights."),
        bind<&LightComponent::range>(4, "range", "Range", Hint::number, positive, "m", "Used by point and spot lights."),
        bind<&LightComponent::inner_cone>(5, "inner_cone", "Inner cone", Hint::number, cone, "rad", "Full angle; must not exceed outer cone."),
        bind<&LightComponent::outer_cone>(6, "outer_cone", "Outer cone", Hint::number, angle, "rad", "Full angle; used by spot lights."),
        bind<&LightComponent::enabled>(7, "enabled", "Enabled", Hint::toggle)};
    return values;
}
const auto& spin_bindings() {
    static const auto values = std::array{
        bind<&SpinComponent::axis>(1, "axis", "Axis", Hint::vector, {}, {}, "Local axis to turn about; its length is ignored."),
        bind<&SpinComponent::speed>(2, "speed", "Speed", Hint::number, {}, "rad/s", "Negative turns the other way.")};
    return values;
}
const auto& fly_bindings() {
    static const auto values = std::array{
        bind<&FlyControlComponent::speed>(1, "speed", "Speed", Hint::number, positive, "m/s", "Shift moves four times faster."),
        bind<&FlyControlComponent::look_sensitivity>(2, "look_sensitivity", "Look sensitivity", Hint::number, positive, "rad/pt",
            "Turn per point of mouse movement.")};
    return values;
}

const auto& collider_bindings() {
    static const auto values = std::array{
        bind<&ColliderComponent::shape>(1, "shape", "Shape", Hint::choice, {}, {}, {}, shape_options),
        bind<&ColliderComponent::half_extents>(2, "half_extents", "Half extents", Hint::vector, positive, "m", "Box only."),
        bind<&ColliderComponent::radius>(3, "radius", "Radius", Hint::number, positive, "m", "Sphere and capsule."),
        bind<&ColliderComponent::half_height>(4, "half_height", "Half height", Hint::number, positive, "m",
            "Capsule only: half the straight section along local Y, excluding the caps."),
        bind<&ColliderComponent::offset>(5, "offset", "Offset", Hint::vector, {}, "m", "In the entity's local space."),
        bind<&ColliderComponent::rotation>(6, "rotation", "Rotation", Hint::rotation, {}, {}, "In the entity's local space."),
        bind<&ColliderComponent::friction>(7, "friction", "Friction", Hint::number, nonnegative, {},
            "A body with several colliders uses its first collider's friction."),
        bind<&ColliderComponent::restitution>(8, "restitution", "Restitution", Hint::number, unit_interval, {},
            "Bounciness, 0 to 1. A body with several colliders uses its first collider's."),
        bind<&ColliderComponent::sensor>(9, "sensor", "Sensor", Hint::toggle, {}, {},
            "Detects overlaps without a contact response."),
        bind<&ColliderComponent::group>(10, "group", "Collision group", Hint::collision_group, collision_groups, {},
            "Named in the project."),
        bind<&ColliderComponent::mask>(11, "mask", "Collides with", Hint::collision_mask, collision_mask, {},
            "Two colliders collide only when each one's group is in the other's mask.")};
    return values;
}
const auto& rigid_body_bindings() {
    static const auto values = std::array{
        bind<&RigidBodyComponent::motion>(1, "motion", "Motion", Hint::choice, {}, {},
            "Dynamic bodies are moved by physics; kinematic bodies follow targets set while playing.", motion_options),
        bind<&RigidBodyComponent::mass>(2, "mass", "Mass", Hint::number, nonnegative, "kg", "0 derives the mass from the density."),
        bind<&RigidBodyComponent::density>(3, "density", "Density", Hint::number, positive, "kg/m\xC2\xB3"),
        bind<&RigidBodyComponent::linear_damping>(4, "linear_damping", "Linear damping", Hint::number, nonnegative, "1/s"),
        bind<&RigidBodyComponent::angular_damping>(5, "angular_damping", "Angular damping", Hint::number, nonnegative, "1/s"),
        bind<&RigidBodyComponent::gravity_factor>(6, "gravity_factor", "Gravity factor", Hint::number),
        bind<&RigidBodyComponent::linear_velocity>(7, "linear_velocity", "Initial velocity", Hint::vector, {}, "m/s",
            "Dynamic bodies only."),
        bind<&RigidBodyComponent::angular_velocity>(8, "angular_velocity", "Initial spin", Hint::vector, {}, "rad/s",
            "Dynamic bodies only.")};
    return values;
}
const auto& physics_settings_bindings() {
    static const auto values = std::array{
        bind<&PhysicsSettingsComponent::gravity>(1, "gravity", "Gravity", Hint::vector, {}, "m/s\xC2\xB2")};
    return values;
}

template<size_t N> auto descriptors(const std::array<Binding, N>& bindings) {
    auto result = std::array<PropertyDescriptor, N>{};
    for (size_t i = 0; i < N; ++i) result[i] = bindings[i].descriptor;
    return result;
}
std::span<const Binding> bindings(ComponentId id) {
    switch (id) {
    case ComponentId::name: return name_bindings();
    case ComponentId::transform: return transform_bindings();
    case ComponentId::mesh_renderer: return mesh_bindings();
    case ComponentId::camera: return camera_bindings();
    case ComponentId::light: return light_bindings();
    case ComponentId::spin: return spin_bindings();
    case ComponentId::fly_control: return fly_bindings();
    case ComponentId::collider: return collider_bindings();
    case ComponentId::rigid_body: return rigid_body_bindings();
    case ComponentId::physics_settings: return physics_settings_bindings();
    }
    return {};
}
const Binding* binding(ComponentId id, PropertyId property) {
    for (const auto& item : bindings(id)) if (item.descriptor.id == property) return &item;
    return nullptr;
}
bool in_range(float value, const NumericRange& range) {
    if (!std::isfinite(value)) return false;
    if (range.minimum && (range.minimum_inclusive ? value < *range.minimum : value <= *range.minimum)) return false;
    if (range.maximum && (range.maximum_inclusive ? value > *range.maximum : value >= *range.maximum)) return false;
    return true;
}
PropertyResult validate(ComponentValue& value, const PropertyValidationContext& context) {
    const auto id = component_id(value);
    for (const auto& item : bindings(id)) {
        const auto& d = item.descriptor;
        const auto input = item.read(value);
        const auto invalid = [&] { return PropertyResult{PropertyError::invalid_value, d.id, "Value is nonfinite or outside its allowed range"}; };
        if (const auto scalar = std::get_if<float>(&input)) {
            if (!in_range(*scalar, d.range)) return invalid();
        } else if (const auto vector = std::get_if<math::Vec3>(&input)) {
            if (!in_range(vector->x, d.range) || !in_range(vector->y, d.range) || !in_range(vector->z, d.range)) return invalid();
        } else if (const auto choice = std::get_if<ChoiceValue>(&input)) {
            auto found = false;
            for (const auto& option : d.choices) found |= option.value == choice->value;
            if (!found) return {PropertyError::invalid_value, d.id, "Value is not one of the property's choices"};
        } else if (const auto whole = std::get_if<int32_t>(&input)) {
            if (!in_range(float(*whole), d.range)) return invalid();
        } else if (const auto bits = std::get_if<uint32_t>(&input)) {
            if (d.range.maximum && float(*bits) > *d.range.maximum) return invalid();
        } else if (d.encoding == PropertyEncoding::persistent_asset_id) {
            const auto mesh = std::get_if<AssetRef<MeshAsset>>(&input);
            const auto asset = mesh ? mesh->id : std::get<AssetRef<MaterialAsset>>(input).id;
            if (!asset.valid()) continue;
            if (!context.resolve_asset) return {PropertyError::validation_context_required, d.id, "A catalog resolver is required for nonempty asset references"};
            const auto status = context.resolve_asset(asset, mesh ? ReferenceKind::mesh : ReferenceKind::material);
            if (status == ReferenceStatus::missing) return {PropertyError::missing_reference, d.id, "Asset ID is not registered in this project"};
            if (status != ReferenceStatus::valid) return {PropertyError::wrong_reference_type, d.id, "Asset catalog kind does not match the property"};
        }
    }
    if (auto transform = std::get_if<TransformComponent>(&value)) {
        const auto normalized = validated_transform(*transform);
        if (!normalized) return {PropertyError::invalid_value, 0,
            "Transform needs a finite nonzero rotation and a numerically representable local matrix and inverse"};
        *transform = *normalized;
    } else if (const auto camera = std::get_if<CameraComponent>(&value)) {
        if (camera->far_clip <= camera->near_clip)
            return {PropertyError::invalid_value, 3, "Far clip must exceed near clip"};
    } else if (const auto light = std::get_if<LightComponent>(&value)) {
        if (light->inner_cone > light->outer_cone)
            return {PropertyError::invalid_value, 5, "Inner cone must not exceed outer cone"};
    } else if (const auto collider = std::get_if<ColliderComponent>(&value)) {
        const auto& q = collider->rotation;
        const auto length = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
        if (!std::isfinite(length) || !(length > 1e-12)) return {PropertyError::invalid_value, 6, "Rotation needs a finite nonzero quaternion"};
        if (std::abs(length - 1.0) > quaternion_unit_tolerance)
            collider->rotation = {float(q.x / length), float(q.y / length), float(q.z / length), float(q.w / length)};
    } else if (const auto body = std::get_if<RigidBodyComponent>(&value)) {
        if (body->motion == BodyMotion::kinematic &&
            (body->linear_velocity.length_squared() != 0.0f || body->angular_velocity.length_squared() != 0.0f))
            return {PropertyError::invalid_value, 7, "A kinematic body has no initial velocity; it follows targets set while playing"};
    }
    return {};
}
} // namespace

std::span<const ComponentDescriptor> component_schemas() {
    static const auto names = descriptors(name_bindings());
    static const auto transforms = descriptors(transform_bindings());
    static const auto meshes = descriptors(mesh_bindings());
    static const auto cameras = descriptors(camera_bindings());
    static const auto lights = descriptors(light_bindings());
    static const auto spins = descriptors(spin_bindings());
    static const auto flights = descriptors(fly_bindings());
    static const auto colliders = descriptors(collider_bindings());
    static const auto bodies = descriptors(rigid_body_bindings());
    static const auto physics = descriptors(physics_settings_bindings());
    static const auto schemas = std::array{
        ComponentDescriptor{ComponentId::name, "maya.name", "Name", 1, names},
        ComponentDescriptor{ComponentId::transform, "maya.transform", "Transform", 1, transforms},
        ComponentDescriptor{ComponentId::mesh_renderer, "maya.mesh_renderer", "Mesh renderer", 1, meshes},
        ComponentDescriptor{ComponentId::camera, "maya.camera", "Camera", 1, cameras},
        ComponentDescriptor{ComponentId::light, "maya.light", "Light", 1, lights},
        ComponentDescriptor{ComponentId::spin, "maya.spin", "Spin", 1, spins},
        ComponentDescriptor{ComponentId::fly_control, "maya.fly_control", "Fly control", 1, flights},
        ComponentDescriptor{ComponentId::collider, "maya.collider", "Collider", 1, colliders},
        ComponentDescriptor{ComponentId::rigid_body, "maya.rigid_body", "Rigid body", 1, bodies},
        ComponentDescriptor{ComponentId::physics_settings, "maya.physics_settings", "Physics settings", 1, physics}};
    return schemas;
}
const ComponentDescriptor* component_schema(ComponentId id) {
    for (const auto& schema : component_schemas()) if (schema.id == id) return &schema;
    return nullptr;
}
const ComponentDescriptor* component_schema(std::string_view name) {
    for (const auto& schema : component_schemas()) if (schema.name == name) return &schema;
    return nullptr;
}
const PropertyDescriptor* property_schema(ComponentId component, PropertyId property) {
    const auto schema = component_schema(component);
    if (schema) for (const auto& item : schema->properties) if (item.id == property) return &item;
    return nullptr;
}
const PropertyDescriptor* property_schema(ComponentId component, std::string_view name) {
    const auto schema = component_schema(component);
    if (schema) for (const auto& item : schema->properties) if (item.name == name) return &item;
    return nullptr;
}
ComponentId component_id(const ComponentValue& value) {
    return std::visit([]<class T>(const T&) {
        if constexpr (std::same_as<T, NameComponent>) return ComponentId::name;
        else if constexpr (std::same_as<T, TransformComponent>) return ComponentId::transform;
        else if constexpr (std::same_as<T, MeshRendererComponent>) return ComponentId::mesh_renderer;
        else if constexpr (std::same_as<T, CameraComponent>) return ComponentId::camera;
        else if constexpr (std::same_as<T, LightComponent>) return ComponentId::light;
        else if constexpr (std::same_as<T, SpinComponent>) return ComponentId::spin;
        else if constexpr (std::same_as<T, FlyControlComponent>) return ComponentId::fly_control;
        else if constexpr (std::same_as<T, ColliderComponent>) return ComponentId::collider;
        else if constexpr (std::same_as<T, RigidBodyComponent>) return ComponentId::rigid_body;
        else { static_assert(std::same_as<T, PhysicsSettingsComponent>); return ComponentId::physics_settings; }
    }, value);
}
std::optional<ComponentValue> default_component(ComponentId id) {
    switch (id) {
    case ComponentId::name: return NameComponent{};
    case ComponentId::transform: return TransformComponent{};
    case ComponentId::mesh_renderer: return MeshRendererComponent{};
    case ComponentId::camera: return CameraComponent{};
    case ComponentId::light: return LightComponent{};
    case ComponentId::spin: return SpinComponent{};
    case ComponentId::fly_control: return FlyControlComponent{};
    case ComponentId::collider: return ColliderComponent{};
    case ComponentId::rigid_body: return RigidBodyComponent{};
    case ComponentId::physics_settings: return PhysicsSettingsComponent{};
    }
    return std::nullopt;
}
std::optional<PropertyValue> read_property(const ComponentValue& value, PropertyId property) {
    const auto item = binding(component_id(value), property);
    return item ? std::optional{item->read(value)} : std::nullopt;
}
PropertyResult edit_properties(ComponentValue& value, std::span<const PropertyEdit> edits,
                               const PropertyValidationContext& context) {
    auto candidate = value;
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& edit = edits[i];
        const auto item = binding(component_id(value), edit.property);
        if (!item) return {PropertyError::unknown_property, edit.property, "Property ID is not in this component schema"};
        for (size_t j = 0; j < i; ++j) if (edits[j].property == edit.property)
            return {PropertyError::duplicate_property, edit.property, "Property appears more than once in this edit"};
        if (!item->write(candidate, edit.value))
            return {PropertyError::type_mismatch, edit.property, "Value type does not match the property"};
    }
    const auto result = validate(candidate, context);
    if (result) value = std::move(candidate);
    return result;
}
PropertyResult validate_component(ComponentValue& value, const PropertyValidationContext& context) {
    return edit_properties(value, {}, context);
}
std::optional<ComponentValue> read_component(const World& world, EntityHandle entity, ComponentId id) {
    auto value = default_component(id);
    if (!value) return std::nullopt;
    const auto found = std::visit([&]<class T>(T& component) {
        return world.with<T>(entity, [&](const T& stored) { component = stored; });
    }, *value);
    return found ? value : std::nullopt;
}
PropertyResult edit_properties(World& world, EntityHandle entity, ComponentId component,
                               std::span<const PropertyEdit> edits, const PropertyValidationContext& context) {
    if (!component_schema(component)) return {PropertyError::unknown_component, 0, "Unknown component schema"};
    if (!world.alive(entity)) return {PropertyError::invalid_entity, 0, "Entity is stale or belongs to another World"};
    auto value = read_component(world, entity, component);
    if (!value) return {PropertyError::component_missing, 0, "Entity does not have this component"};
    const auto validation = edit_properties(*value, edits, context);
    if (!validation) return validation;
    auto commands = world.commands();
    std::visit([&](auto& candidate) { commands.replace(entity, std::move(candidate)); }, *value);
    const auto result = world.commit(commands);
    if (!result) return {PropertyError::world_rejected, 0, "World rejected property publication", result.error};
    return {};
}
} // namespace maya
