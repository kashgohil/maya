#pragma once

#include "maya/renderer/render_snapshot.hpp"
#include <optional>
#include <vector>

namespace maya::editor {

struct Ray {
    math::Vec3 origin{0.0f};
    math::Vec3 direction{0.0f, 0.0f, -1.0f}; // unit length
};
struct PickHit {
    EntityId entity{};
    float distance = 0.0f; // along the ray, in metres
};

/// The ray through a point of a view, given in normalized device coordinates (x right, y up, -1..1),
/// from a camera's rigid pose and vertical field of view. The view's aspect comes from its size.
Ray view_ray(const RenderView& view, const math::Mat4& camera_pose, float vertical_fov, float x_ndc, float y_ndc);

/// Mesh instances the ray hits, nearest first, at most one hit per instance. This is the bounded
/// initial technique: a linear pass over the snapshot's instances with a local bounding-box test,
/// then an exact two-sided triangle test only for instances whose box the ray enters. Cost is
/// O(instances + triangles of candidate meshes); there is no acceleration structure yet. Meshes
/// without CPU geometry cannot be hit.
std::vector<PickHit> pick_meshes(const RenderSnapshot& snapshot, const Ray& ray);

/// Where the ray enters an axis-aligned box, or nullopt when it misses (or the box is behind it).
std::optional<float> ray_box(const math::Vec3& origin, const math::Vec3& direction, const math::Vec3& min,
                             const math::Vec3& max);
/// Where the ray hits a triangle from either side, or nullopt.
std::optional<float> ray_triangle(const math::Vec3& origin, const math::Vec3& direction, const math::Vec3& a,
                                  const math::Vec3& b, const math::Vec3& c);

} // namespace maya::editor
