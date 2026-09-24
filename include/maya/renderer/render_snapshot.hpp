#pragma once

#include "maya/assets/registry.hpp"
#include "maya/world/world.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace maya {
inline constexpr size_t max_directional_lights = 4;
inline constexpr size_t max_render_diagnostics = 64;

enum class RenderIssue {
    none, missing_mesh, missing_material, missing_transform, invalid_transform, unsupported_light, light_limit
};
struct RenderDiagnostic {
    RenderIssue code = RenderIssue::none;
    EntityId entity{};
    AssetId asset{}; // invalid when the problem is not about an asset
    std::string message;
};

/// Material factors copied at extraction. Later edits and reloads affect only later snapshots.
struct RenderMaterial {
    math::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f}; // linear RGBA, multiplies the vertex color
    float metallic = 0.0f;
    float roughness = 1.0f;
};
struct RenderInstance {
    EntityId entity{}; // identity for picking and diagnostics; resolve it again before touching a World
    uint32_t mesh = 0; // index into RenderSnapshot::meshes
    math::Mat4 world = math::Mat4::identity();
    /// Columns of the inverse transpose of world's linear part, so nonuniform scale keeps normals
    /// perpendicular to surfaces. The shader renormalizes, so only its direction matters.
    std::array<math::Vec3, 3> normal_matrix{math::Vec3{1, 0, 0}, math::Vec3{0, 1, 0}, math::Vec3{0, 0, 1}};
    RenderMaterial material{};
};
struct RenderDirectionalLight {
    EntityId entity{};
    math::Vec3 direction_to_light{0.0f, 0.0f, 1.0f}; // unit world vector: the light shines along its local -Z
    math::Vec3 radiance{1.0f}; // linear color × intensity; no exposure is applied yet
};
struct RenderExtractOptions {
    math::Vec3 ambient{0.06f, 0.07f, 0.09f}; // linear RGB added to every surface
};
struct RenderSnapshotStats {
    size_t mesh_renderers = 0;
    size_t hidden = 0; // not visible, or no mesh assigned
    size_t skipped = 0; // could not be drawn; see diagnostics
};

/// Immutable renderer input extracted from one World. It holds no World handles or pointers into
/// component storage, and it leases every mesh version it draws, so deleting entities, evicting or
/// reloading assets, or destroying the registry after extraction cannot invalidate it. Instances of
/// one mesh share a single lease and GPU allocation. Release a snapshot once its frames are encoded;
/// the graphics device keeps the GPU buffers alive until those frames complete.
struct RenderSnapshot {
    uint64_t world = 0; // World::token() of the source
    std::vector<AssetLease<MeshAsset>> meshes; // one per distinct mesh asset
    std::vector<RenderInstance> instances;
    std::vector<RenderDirectionalLight> lights; // enabled directional lights in EntityId order
    math::Vec3 ambient{0.0f};
    std::vector<RenderDiagnostic> diagnostics; // capped at max_render_diagnostics
    RenderSnapshotStats stats{};
};

/// Reads the World without modifying it and acquires assets through the registry, which loads
/// unloaded assets synchronously. Missing or failed meshes skip their draw; missing or failed
/// materials use fallback_material(); an unassigned material uses default MaterialAsset factors.
RenderSnapshot extract_render_snapshot(const World& world, AssetRegistry& assets,
                                       const RenderExtractOptions& options = {});

/// One camera's output description. Views are independent of Worlds and windows: several views
/// can render the same snapshot, each into its own target.
struct RenderView {
    uint32_t width = 0; // framebuffer pixels; must match the target
    uint32_t height = 0;
    CameraMatrices matrices{};
    math::Vec3 position{0.0f};
    std::array<double, 4> clear_color{0.1, 0.1, 0.1, 1.0};
};
/// A view from camera data and a rigid world pose, e.g. an editor camera that is tool state.
/// Returns nullopt for a zero size or an invalid camera/pose.
std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Mat4& pose,
                                           uint32_t width, uint32_t height);
/// A view from a camera entity. Returns nullopt for a zero size, a missing camera component, or an
/// invalid pose (see World::camera).
std::optional<RenderView> extract_render_view(const World& world, EntityHandle camera,
                                              uint32_t width, uint32_t height);
} // namespace maya
