#pragma once

#include "maya/math/vector.hpp"

namespace maya {

struct Vertex {
    math::Vec3 position;
    float _pad0;
    math::Vec3 normal;
    float _pad1;
    math::Vec4 color;
    math::Vec2 uv;
    float _pad2[2];

    Vertex(const math::Vec3& p, const math::Vec3& n, const math::Vec4& c, const math::Vec2& u = {0.0f, 0.0f}) 
        : position(p), _pad0(0), normal(n), _pad1(0), color(c), uv(u) {
            _pad2[0] = 0; _pad2[1] = 0;
        }
};

} // namespace maya
