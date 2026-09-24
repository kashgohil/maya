#pragma once

#include "maya/properties/schema.hpp"
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace maya {
inline constexpr uint32_t scene_format_version = 1;
inline constexpr size_t max_scene_diagnostics = 64;

/// Detached authored data: persistent IDs and schema component values only. No runtime handles.
struct SceneEntity {
    EntityId id{};
    std::optional<EntityId> parent; // another entity in this document; both need a Transform
    std::vector<ComponentValue> components; // at most one per ComponentId
};
/// Entity order is also sibling order: children appear in their parent's child order.
struct SceneDocument {
    std::vector<SceneEntity> entities;
};

enum class SceneError {
    none, io_error, malformed, unsupported_version, unknown_component, unknown_property,
    missing_property, duplicate_entity, duplicate_component, duplicate_property, invalid_value,
    invalid_hierarchy, missing_asset, wrong_asset_type, validation_context_required, world_rejected
};
struct SceneDiagnostic {
    SceneError code = SceneError::none;
    std::string message; // includes the location and a suggested remedy
    size_t line = 0; // 1-based source line; 0 when not read from text
    EntityId entity{}; // invalid when not entity-specific
    explicit operator bool() const noexcept { return code != SceneError::none; }
};
using SceneDiagnostics = std::vector<SceneDiagnostic>; // empty on success, capped at max_scene_diagnostics

struct SceneDocumentResult {
    SceneDocument document; // empty unless the whole document is valid
    SceneDiagnostics diagnostics;
    explicit operator bool() const noexcept { return diagnostics.empty(); }
};
struct SceneWorldResult {
    std::unique_ptr<World> world; // null on failure; a fresh lifetime, never a caller's World
    SceneDiagnostics diagnostics;
    explicit operator bool() const noexcept { return static_cast<bool>(world); }
};

/// Snapshot of schema components and hierarchy. Other native component types are runtime state.
/// Roots are ordered by EntityId, followed depth-first by children in World sibling order.
SceneDocument capture_scene(const World& world);
/// Validates IDs, hierarchy, and complete components; normalizes rotations in place.
/// Nonempty asset references require a resolver, such as asset_property_context(registry).
SceneDiagnostics validate_scene(SceneDocument& document, const PropertyValidationContext& context);
/// Validates, then builds a new World in one commit. Callers replace their active World only on success.
SceneWorldResult instantiate_scene(SceneDocument document, const PropertyValidationContext& context);

/// Parses the versioned text format, then validates. No partial document is returned.
SceneDocumentResult read_scene(std::string_view text, const PropertyValidationContext& context);
SceneDocumentResult read_scene(std::istream& input, const PropertyValidationContext& context);
/// Validates before emitting anything; invalid documents write nothing.
SceneDiagnostics write_scene(std::ostream& output, SceneDocument document,
                             const PropertyValidationContext& context);

SceneDocumentResult load_scene_file(const std::filesystem::path& path,
                                    const PropertyValidationContext& context);
/// load_scene_file followed by instantiate_scene.
SceneWorldResult open_scene_file(const std::filesystem::path& path,
                                 const PropertyValidationContext& context);
/// Validates, writes a private temporary file in the destination directory, flushes it, then
/// renames it over the destination. Any failure leaves the previous file untouched.
SceneDiagnostics save_scene_file(const std::filesystem::path& path, SceneDocument document,
                                 const PropertyValidationContext& context);
} // namespace maya
