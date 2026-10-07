#pragma once

#include "maya/assets/material.hpp"
#include "maya/world/world.hpp"
#include <functional>
#include <span>
#include <string_view>
#include <variant>

namespace maya {
// Explicit persistent IDs. Never derive identity from RTTI, ordering, labels, or hashes.
enum class ComponentId : uint32_t {
    name = 1, transform = 2, mesh_renderer = 3, camera = 4, light = 5, spin = 6, fly_control = 7,
    collider = 8, rigid_body = 9, physics_settings = 10, script = 11, environment = 12, skin = 13, animation = 14
};
using PropertyId = uint32_t; // scoped to ComponentId; zero is reserved
using ComponentValue = std::variant<NameComponent, TransformComponent, MeshRendererComponent,
                                    CameraComponent, LightComponent, SpinComponent, FlyControlComponent,
                                    ColliderComponent, RigidBodyComponent, PhysicsSettingsComponent, ScriptComponent,
                                    EnvironmentComponent, SkinComponent, AnimationComponent>;
/// One of a property's named choices (an enumeration such as a light's kind), by its value.
struct ChoiceValue {
    uint32_t value = 0;
    auto operator<=>(const ChoiceValue&) const = default;
};
using PropertyValue = std::variant<std::string, bool, float, math::Vec3, math::Quat, ChoiceValue,
                                  AssetRef<MeshAsset>, AssetRef<MaterialAsset>, int32_t, uint32_t,
                                  AssetRef<ScriptAsset>, std::vector<ScriptValue>, AssetRef<TextureAsset>,
                                  AssetRef<EnvironmentAsset>, math::Vec2, AssetRef<SkinAsset>, AssetRef<AnimationAsset>>;
/// integer is a whole number within the range; flags is a bit set no greater than range.maximum;
/// script_values is the named values of a script component's declared properties.
enum class PropertyType {
    text, boolean, scalar, vector3, quaternion, choice, mesh_ref, material_ref, integer, flags, script_ref, script_values,
    texture_ref, environment_ref, vector2, skin_ref, animation_ref
};
enum class PropertyPresentation {
    text, toggle, number, vector, rotation, color, choice, asset,
    collision_group, collision_mask, // an integer or flags shown with the project's collision group names
    script_values // the properties the attached script declares
};
enum class PropertyEncoding { value, persistent_asset_id };
struct NumericRange {
    std::optional<float> minimum;
    std::optional<float> maximum;
    bool minimum_inclusive = true;
    bool maximum_inclusive = true;
};
struct EnumOption { uint32_t value; std::string_view name; std::string_view label; };
struct PropertyDescriptor {
    PropertyId id;
    std::string_view name; // stable serialization key
    std::string_view label;
    PropertyType type;
    PropertyValue default_value;
    NumericRange range; // per-axis for vector3; all numeric values must be finite
    std::string_view units;
    PropertyPresentation presentation;
    PropertyEncoding encoding;
    std::span<const EnumOption> choices;
    std::string_view description;
    /// The component version that added the property. A scene written at an older version has none
    /// of the properties added since, and loads with their defaults (docs/scene.md#versions-and-migration).
    uint32_t since = 1;
};
struct ComponentDescriptor {
    ComponentId id;
    std::string_view name;
    std::string_view label;
    uint32_t version;
    std::span<const PropertyDescriptor> properties;
};

enum class ReferenceKind { mesh, material, script, texture, environment, skin, animation };
enum class ReferenceStatus { valid, missing, wrong_type };
struct PropertyValidationContext {
    // Synchronous, read-only callback. Must not mutate/re-enter World or asset services.
    // Nonempty references require a resolver; empty references are valid unassigned slots.
    std::function<ReferenceStatus(AssetId, ReferenceKind)> resolve_asset;
};
enum class PropertyError {
    none, unknown_component, unknown_property, type_mismatch, duplicate_property,
    invalid_value, validation_context_required, missing_reference, wrong_reference_type,
    invalid_entity, component_missing, world_rejected
};
struct PropertyResult {
    PropertyError error = PropertyError::none;
    PropertyId property = 0;
    std::string_view message;
    WorldError world_error = WorldError::none;
    explicit operator bool() const noexcept { return error == PropertyError::none; }
};
struct PropertyEdit { PropertyId property; PropertyValue value; };

/// "number", "integer", "boolean", "string", "vector", "color", or "entity".
const char* script_value_type_name(ScriptValueType type) noexcept;
std::optional<ScriptValueType> script_value_type(std::string_view name) noexcept;
/// Why a script component's values are invalid, or empty: names are identifiers of at most 64
/// characters and unique, and each value's data matches its type and is finite.
std::string script_values_problem(const std::vector<ScriptValue>& values);
std::span<const ComponentDescriptor> component_schemas();
const ComponentDescriptor* component_schema(ComponentId id);
const ComponentDescriptor* component_schema(std::string_view name);
const PropertyDescriptor* property_schema(ComponentId component, PropertyId property);
const PropertyDescriptor* property_schema(ComponentId component, std::string_view name);
ComponentId component_id(const ComponentValue& value);
std::optional<ComponentValue> default_component(ComponentId id);
std::optional<PropertyValue> read_property(const ComponentValue& value, PropertyId property);
/// Applies all edits to a private copy, validates the complete final component, then publishes.
/// Duplicate property IDs fail. Allocation/resolver exceptions propagate with no mutation.
PropertyResult edit_properties(ComponentValue& value, std::span<const PropertyEdit> edits,
                               const PropertyValidationContext& context = {});
/// Same validation/normalization as editing; failure leaves the input unchanged.
PropertyResult validate_component(ComponentValue& value, const PropertyValidationContext& context = {});
std::optional<ComponentValue> read_component(const World& world, EntityHandle entity, ComponentId id);
/// Owner-thread operation, one atomic component edit; rejects publication during World borrows.
PropertyResult edit_properties(World& world, EntityHandle entity, ComponentId component,
                               std::span<const PropertyEdit> edits,
                               const PropertyValidationContext& context = {});

/// A material asset's properties (docs/properties.md#materials), edited like a component's. Their
/// names are the material file's keys; base color's alpha is its own property.
std::span<const PropertyDescriptor> material_properties();
const PropertyDescriptor* material_property(PropertyId property);
const PropertyDescriptor* material_property(std::string_view name);
std::optional<PropertyValue> read_property(const MaterialAsset& material, PropertyId property);
/// Applies all edits to a copy, validates the whole material, then publishes; as for components.
PropertyResult edit_properties(MaterialAsset& material, std::span<const PropertyEdit> edits,
                               const PropertyValidationContext& context = {});
PropertyResult validate_material(MaterialAsset& material, const PropertyValidationContext& context = {});
} // namespace maya
