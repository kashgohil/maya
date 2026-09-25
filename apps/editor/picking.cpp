#include "picking.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace maya::editor {
namespace {
math::Vec3 transform_point(const math::Mat4& m, const math::Vec3& p) {
    const auto v = m * math::Vec4(p, 1.0f);
    return {v.x, v.y, v.z};
}
math::Vec3 transform_direction(const math::Mat4& m, const math::Vec3& d) {
    const auto v = m * math::Vec4(d, 0.0f);
    return {v.x, v.y, v.z};
}
} // namespace

Ray view_ray(const RenderView& view, const math::Mat4& camera_pose, float vertical_fov, float x_ndc, float y_ndc) {
    const auto tangent = std::tan(vertical_fov * 0.5f);
    const auto aspect = view.height ? float(view.width) / float(view.height) : 1.0f;
    const auto local = math::Vec3(x_ndc * tangent * aspect, y_ndc * tangent, -1.0f); // the camera looks along -Z
    return {transform_point(camera_pose, {0.0f, 0.0f, 0.0f}), transform_direction(camera_pose, local).normalized()};
}

std::optional<float> ray_box(const math::Vec3& origin, const math::Vec3& direction, const math::Vec3& min,
                             const math::Vec3& max) {
    auto entry = 0.0f, exit = std::numeric_limits<float>::max();
    const float o[] = {origin.x, origin.y, origin.z}, d[] = {direction.x, direction.y, direction.z};
    const float lo[] = {min.x, min.y, min.z}, hi[] = {max.x, max.y, max.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return std::nullopt;
            continue;
        }
        auto t0 = (lo[axis] - o[axis]) / d[axis], t1 = (hi[axis] - o[axis]) / d[axis];
        if (t0 > t1) std::swap(t0, t1);
        entry = std::max(entry, t0);
        exit = std::min(exit, t1);
        if (entry > exit) return std::nullopt;
    }
    return entry;
}

std::optional<float> ray_triangle(const math::Vec3& origin, const math::Vec3& direction, const math::Vec3& a,
                                  const math::Vec3& b, const math::Vec3& c) {
    // Möller–Trumbore.
    const auto edge1 = b - a, edge2 = c - a;
    const auto p = math::Vec3::cross(direction, edge2);
    const auto determinant = math::Vec3::dot(edge1, p);
    if (std::abs(determinant) < 1e-12f) return std::nullopt;
    const auto inverse = 1.0f / determinant;
    const auto s = origin - a;
    const auto u = math::Vec3::dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) return std::nullopt;
    const auto q = math::Vec3::cross(s, edge1);
    const auto v = math::Vec3::dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) return std::nullopt;
    const auto t = math::Vec3::dot(edge2, q) * inverse;
    return t >= 0.0f ? std::optional{t} : std::nullopt;
}

std::vector<PickHit> pick_meshes(const RenderSnapshot& snapshot, const Ray& ray) {
    auto hits = std::vector<PickHit>{};
    for (const auto& instance : snapshot.instances) {
        const auto& geometry = snapshot.meshes[instance.mesh].value().geometry();
        if (geometry.empty()) continue;
        const auto inverse = inverse_affine(instance.world);
        if (!inverse) continue;
        // The local ray keeps the world ray's parameter: t is the same distance in both spaces.
        const auto origin = transform_point(*inverse, ray.origin);
        const auto direction = transform_direction(*inverse, ray.direction);
        if (!ray_box(origin, direction, geometry.min, geometry.max)) continue;
        auto nearest = std::optional<float>{};
        const auto& p = geometry.positions;
        for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
            const auto t = ray_triangle(origin, direction, p[geometry.indices[i]], p[geometry.indices[i + 1]],
                                        p[geometry.indices[i + 2]]);
            if (t && (!nearest || *t < *nearest)) nearest = t;
        }
        if (nearest) hits.push_back({instance.entity, *nearest});
    }
    std::ranges::sort(hits, {}, &PickHit::distance);
    return hits;
}

} // namespace maya::editor
