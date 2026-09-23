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
    else if constexpr (std::same_as<T, LightKind>) return PropertyType::light_kind;
    else if constexpr (std::same_as<T, AssetRef<MeshAsset>>) return PropertyType::mesh_ref;
    else { static_assert(std::same_as<T, AssetRef<MaterialAsset>>); return PropertyType::material_ref; }
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
    return {{id, name, label, type, C{}.*Member, range, units, presentation, encoding, choices, description},
        [](const ComponentValue& value) -> PropertyValue { return std::get<C>(value).*Member; },
        [](ComponentValue& value, const PropertyValue& input) {
            const auto typed = std::get_if<V>(&input);
            if (!typed) return false;
            std::get<C>(value).*Member = *typed;
            return true;
        }};
}
using Hint = PropertyPresentation;
constexpr auto positive = NumericRange{0.0f, {}, false, true};
constexpr auto nonnegative = NumericRange{0.0f, {}, true, true};
constexpr auto angle = NumericRange{0.0f, math::PI, false, false};
constexpr auto cone = NumericRange{0.0f, math::PI, true, false};
constexpr auto light_options = std::array{
    EnumOption{LightKind::directional, "directional", "Directional"},
    EnumOption{LightKind::point, "point", "Point"},
    EnumOption{LightKind::spot, "spot", "Spot"}};
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
        } else if (const auto kind = std::get_if<LightKind>(&input)) {
            auto found = false;
            for (const auto& option : d.choices) found |= option.value == *kind;
            if (!found) return {PropertyError::invalid_value, d.id, "Unknown light kind"};
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
    static const auto schemas = std::array{
        ComponentDescriptor{ComponentId::name, "maya.name", "Name", 1, names},
        ComponentDescriptor{ComponentId::transform, "maya.transform", "Transform", 1, transforms},
        ComponentDescriptor{ComponentId::mesh_renderer, "maya.mesh_renderer", "Mesh renderer", 1, meshes},
        ComponentDescriptor{ComponentId::camera, "maya.camera", "Camera", 1, cameras},
        ComponentDescriptor{ComponentId::light, "maya.light", "Light", 1, lights}};
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
        else return ComponentId::light;
    }, value);
}
std::optional<ComponentValue> default_component(ComponentId id) {
    switch (id) {
    case ComponentId::name: return NameComponent{};
    case ComponentId::transform: return TransformComponent{};
    case ComponentId::mesh_renderer: return MeshRendererComponent{};
    case ComponentId::camera: return CameraComponent{};
    case ComponentId::light: return LightComponent{};
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
