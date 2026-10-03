#pragma once

#include "maya/math/vector.hpp"

namespace maya {

struct Vertex {
    math::Vec3 position;
    float _pad0;
    math::Vec3 normal;
    float _pad1;
    math::Vec4 color;
    math::Vec2 uv; // texture rows run top to bottom: v grows downward, as in glTF
    float _pad2[2];
    /// xyz along increasing u; w is the bitangent's sign, so cross(normal, tangent) * w points toward
    /// decreasing v (up the texture), glTF's convention. MikkTSpace generates it where a mesh has none.
    math::Vec4 tangent;

    Vertex(const math::Vec3& p, const math::Vec3& n, const math::Vec4& c, const math::Vec2& u = {0.0f, 0.0f},
           const math::Vec4& t = {1.0f, 0.0f, 0.0f, 1.0f})
        : position(p), _pad0(0), normal(n), _pad1(0), color(c), uv(u), tangent(t) {
            _pad2[0] = 0; _pad2[1] = 0;
        }
};

static_assert(sizeof(Vertex) == 80, "Vertex must match the Metal vertex layout");

} // namespace maya
