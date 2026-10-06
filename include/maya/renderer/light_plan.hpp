#pragma once
// What lights a view (#1034, docs/renderer.md#lights): which point and spot lights it draws, which of them
// have shadow maps, and the sun's shadow cascades. Pure calculation from a snapshot and a view, so it is
// tested without a GPU; the renderer encodes its shadow passes and lit pass from it.

#include "maya/renderer/render_snapshot.hpp"
#include <array>
#include <optional>
#include <vector>

namespace maya {
inline constexpr uint32_t sun_cascades = 4;
inline constexpr uint32_t sun_cascade_size = 2048; // texels per side; four in a 4096 atlas, 2 x 2
inline constexpr uint32_t spot_shadow_size = 1024; // texels per side; four in a 2048 atlas, 2 x 2
/// How cascade splits are placed between the view's near plane and the shadow distance: 0 evenly,
/// 1 logarithmically (each the same ratio).
inline constexpr float cascade_split_blend = 0.75f;
/// The fraction of each cascade's depth range, at its far end, over which it blends into the next.
inline constexpr float cascade_blend_band = 0.1f;

/// One of the sun's cascades: an orthographic view along the light around a sphere that holds a slice
/// of the view's frustum. Its size never changes as the view turns, and its centre moves in whole
/// shadow-map texels, so shadows do not shimmer.
struct ShadowCascade {
    math::Mat4 view_projection; // world to the cascade's clip space: x, y in [-1, 1], depth in [0, 1] toward the light
    float near = 0, far = 0; // the view depths (metres along the view's forward axis) it covers
    math::Vec3 center{0.0f};
    float radius = 0;
    float texel = 0; // metres per shadow-map texel
    float depth_range = 0; // metres from depth 0 to depth 1
};
/// A spot light's shadow map: a perspective view from the light through its outer cone.
struct SpotShadow {
    uint32_t light = 0; // index into RenderSnapshot::local_lights
    math::Mat4 view_projection;
    float texel_per_metre = 0; // metres per texel at one metre from the light
    float near = 0, far = 0;
};
struct LightPlan {
    std::vector<uint32_t> local; // RenderSnapshot::local_lights drawn in this view, most important first
    std::vector<uint32_t> dropped; // reaching the view, but beyond max_local_lights
    std::vector<SpotShadow> spot_shadows; // at most max_shadowed_spot_lights, in `local`'s order
    std::vector<uint32_t> unshadowed; // drawn spot lights asking for shadows beyond max_shadowed_spot_lights
    std::optional<uint32_t> sun; // index into RenderSnapshot::lights of the light with cascades
    std::array<ShadowCascade, sun_cascades> cascades{};
    math::Vec3 view_forward{0.0f, 0.0f, -1.0f};
};
/// Plans a view's lights. A point or spot light is drawn when its range reaches the view's frustum;
/// beyond max_local_lights, those with the most light at the view's position (intensity / distance^2)
/// are drawn, ties by EntityId, and the rest dropped. Spot shadow maps go to the most important spot
/// lights casting shadows. The sun's cascades cover the view out to its shadow distance.
LightPlan plan_lights(const RenderSnapshot& snapshot, const RenderView& view);

/// Whether an instance's bounding sphere may cast into a cascade (it overlaps the cascade's box), or into
/// a spot light's map (it is within the light's range).
bool casts_into(const ShadowCascade& cascade, const RenderInstance& instance) noexcept;
bool casts_into(const RenderLocalLight& light, const RenderInstance& instance) noexcept;
/// The light's view matrix the cascades share: looking along the light, +Y up unless the light is near vertical.
math::Mat4 sun_view(const math::Vec3& direction_to_light) noexcept;
} // namespace maya
