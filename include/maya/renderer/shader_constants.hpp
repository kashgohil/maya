#pragma once

#include "maya/math/matrix.hpp"
#include "maya/renderer/render_snapshot.hpp"
#include <cstddef>
#include <cstdint>

namespace maya {
// GPU layouts shared with resources/shaders/metal/renderer.metal. Buffer indices: 0 vertices,
// 1 per-draw constants, 2 per-view constants, 3 material constants.

struct GpuDirectionalLight {
    math::Vec4 direction_to_light; // xyz unit vector
    math::Vec4 radiance; // rgb
};
/// Uploaded once per view.
struct ViewConstants {
    math::Mat4 view_projection;
    math::Vec4 camera_position; // xyz
    math::Vec4 ambient; // rgb
    uint32_t light_count[4]; // x; the rest is padding
    GpuDirectionalLight lights[max_directional_lights];
};
/// Uploaded once per drawn instance; read by the vertex stage only.
struct DrawConstants {
    math::Mat4 model;
    math::Vec4 normal_matrix[3]; // columns; w unused
};
/// Uploaded when a draw's material differs from the previous draw's; read by the fragment stage only, so
/// per-draw constants do not make Metal re-emit the fragment stage's maps with every draw.
struct MaterialConstants {
    math::Vec4 base_color;
    math::Vec4 factors; // x metallic, y roughness, z normal scale, w occlusion strength
    math::Vec4 emissive; // rgb emitted light, w alpha cutoff
    uint32_t flags[4]; // x a bit per MaterialSlot with a texture, y AlphaMode; the rest is padding
};
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
    uint32_t view; // ExposureView
    uint32_t pad;
};
inline constexpr size_t debug_line_floats = 12;
inline constexpr size_t debug_shape_floats = 24;

static_assert(sizeof(ViewConstants) == 240 && offsetof(ViewConstants, camera_position) == 64 &&
              offsetof(ViewConstants, ambient) == 80 && offsetof(ViewConstants, light_count) == 96 &&
              offsetof(ViewConstants, lights) == 112, "ViewConstants must match renderer.metal");
static_assert(sizeof(DrawConstants) == 112 && offsetof(DrawConstants, normal_matrix) == 64,
              "DrawConstants must match renderer.metal");
static_assert(sizeof(MaterialConstants) == 64 && offsetof(MaterialConstants, factors) == 16 &&
              offsetof(MaterialConstants, emissive) == 32 && offsetof(MaterialConstants, flags) == 48,
              "MaterialConstants must match renderer.metal");
static_assert(sizeof(PresentConstants) == 16, "PresentConstants must match renderer.metal");
static_assert(sizeof(ToneMapConstants) == 16, "ToneMapConstants must match renderer.metal");
static_assert(sizeof(DebugConstants) == 32 && offsetof(DebugConstants, kind) == 16, "DebugConstants must match renderer.metal");
} // namespace maya
