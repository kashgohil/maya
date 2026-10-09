#pragma once

#include "maya/assets/registry.hpp"
#include "maya/world/debug_draw.hpp"
#include "maya/world/presentation.hpp"
#include "maya/world/world.hpp"
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace maya {
inline constexpr size_t max_directional_lights = 4;
/// Point and spot lights drawn per view, chosen by their importance to it (docs/renderer.md#lights).
inline constexpr size_t max_local_lights = 16;
/// Spot lights with shadow maps per view, each 1024 texels in a 2048 atlas (docs/renderer.md#shadows).
inline constexpr size_t max_shadowed_spot_lights = 4;
/// Every enabled point and spot light is extracted up to this many; the renderer chooses per view.
inline constexpr size_t max_extracted_local_lights = 1024;
inline constexpr size_t max_render_diagnostics = 64;

enum class RenderIssue {
    none, missing_mesh, missing_material, missing_transform, invalid_transform, light_limit,
    missing_texture, // a material's texture is missing or failed to load: the placeholder is drawn
    texture_role, // a material's texture has the wrong role for its slot (e.g. a color texture as a normal map)
    missing_environment, // the environment is missing or failed to load: the uniform ambient light is used
    environment_limit, // more than one environment component: the one with the lowest EntityId is used
    shadow_limit, // a directional light casts shadows after another one already does: it is drawn unshadowed
    unbound_skin, // a skin is missing, or its joints are not below the entity's ancestors: the mesh is drawn unskinned
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
    math::Mat4 world = math::Mat4::identity(); // relative to RenderSnapshot::origin (#1065)
    /// Columns of the inverse transpose of world's linear part, so nonuniform scale keeps normals
    /// perpendicular to surfaces. The shader renormalizes, so only its direction matters.
    std::array<math::Vec3, 3> normal_matrix{math::Vec3{1, 0, 0}, math::Vec3{0, 1, 0}, math::Vec3{0, 0, 1}};
    uint32_t material = 0; // index into RenderSnapshot::materials, which instances share (#1025)
    /// A sphere around the mesh (relative to RenderSnapshot::origin), for culling views and shadow casters; infinite when the mesh
    /// has no CPU geometry. A skinned mesh's encloses its bounds carried by every joint's skin matrix.
    math::Vec3 bounds_center{0.0f};
    float bounds_radius = std::numeric_limits<float>::infinity();
    /// A skinned mesh's joints (docs/animation.md#skinning): RenderSnapshot::joints from first_joint. Its
    /// vertices are placed by them in world space, and `world` is not applied. None when joint_count is 0.
    uint32_t first_joint = 0;
    uint32_t joint_count = 0;
};
/// A light's shadow settings (LightComponent's), in shadow-map texels.
struct RenderShadow {
    bool cast = false;
    float bias = 1.0f;
    float normal_bias = 1.0f;
};
struct RenderDirectionalLight {
    EntityId entity{};
    math::Vec3 direction_to_light{0.0f, 0.0f, 1.0f}; // unit world vector: the light shines along its local -Z
    math::Vec3 radiance{1.0f}; // linear color × intensity (lux); no exposure is applied yet
    RenderShadow shadow{}; // only the first directional light casting shadows has them; see shadow_limit
    float shadow_distance = 60.0f; // metres from the camera the cascades cover
};
/// A point or spot light (docs/renderer.md#lights): its light at distance d is intensity / d^2, faded to
/// nothing at its range, and a spot's between its inner and outer cone.
struct RenderLocalLight {
    EntityId entity{};
    LightKind kind = LightKind::point;
    math::Vec3 position{0.0f};
    math::Vec3 direction{0.0f, 0.0f, -1.0f}; // a spot's: unit world vector it shines along (local -Z)
    math::Vec3 intensity{1.0f}; // linear color × candela
    float range = 10.0f;
    float cos_inner = 1.0f, cos_outer = 0.0f; // cosines of the cone's half angles, spot only
    RenderShadow shadow{}; // spot lights only
};
/// The scene's environment, from its Environment component (docs/renderer.md#environments).
struct RenderEnvironment {
    EntityId entity{};
    AssetLease<EnvironmentAsset> asset;
    float intensity = 1.0f;
    float rotation = 0.0f; // radians about +Y
    bool background = true;
};
/// Skins' joints as extraction found them below each skinned entity's ancestors (docs/animation.md#binding),
/// kept between extractions while the World's names and hierarchy stay the same (World::names_revision), so
/// steady frames compute joint matrices without searching for joints. Keep one for each World a view draws
/// and pass it in RenderExtractOptions::skins; with another World, or after a rename or reparent, it starts
/// again. Extraction alone fills and reads it.
struct SkinBindingCache {
    struct Binding {
        std::optional<EntityHandle> root; // the ancestor the joints are below, when every one is there
        std::vector<EntityHandle> joints;
        std::string missing; // otherwise, why not
    };
    uint64_t world = 0, names_revision = 0;
    std::map<std::tuple<EntityHandle, AssetHandle<SkinAsset>>, Binding> bindings; // by skinned entity and skin version
};

/// How extraction gets assets that are not resident (docs/assets.md#asynchronous-loading).
enum class AssetLoading : uint8_t {
    wait, // loads them before it returns: tests, tools, and captures
    stream, // never blocks: requests them, skips instances whose mesh, material, or skin is still loading, and
            // draws a texture still loading as its material's factor alone (the editor, the player, the benchmark runner)
};
struct RenderExtractOptions {
    /// Linear RGB light from every direction, used when the scene has no usable environment.
    math::Vec3 ambient{0.06f, 0.07f, 0.09f};
    /// Poses shown in place of the World's, e.g. a play session's between ticks; none when null.
    const PresentationPoses* poses = nullptr;
    /// Debug lines and outlines drawn over the scene (docs/renderer.md#debug-lines); none when null.
    const DebugDraw* debug = nullptr;
    /// Skins' joints from earlier extractions of the same World (#1038); without it, every extraction
    /// searches for them again.
    SkinBindingCache* skins = nullptr;
    AssetLoading loading = AssetLoading::wait;
    /// The snapshot's origin (RenderSnapshot::origin): the camera's position for the view it is drawn for,
    /// so what is near the camera is precise however far from the world's origin it is.
    math::DVec3 origin{};
};
/// Loads everything `world` draws now: its meshes, materials and their textures, skins, and environment.
/// An explicit wait (docs/assets.md#asynchronous-loading), for a loading screen before streaming frames.
/// Returns the number of assets resident afterwards that were not before.
size_t preload_render_assets(const World& world, AssetRegistry& assets);

struct RenderSnapshotStats {
    size_t mesh_renderers = 0;
    size_t hidden = 0; // not visible, or no mesh assigned
    size_t skipped = 0; // could not be drawn; see diagnostics
    // Streaming extraction (AssetLoading::stream): what was still loading.
    size_t pending = 0; // instances not drawn yet: their mesh, material, or skin is loading
    size_t pending_textures = 0; // distinct textures drawn as their materials' factors while they load
    bool environment_pending = false; // the scene's environment is loading: the ambient light is used meanwhile
};

/// Immutable renderer input extracted from one World. It holds no World handles or pointers into
/// component storage, and it leases every mesh and texture version it draws, so deleting entities,
/// evicting or reloading assets, or destroying the registry after extraction cannot invalidate it.
/// Instances of one mesh, and materials using one texture, share a single lease and GPU allocation. Release a snapshot once its frames are encoded;
/// the graphics device keeps the GPU buffers alive until those frames complete.
struct RenderSnapshot {
    uint64_t world = 0; // World::token() of the source
    /// Every position in the snapshot (instances, bounds, joints, lights, debug lines) is relative to this
    /// world position, in float (#1065): extraction subtracts it in double. Usually the view camera's.
    math::DVec3 origin{};
    std::vector<AssetLease<MeshAsset>> meshes; // one per distinct mesh asset
    std::vector<AssetLease<TextureAsset>> textures; // one per distinct texture the materials use
    std::vector<RenderMaterial> materials; // copied once each, and shared by the instances that use them
    std::vector<RenderInstance> instances;
    /// Skin matrices of skinned instances' joints: each joint's shown world matrix times its inverse bind
    /// matrix. Instances of one skin under one ancestor share theirs.
    std::vector<math::Mat4> joints;
    std::vector<RenderDirectionalLight> lights; // enabled directional lights in EntityId order
    std::vector<RenderLocalLight> local_lights; // enabled point and spot lights in EntityId order
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

inline constexpr DebugColor skeleton_bone_color{1.0f, 0.78f, 0.25f, 1.0f};
/// The skeleton debug view (docs/animation.md#debug-view): for every skin that binds, as extraction binds
/// it, a line from each joint to its parent where the parent is a joint of the skin too, and each joint's
/// axes (X red, Y green, Z blue) a third of its skin's mean bone long. Joints are at their shown poses.
void skeleton_debug(const World& world, AssetRegistry& assets, const PresentationPoses* poses, DebugDraw& out,
                    SkinBindingCache* cache = nullptr);

/// What a view shows instead of its lit image (docs/renderer.md#debug-views), one at a time. Material
/// views show the shading's inputs as they are, without exposure or tone mapping; lighting and shadow
/// views are light, exposed and tone-mapped; exposure views show the exposed light by stops.
enum class DebugView : uint8_t {
    none, // the lit, tone-mapped image
    luminance, // exposed luminance in grey by stops from middle grey (0.18): black at -8, white at +8
    false_color, // a color per band of stops from middle grey
    base_color, // as authored: the factor times the map and the vertex color
    normals, // the surface's own normal, world space, as 0.5 + 0.5 n
    shading_normals, // with the normal map, as shading uses it
    metallic, // grey: 0 black, 1 white
    roughness, // perceptual, as authored, in grey
    occlusion, // grey: what darkens light from the surroundings
    emissive, // as authored, clipped at 1
    direct_light, // the lit image from directional, point, and spot lights alone
    environment_light, // the lit image from the surroundings alone (environment or ambient)
    lighting, // all light on a white, fully rough, nonmetallic surface: lighting without albedo
    cascades, // tinted by the sun's cascade that shadows each surface: red, green, blue, yellow
    texels, // and a checker of that cascade's shadow-map texels
};
/// Whether the view's lit pass draws it (with the debug pipelines), rather than the tone-mapping pass alone.
constexpr bool lit_debug_view(DebugView view) noexcept { return view >= DebugView::base_color; }
/// Whether it shows the shading's inputs, unexposed.
constexpr bool material_debug_view(DebugView view) noexcept { return view >= DebugView::base_color && view <= DebugView::emissive; }
/// Stable names, for preferences and command lines ("none", "false-color", ...), and back.
std::string_view debug_view_name(DebugView view) noexcept;
std::optional<DebugView> debug_view_named(std::string_view name) noexcept;
/// The scale an exposure in EV100 applies to scene values: 1 / (1.2 x 2^EV100), the photometric
/// saturation-based exposure (ISO 100, K = 12.5, q = 0.65).
float exposure_scale(float ev100) noexcept;

/// One camera's output description. Views are independent of Worlds and windows: several views
/// can render the same snapshot, each into its own target.
struct RenderView {
    uint32_t width = 0; // framebuffer pixels; must match the target
    uint32_t height = 0;
    CameraMatrices matrices{}; // camera-relative: the view has no translation (#1065)
    math::DVec3 position{}; // the camera's world position (matrices.origin)
    std::array<double, 4> clear_color{0.1, 0.1, 0.1, 1.0}; // scene light where nothing is drawn, before exposure
    float debug_line_width = 1.5f; // framebuffer pixels
    float exposure = exposure_scale(0.0f); // the camera's, as a scale
    ToneMapping tone_mapping = ToneMapping::agx;
    DebugView debug_view = DebugView::none; // what it shows instead of the lit image
};
/// A view from camera data and a rigid world pose, e.g. an editor camera that is tool state.
/// Returns nullopt for a zero size or an invalid camera/pose.
std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Affine& pose,
                                           uint32_t width, uint32_t height);
/// A view from a camera entity, at its pose in `poses` when it has one there. Returns nullopt for a
/// zero size, a missing camera component, or an invalid pose (see World::camera).
std::optional<RenderView> extract_render_view(const World& world, EntityHandle camera,
                                              uint32_t width, uint32_t height, const PresentationPoses* poses = nullptr);

/// A view placed in a snapshot's frame (#1065): its camera's offset from the snapshot's origin (`eye`),
/// and its matrices with that offset applied. The renderer, its light plan, and its batches work in
/// this frame; for the camera the snapshot was extracted for, `eye` is zero.
struct ViewFrame {
    CameraMatrices matrices;
    math::Vec3 eye{0.0f};
};
ViewFrame view_frame(const RenderSnapshot& snapshot, const RenderView& view) noexcept;
} // namespace maya
