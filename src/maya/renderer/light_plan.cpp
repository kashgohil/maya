#include "maya/renderer/light_plan.hpp"
#include "maya/renderer/draw_batches.hpp"
#include "maya/world/spatial.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace maya {
namespace {
math::Vec3 transform_point(const math::Mat4& m, const math::Vec3& p) {
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
            m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}
math::Vec4 transform(const math::Mat4& m, const math::Vec3& p) { return m * math::Vec4{p, 1.0f}; }
/// An orthographic projection of light-view space: x and y in [-radius, radius] about `center` to
/// [-1, 1], and view-space z from `top` (nearest the light) to `bottom` to depth 0 to 1.
math::Mat4 orthographic(const math::Vec3& center, float radius, float top, float bottom) {
    auto m = math::Mat4::identity();
    m.at(0, 0) = 1.0f / radius;
    m.at(0, 3) = -center.x / radius;
    m.at(1, 1) = 1.0f / radius;
    m.at(1, 3) = -center.y / radius;
    m.at(2, 2) = -1.0f / (top - bottom);
    m.at(2, 3) = top / (top - bottom);
    return m;
}
} // namespace

math::Mat4 sun_view(const math::Vec3& direction_to_light) noexcept {
    const auto up = std::abs(direction_to_light.y) > 0.99f ? math::Vec3{0, 0, 1} : math::Vec3{0, 1, 0};
    return math::Mat4::look_at(math::Vec3{0.0f}, -direction_to_light, up);
}

bool casts_into(const ShadowCascade& cascade, const RenderInstance& instance) noexcept {
    if (!std::isfinite(instance.bounds_radius)) return true;
    const auto clip = transform(cascade.view_projection, instance.bounds_center);
    const auto reach = instance.bounds_radius / cascade.radius;
    // Anything nearer the light than depth 0 was included when the cascade was fitted.
    return std::abs(clip.x) <= 1.0f + reach && std::abs(clip.y) <= 1.0f + reach &&
           clip.z - instance.bounds_radius / cascade.depth_range <= 1.0f;
}
bool casts_into(const RenderLocalLight& light, const RenderInstance& instance) noexcept {
    return !std::isfinite(instance.bounds_radius) || (instance.bounds_center - light.position).length() <= light.range + instance.bounds_radius;
}

