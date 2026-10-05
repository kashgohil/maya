#pragma once

#include "maya/assets/registry.hpp"
#include "maya/world/debug_draw.hpp"
#include "maya/world/presentation.hpp"
#include "maya/world/world.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace maya {
inline constexpr size_t max_directional_lights = 4;
inline constexpr size_t max_render_diagnostics = 64;

enum class RenderIssue {
    none, missing_mesh, missing_material, missing_transform, invalid_transform, unsupported_light, light_limit,
    missing_texture, // a material's texture is missing or failed to load: the placeholder is drawn
    texture_role, // a material's texture has the wrong role for its slot (e.g. a color texture as a normal map)
    missing_environment, // the environment is missing or failed to load: the uniform ambient light is used
    environment_limit, // more than one environment component: the one with the lowest EntityId is used
};
struct RenderDiagnostic {
    RenderIssue code = RenderIssue::none;
    EntityId entity{};
    AssetId asset{}; // invalid when the problem is not about an asset
    std::string message;
};

/// A material's texture slots, in the order the shader binds them.
enum class MaterialSlot : uint8_t { base_color, metallic_roughness, normal, occlusion, emissive };
inline constexpr size_t material_slots = 5;
inline constexpr uint32_t no_texture = UINT32_MAX; // the slot is empty: only its factor applies
inline constexpr uint32_t placeholder_texture = UINT32_MAX - 1; // missing or unusable: the renderer's placeholder
/// A material copied at extraction (docs/renderer.md#materials). Later edits and reloads affect only
/// later snapshots.
struct RenderMaterial {
    math::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f}; // linear RGBA, multiplies the vertex color
    float metallic = 0.0f;
    float roughness = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    math::Vec3 emissive{0.0f}; // linear RGB, times its strength
    AlphaMode alpha_mode = AlphaMode::opaque;
    float alpha_cutoff = 0.5f;
    bool double_sided = false;
    math::Vec2 uv_offset{0.0f, 0.0f}; // as MaterialAsset's
    float uv_rotation = 0.0f;
    math::Vec2 uv_scale{1.0f, 1.0f};
    /// Per MaterialSlot: an index into RenderSnapshot::textures, no_texture, or placeholder_texture.
    std::array<uint32_t, material_slots> textures{no_texture, no_texture, no_texture, no_texture, no_texture};
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
/// The scene's environment, from its Environment component (docs/renderer.md#environments).
struct RenderEnvironment {
    EntityId entity{};
    AssetLease<EnvironmentAsset> asset;
    float intensity = 1.0f;
    float rotation = 0.0f; // radians about +Y
    bool background = true;
};
struct RenderExtractOptions {
    /// Linear RGB light from every direction, used when the scene has no usable environment.
    math::Vec3 ambient{0.06f, 0.07f, 0.09f};
    /// Poses shown in place of the World's, e.g. a play session's between ticks; none when null.
    const PresentationPoses* poses = nullptr;
    /// Debug lines and outlines drawn over the scene (docs/renderer.md#debug-lines); none when null.
    const DebugDraw* debug = nullptr;
};
struct RenderSnapshotStats {
    size_t mesh_renderers = 0;
    size_t hidden = 0; // not visible, or no mesh assigned
    size_t skipped = 0; // could not be drawn; see diagnostics
};

/// Immutable renderer input extracted from one World. It holds no World handles or pointers into
/// component storage, and it leases every mesh and texture version it draws, so deleting entities,
/// evicting or reloading assets, or destroying the registry after extraction cannot invalidate it.
/// Instances of one mesh, and materials using one texture, share a single lease and GPU allocation. Release a snapshot once its frames are encoded;
/// the graphics device keeps the GPU buffers alive until those frames complete.
struct RenderSnapshot {
    uint64_t world = 0; // World::token() of the source
    std::vector<AssetLease<MeshAsset>> meshes; // one per distinct mesh asset
    std::vector<AssetLease<TextureAsset>> textures; // one per distinct texture the materials use
    std::vector<RenderInstance> instances;
    std::vector<RenderDirectionalLight> lights; // enabled directional lights in EntityId order
    math::Vec3 ambient{0.0f};
    /// The environment that lights the scene in place of the ambient light, when there is one.
    std::optional<RenderEnvironment> environment;
    std::vector<RenderDiagnostic> diagnostics; // capped at max_render_diagnostics
    RenderSnapshotStats stats{};
    DebugDraw debug; // copied from RenderExtractOptions::debug; empty draws nothing and costs nothing
};

/// Reads the World without modifying it and acquires assets through the registry, which loads
/// unloaded assets synchronously. Missing or failed meshes skip their draw; missing or failed
/// materials use fallback_material(); an unassigned material uses default MaterialAsset factors.
/// Missing or failed textures, and textures of the wrong role, draw the placeholder. A missing or failed
/// environment leaves the ambient light, and is reported.
RenderSnapshot extract_render_snapshot(const World& world, AssetRegistry& assets,
                                       const RenderExtractOptions& options = {});

/// What the tone-mapping pass shows (docs/renderer.md#exposure-views): the image, or diagnostic views
/// of the exposed scene's luminance.
enum class ExposureView : uint8_t {
    none, // the tone-mapped image
    luminance, // grey by stops from middle grey (0.18): black at -8, white at +8
    false_color, // a color per band of stops from middle grey
};
/// The scale an exposure in EV100 applies to scene values: 1 / (1.2 x 2^EV100), the photometric
/// saturation-based exposure (ISO 100, K = 12.5, q = 0.65).
float exposure_scale(float ev100) noexcept;

/// One camera's output description. Views are independent of Worlds and windows: several views
/// can render the same snapshot, each into its own target.
struct RenderView {
    uint32_t width = 0; // framebuffer pixels; must match the target
    uint32_t height = 0;
    CameraMatrices matrices{};
    math::Vec3 position{0.0f};
    std::array<double, 4> clear_color{0.1, 0.1, 0.1, 1.0}; // scene light where nothing is drawn, before exposure
    float debug_line_width = 1.5f; // framebuffer pixels
    float exposure = exposure_scale(0.0f); // the camera's, as a scale
    ToneMapping tone_mapping = ToneMapping::agx;
    ExposureView exposure_view = ExposureView::none;
};
/// A view from camera data and a rigid world pose, e.g. an editor camera that is tool state.
/// Returns nullopt for a zero size or an invalid camera/pose.
std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Mat4& pose,
                                           uint32_t width, uint32_t height);
/// A view from a camera entity, at its pose in `poses` when it has one there. Returns nullopt for a
/// zero size, a missing camera component, or an invalid pose (see World::camera).
std::optional<RenderView> extract_render_view(const World& world, EntityHandle camera,
                                              uint32_t width, uint32_t height, const PresentationPoses* poses = nullptr);
} // namespace maya
