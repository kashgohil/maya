#pragma once

#include "maya/world/world.hpp"
#include <functional>
#include <span>
#include <string_view>
#include <variant>

namespace maya {
// Explicit persistent IDs. Never derive identity from RTTI, ordering, labels, or hashes.
enum class ComponentId : uint32_t { name = 1, transform = 2, mesh_renderer = 3, camera = 4, light = 5 };
using PropertyId = uint32_t; // scoped to ComponentId; zero is reserved
using ComponentValue = std::variant<NameComponent, TransformComponent, MeshRendererComponent,
                                    CameraComponent, LightComponent>;
using PropertyValue = std::variant<std::string, bool, float, math::Vec3, math::Quat,
                                  LightKind, AssetRef<MeshAsset>, AssetRef<MaterialAsset>>;
enum class PropertyType { text, boolean, scalar, vector3, quaternion, light_kind, mesh_ref, material_ref };
enum class PropertyPresentation { text, toggle, number, vector, rotation, color, choice, asset };
enum class PropertyEncoding { value, persistent_asset_id };
struct NumericRange {
    std::optional<float> minimum;
    std::optional<float> maximum;
    bool minimum_inclusive = true;
    bool maximum_inclusive = true;
};
struct EnumOption { LightKind value; std::string_view name; std::string_view label; };
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
};
struct ComponentDescriptor {
    ComponentId id;
    std::string_view name;
    std::string_view label;
    uint32_t version;
    std::span<const PropertyDescriptor> properties;
};

enum class ReferenceKind { mesh, material };
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
} // namespace maya
