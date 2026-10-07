#pragma once

#include "maya/math/matrix.hpp"
#include "maya/renderer/light_plan.hpp"
#include "maya/renderer/render_snapshot.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace maya {
// GPU layouts shared with resources/shaders/metal/renderer.metal. Buffer indices: 0 vertices,
// 1 per-draw constants, 2 per-view constants (or a shadow map's ShadowConstants), 3 material constants,
// 4 the pass's draw order, 5 a skinned mesh's SkinVertex stream, 6 the frame's joint palette.
// Texture slots: 0-4 a material's maps, 5 the environment's specular cube, 6 its background, 7 the
// split-sum table, 8 the sun's shadow atlas, 9 the spot lights'; sampler slots 0-4 the maps', 5 the
// environment's, 7 the table's, 8 the shadow maps' comparison sampler.

struct GpuDirectionalLight {
    math::Vec4 direction_to_light; // xyz unit vector
    math::Vec4 radiance; // rgb
};
/// A point or spot light (RenderLocalLight).
struct GpuLocalLight {
    math::Vec4 position_range; // xyz, w range
    math::Vec4 direction_spot; // xyz a spot's direction, w 1 for a spot and 0 for a point
    math::Vec4 intensity; // rgb candela, w its spot shadow map + 1 (0: none)
    math::Vec4 cone; // x cos of the outer half angle, y 1 / (cos inner - cos outer), z bias and w normal bias in texels
};
/// The view's shadow maps (LightPlan): offsets are applied in world space, in texels of the map used.
struct GpuShadows {
    math::Mat4 cascades[sun_cascades]; // world to each cascade's clip space
    math::Vec4 cascade_far; // the view depth where each cascade ends
    math::Vec4 cascade_texel; // metres per texel
    math::Vec4 sun; // x bias and y normal bias in texels, z the view's near plane, w the sun's index in lights + 1 (0: none)
    math::Mat4 spots[max_shadowed_spot_lights]; // world to each spot light's clip space
    math::Vec4 spot_texel; // metres per texel at one metre from each spot light
    math::Vec4 view_forward; // xyz the view's forward axis, w the DebugView (read by the debug pipelines only)
};
/// Uploaded once per view.
struct ViewConstants {
    math::Mat4 view_projection;
    math::Vec4 camera_position; // xyz
    math::Vec4 ambient; // rgb
    uint32_t light_count[4]; // x; the rest is padding
    GpuDirectionalLight lights[max_directional_lights];
    math::Mat4 inverse_view_projection; // for the sky's view rays
    math::Vec4 environment; // x intensity, y cos and z sin of the rotation, w the specular cube's last level
    uint32_t environment_flags[4]; // x an environment lights the scene, y the sky is drawn
    math::Vec4 irradiance[9]; // spherical-harmonic coefficients (rgb) of the environment's irradiance
    uint32_t local_count[4]; // x the point and spot lights drawn; the rest is padding
    GpuLocalLight local_lights[max_local_lights];
    GpuShadows shadows;
};
/// Uploaded once per shadow map.
struct ShadowConstants {
    math::Mat4 view_projection;
};
/// Uploaded once per drawn instance; read by the vertex stage only.
struct DrawConstants {
    math::Mat4 model;
    math::Vec4 normal_matrix[3]; // columns; w unused
    uint32_t skin[4]; // x the first joint in the palette, y the skin's joints (0: not skinned); the rest is padding
};
/// Uploaded when a draw's material differs from the previous draw's; read by the fragment stage only, so
/// per-draw constants do not make Metal re-emit the fragment stage's maps with every draw.
struct MaterialConstants {
    math::Vec4 base_color;
    math::Vec4 factors; // x metallic, y roughness, z normal scale, w occlusion strength
    math::Vec4 emissive; // rgb emitted light, w alpha cutoff
    uint32_t flags[4]; // x a bit per MaterialSlot with a texture, y AlphaMode; the rest is padding
    // Texture coordinates for every map: u' = dot(uv_transform.xy, uv), v' = dot(uv_transform.zw, uv),
    // plus uv_offset.xy (material_uv_transform).
    math::Vec4 uv_transform;
    math::Vec4 uv_offset; // zw padding
};
/// The rows of a material's texture-coordinate matrix: rotate(rotation) x scale.
inline math::Vec4 material_uv_transform(float rotation, const math::Vec2& scale) noexcept {
    const auto c = std::cos(rotation), s = std::sin(rotation);
    return {c * scale.x, s * scale.y, -s * scale.x, c * scale.y};
}
/// Presentation of a view texture into part of another target, in normalized device coordinates.
struct PresentConstants {
    math::Vec4 area; // left, bottom, right, top
};
/// Uploaded once per debug draw. Buffer 0 holds that draw's lines (from, to, color: three float4
/// each) or outlines (world matrix columns, size, color: six float4 each).
struct DebugConstants {
    math::Vec4 viewport; // width and height in pixels, line width in pixels, opacity
    uint32_t kind[4]; // x: 0 lines, else DebugShapeKind + 1; y: circle segments; the rest is padding
};
/// The tone-mapping pass's settings, uploaded once per view.
struct ToneMapConstants {
    float exposure; // a scale: exposure_scale(EV100)
    uint32_t tone_mapping; // ToneMapping
    uint32_t view; // 0 light, 1 luminance, 2 false color, 3 material inputs as they are
    uint32_t pad;
};
inline constexpr size_t debug_line_floats = 12;
inline constexpr size_t debug_shape_floats = 24;

static_assert(offsetof(ViewConstants, camera_position) == 64 &&
              offsetof(ViewConstants, ambient) == 80 && offsetof(ViewConstants, light_count) == 96 &&
              offsetof(ViewConstants, lights) == 112 && offsetof(ViewConstants, inverse_view_projection) == 240 &&
              offsetof(ViewConstants, environment) == 304 && offsetof(ViewConstants, environment_flags) == 320 &&
              offsetof(ViewConstants, irradiance) == 336, "ViewConstants must match renderer.metal");
static_assert(offsetof(ViewConstants, local_count) == 480 && offsetof(ViewConstants, local_lights) == 496 &&
              offsetof(ViewConstants, shadows) == 496 + 64 * max_local_lights && sizeof(GpuShadows) == 592 &&
              sizeof(ViewConstants) == 496 + 64 * max_local_lights + 592, "ViewConstants' lights and shadows must match renderer.metal");
static_assert(sizeof(DrawConstants) == 128 && offsetof(DrawConstants, normal_matrix) == 64 && offsetof(DrawConstants, skin) == 112,
              "DrawConstants must match renderer.metal");
static_assert(sizeof(MaterialConstants) == 96 && offsetof(MaterialConstants, factors) == 16 &&
              offsetof(MaterialConstants, emissive) == 32 && offsetof(MaterialConstants, flags) == 48 &&
              offsetof(MaterialConstants, uv_transform) == 64 && offsetof(MaterialConstants, uv_offset) == 80,
              "MaterialConstants must match renderer.metal");
static_assert(sizeof(PresentConstants) == 16, "PresentConstants must match renderer.metal");
static_assert(sizeof(ToneMapConstants) == 16, "ToneMapConstants must match renderer.metal");
static_assert(sizeof(DebugConstants) == 32 && offsetof(DebugConstants, kind) == 16, "DebugConstants must match renderer.metal");
} // namespace maya
