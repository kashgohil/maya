#pragma once

#include "maya/math/affine.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace maya {

/// A debug color, as the view stores it: RGBA from 0 to 1, alpha blending over the scene.
using DebugColor = math::Vec4;

/// A wire outline drawn from a unit template: the renderer places the template with `world`, after
/// stretching it by `size`. The kinds match the collider shapes (docs/renderer.md#debug-lines).
enum class DebugShapeKind : uint8_t {
    box, // size: half extents
    sphere, // size.x: radius
    capsule, // size.x: radius, size.y: half the straight section along local Y (the caps stay round)
};
struct DebugShape {
    DebugShapeKind kind = DebugShapeKind::box;
    math::Mat4 world = math::Mat4::identity(); // relative to DebugDraw::origin; for a capsule, rotation and translation only
    math::Vec3 size{0.5f};
    DebugColor color{1.0f, 1.0f, 1.0f, 1.0f};
};
struct DebugLine {
    math::Vec3 from{0.0f}; // relative to DebugDraw::origin
    math::Vec3 to{0.0f};
    DebugColor color{1.0f, 1.0f, 1.0f, 1.0f};
};

/// World-space lines and shape outlines for debug views: plain data from any producer (physics debug
/// views, tools) that extraction copies into a render snapshot. Nothing here refers to a World.
/// Producers pass world positions (in double, #1065); they are stored relative to `origin` in float,
/// so set `origin` near what is drawn (the camera) before adding to it.
struct DebugDraw {
    math::DVec3 origin{};
    std::vector<DebugShape> shapes;
    std::vector<DebugLine> lines; // bright in front of the scene, faint where it hides them
    std::vector<DebugLine> xray_lines; // as bright where the scene hides them: what is inside things, such as skeletons

    bool empty() const noexcept { return shapes.empty() && lines.empty() && xray_lines.empty(); }
    /// The same lines and shapes, stored relative to `to` instead (a render snapshot's origin).
    void rebase(const math::DVec3& to) {
        const auto shift = (origin - to).to_float();
        origin = to;
        if (shift.x == 0.0f && shift.y == 0.0f && shift.z == 0.0f) return;
        for (auto* list : {&lines, &xray_lines})
            for (auto& line : *list) {
                line.from += shift;
                line.to += shift;
            }
        for (auto& shape : shapes) {
            shape.world.at(0, 3) += shift.x;
            shape.world.at(1, 3) += shift.y;
            shape.world.at(2, 3) += shift.z;
        }
    }
    void clear() noexcept {
        shapes.clear();
        lines.clear();
        xray_lines.clear();
    }
    void line(const math::DVec3& from, const math::DVec3& to, const DebugColor& color) {
        lines.push_back({(from - origin).to_float(), (to - origin).to_float(), color});
    }
    void xray_line(const math::DVec3& from, const math::DVec3& to, const DebugColor& color) {
        xray_lines.push_back({(from - origin).to_float(), (to - origin).to_float(), color});
    }
    /// Three short axis-aligned lines through `at`, `size` long.
    void cross(const math::DVec3& at, float size, const DebugColor& color) {
        const auto h = double(size) * 0.5;
        line(at - math::DVec3{h, 0, 0}, at + math::DVec3{h, 0, 0}, color);
        line(at - math::DVec3{0, h, 0}, at + math::DVec3{0, h, 0}, color);
        line(at - math::DVec3{0, 0, h}, at + math::DVec3{0, 0, h}, color);
    }
    /// A line with a four-sided head at `to`, a fifth of its length.
    void arrow(const math::DVec3& from, const math::DVec3& to, const DebugColor& color) {
        line(from, to, color);
        const auto along = (to - from).to_float();
        const auto length = along.length();
        if (!(length > 1e-6f)) return;
        const auto d = along * (1.0f / length);
        // Two directions across the arrow, from whichever axis is least parallel to it.
        const auto axis = std::abs(d.x) < 0.9f ? math::Vec3{1, 0, 0} : math::Vec3{0, 1, 0};
        const auto u = math::Vec3::cross(d, axis).normalized();
        const auto v = math::Vec3::cross(d, u);
        const auto back = to - math::DVec3(d * (length * 0.2f));
        const auto spread = length * 0.07f;
        for (const auto& side : {u, u * -1.0f, v, v * -1.0f}) line(to, back + math::DVec3(side * spread), color);
    }
    void box(const math::Affine& world, const math::Vec3& half_extents, const DebugColor& color) {
        shapes.push_back({DebugShapeKind::box, world.relative_to(origin), half_extents, color});
    }
    void sphere(const math::Affine& world, float radius, const DebugColor& color) {
        shapes.push_back({DebugShapeKind::sphere, world.relative_to(origin), math::Vec3(radius), color});
    }
    /// `world` must be rigid (rotation and translation): the caps stay round.
    void capsule(const math::Affine& world, float radius, float half_height, const DebugColor& color) {
        shapes.push_back({DebugShapeKind::capsule, world.relative_to(origin), {radius, half_height, radius}, color});
    }
};

/// Segments in a full circle of a sphere or capsule outline: fewer for outlines small on screen.
inline constexpr uint32_t debug_circle_segments = 32;
/// The segments for an outline whose radius covers `radius_pixels` on screen: 8, 16, or 32, so
/// segments stay a few pixels long and distant outlines cost little.
constexpr uint32_t debug_segments_for(float radius_pixels) noexcept {
    return radius_pixels < 6.0f ? 8 : radius_pixels < 20.0f ? 16 : debug_circle_segments;
}
/// Line segments in each shape kind's unit template, which the renderer draws per shape.
constexpr uint32_t debug_template_lines(DebugShapeKind kind, uint32_t segments = debug_circle_segments) noexcept {
    switch (kind) {
    case DebugShapeKind::box: return 12;
    case DebugShapeKind::sphere: return 3 * segments; // three great circles
    case DebugShapeKind::capsule: break;
    }
    return 4 * segments + 4; // two rings, four sides, and two arcs over each cap
}

} // namespace maya
