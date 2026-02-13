#pragma once

#include "maya/math/vector.hpp"

namespace maya {

struct Vertex {
    math::Vec3 position;
    math::Vec4 color;
    math::Vec2 uv;

    Vertex(const math::Vec3& p, const math::Vec4& c, const math::Vec2& u = {0.0f, 0.0f}) 
        : position(p), color(c), uv(u) {}
};

} // namespace maya