LightPlan plan_lights(const RenderSnapshot& snapshot, const RenderView& view) {
    auto plan = LightPlan{};
    const auto& matrices = view.matrices;
    plan.view_forward = math::Vec3{-matrices.view.at(2, 0), -matrices.view.at(2, 1), -matrices.view.at(2, 2)}.normalized();

    // Point and spot lights: those reaching the frustum, most light at the view's position first.
    const auto frustum = Frustum::from(matrices.view_projection);
    struct Candidate { uint32_t index; float score; };
    auto candidates = std::vector<Candidate>{};
    for (uint32_t i = 0; i < snapshot.local_lights.size(); ++i) {
        const auto& light = snapshot.local_lights[i];
        if (!frustum.reaches(light.position, light.range)) continue;
        const auto brightest = std::max({light.intensity.x, light.intensity.y, light.intensity.z});
        const auto distance2 = std::max((light.position - view.position).length_squared(), 0.25f);
        candidates.push_back({i, brightest / distance2});
    }
    std::ranges::stable_sort(candidates, [&](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return snapshot.local_lights[a.index].entity < snapshot.local_lights[b.index].entity;
    });
    for (size_t k = 0; k < candidates.size(); ++k) (k < max_local_lights ? plan.local : plan.dropped).push_back(candidates[k].index);

    // Spot shadow maps for the most important spot lights that cast shadows.
    for (const auto index : plan.local) {
        const auto& light = snapshot.local_lights[index];
        if (light.kind != LightKind::spot || !light.shadow.cast) continue;
        if (plan.spot_shadows.size() == max_shadowed_spot_lights) {
            plan.unshadowed.push_back(index);
            continue;
        }
        const auto half = std::acos(std::clamp(light.cos_outer, -1.0f, 1.0f));
        const auto fov = std::min(2.0f * half + 0.02f, 3.0f); // a little margin for the filter's taps
        const auto near = std::max(0.02f, light.range * 0.001f);
        const auto up = std::abs(light.direction.y) > 0.99f ? math::Vec3{0, 0, 1} : math::Vec3{0, 1, 0};
        const auto projection = math::Mat4::perspective(fov, 1.0f, near, light.range);
        const auto look = math::Mat4::look_at(light.position, light.position + light.direction, up);
        plan.spot_shadows.push_back({index, projection * look, 2.0f * std::tan(fov * 0.5f) / float(spot_shadow_size), near, light.range});
    }

    // The sun's cascades.
    const auto sun = std::ranges::find_if(snapshot.lights, [](const RenderDirectionalLight& light) { return light.shadow.cast; });
    if (sun == snapshot.lights.end()) return plan;
    // The view's near and far planes, and its frustum's half extents per metre of depth, from its projection.
    const auto& p = matrices.projection;
    if (p.at(3, 2) != -1.0f || p.at(2, 2) == 0.0f) return plan; // not a perspective view: no cascades
    const auto view_near = p.at(2, 3) / p.at(2, 2);
    const auto view_far = p.at(2, 3) / (p.at(2, 2) + 1.0f);
    const auto tan_x = 1.0f / p.at(0, 0), tan_y = 1.0f / p.at(1, 1);
    const auto far = std::min(view_far, sun->shadow_distance);
    if (!(far > view_near)) return plan;
    plan.sun = uint32_t(sun - snapshot.lights.begin());
    const auto light_view = sun_view(sun->direction_to_light);
    const auto camera = inverse_affine(matrices.view);
    if (!camera) {
        plan.sun.reset();
        return plan;
    }
    // The top of every caster along the light, so casters outside the view still shadow it.
    auto casters_top = -std::numeric_limits<float>::infinity();
    auto unbounded = false;
    for (const auto& instance : snapshot.instances) {
        if (snapshot.materials[instance.material].alpha_mode == AlphaMode::blend) continue;
        if (!std::isfinite(instance.bounds_radius)) {
            unbounded = true;
            continue;
        }
        casters_top = std::max(casters_top, transform_point(light_view, instance.bounds_center).z + instance.bounds_radius);
    }
    auto split = [&](uint32_t i) {
        const auto t = float(i) / float(sun_cascades);
        const auto even = view_near + (far - view_near) * t, ratio = view_near * std::pow(far / view_near, t);
        return cascade_split_blend * ratio + (1.0f - cascade_split_blend) * even;
    };
    for (uint32_t i = 0; i < sun_cascades; ++i) {
        auto& cascade = plan.cascades[i];
        cascade.far = split(i + 1);
        // Each cascade also covers the band where the one before blends into it.
        cascade.near = i == 0 ? view_near : split(i) - cascade_blend_band * (split(i) - (i == 1 ? view_near : split(i - 1)));
        // The slice's corners in view space, then the smallest sphere about the view axis through them.
        const auto corner = [&](float depth, float sx, float sy) { return math::Vec3{sx * tan_x * depth, sy * tan_y * depth, -depth}; };
        const auto a = cascade.near, b = cascade.far;
        const auto k2 = tan_x * tan_x + tan_y * tan_y; // squared half-diagonal per metre of depth
        // Centre at depth c on the axis, equidistant from the near and far corners, kept within the slice.
        auto c = std::clamp(0.5f * (a + b) * (1.0f + k2), a, b);
        const auto near_corner = corner(a, 1, 1), far_corner = corner(b, 1, 1);
        const auto centre_view = math::Vec3{0.0f, 0.0f, -c};
        auto radius = std::max((near_corner - centre_view).length(), (far_corner - centre_view).length());
        radius = std::ceil(radius * 16.0f) / 16.0f; // the same size every frame, whatever the rounding
        const auto centre_world = transform_point(*camera, centre_view);
        auto centre_light = transform_point(light_view, centre_world);
        // Whole texels in the light's view, so the map's texels stay put on the ground as the view moves.
        const auto texel = 2.0f * radius / float(sun_cascade_size);
        centre_light.x = std::floor(centre_light.x / texel) * texel;
        centre_light.y = std::floor(centre_light.y / texel) * texel;
        const auto bottom = centre_light.z - radius;
        auto top = centre_light.z + radius;
        if (unbounded) top = std::max(top, centre_light.z + radius + 2.0f * sun->shadow_distance);
        else if (std::isfinite(casters_top)) top = std::max(top, casters_top);
        cascade.center = transform_point(*inverse_affine(light_view), centre_light);
        cascade.radius = radius;
        cascade.texel = texel;
        cascade.depth_range = top - bottom;
        cascade.view_projection = orthographic(centre_light, radius, top, bottom) * light_view;
    }
    return plan;
}
} // namespace maya
