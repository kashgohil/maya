#pragma once

#include "maya/math/vector.hpp"

namespace maya {

struct Vertex {
    math::Vec3 position;
    float _pad;
    math::Vec4 color;

    Vertex(const math::Vec3& p, const math::Vec4& c) 
        : position(p), _pad(0.0f), color(c) {}
};

} // namespace maya
