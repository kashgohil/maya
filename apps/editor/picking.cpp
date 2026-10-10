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

Ray view_ray(const RenderView& view, const math::Affine& camera_pose, float vertical_fov, float x_ndc, float y_ndc) {
    const auto tangent = std::tan(vertical_fov * 0.5f);
    const auto aspect = view.height ? float(view.width) / float(view.height) : 1.0f;
    const auto local = math::Vec3(x_ndc * tangent * aspect, y_ndc * tangent, -1.0f); // the camera looks along -Z
    return {camera_pose.translation, camera_pose.vector(local).normalized()};
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
        // The local ray keeps the world ray's parameter: t is the same distance in both spaces. Instances
        // are relative to the snapshot's origin (#1065), so the ray is moved there in double first.
        const auto origin = transform_point(*inverse, (ray.origin - snapshot.origin).to_float());
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

std::optional<PickHit> pick_terrain(std::span<const TerrainView> terrains, const Ray& ray, float reach) {
    auto best = std::optional<PickHit>{};
    for (const auto& terrain : terrains) {
        // Its height at a point, on the triangles drawn at the finest level (TerrainStreamer::height_at's).
        const auto height = [&](double x, double z) -> std::optional<float> {
            const auto index = TileIndex{int32_t(std::floor(x / tile_size)), int32_t(std::floor(z / tile_size))};
            const auto found = std::ranges::lower_bound(terrain.tiles, index, {}, &TerrainTileView::index);
            if (found == terrain.tiles.end() || found->index != index || !found->heights) return std::nullopt;
            const auto& tile = *found->heights;
            const auto fx = x - double(index.x) * tile_size, fz = z - double(index.z) * tile_size;
            const auto ix = std::min(uint32_t(fx), tile_samples - 2), iz = std::min(uint32_t(fz), tile_samples - 2);
            const auto tx = float(fx - ix), tz = float(fz - iz);
            const auto h00 = tile.height(ix, iz), h10 = tile.height(ix + 1, iz), h01 = tile.height(ix, iz + 1), h11 = tile.height(ix + 1, iz + 1);
            return tx >= tz ? h00 + (h10 - h00) * tx + (h11 - h10) * tz : h00 + (h01 - h00) * tz + (h11 - h01) * tx;
        };
        const auto above = [&](float t) -> std::optional<bool> {
            const auto p = ray.at(t);
            const auto h = height(p.x, p.z);
            if (!h) return std::nullopt;
            return p.y > double(*h);
        };
        auto previous = 0.0f;
        auto was_above = above(0.0f);
        for (float t = 1.0f; t <= reach && (!best || t < best->distance); t += 1.0f) {
            const auto now = above(t);
            if (was_above && *was_above && now && !*now) { // crossed between previous and t: halve it to a centimetre
                auto low = previous, high = t;
                while (high - low > 0.01f) {
                    const auto mid = (low + high) * 0.5f;
                    const auto at_mid = above(mid);
                    if (at_mid && *at_mid) low = mid;
                    else high = mid;
                }
                best = PickHit{terrain.id, high};
                break;
            }
            previous = t;
            was_above = now;
        }
    }
    return best;
}
} // namespace maya::editor
