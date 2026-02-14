#pragma once

#include "maya/math/vector.hpp"

namespace maya {

struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec4 color;
    math::Vec2 uv;

    Vertex(const math::Vec3& p, const math::Vec3& n, const math::Vec4& c, const math::Vec2& u = {0.0f, 0.0f}) 
        : position(p), normal(n), color(c), uv(u) {}
};

} // namespace maya
