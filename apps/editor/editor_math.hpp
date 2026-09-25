#pragma once

#include "maya/math/quaternion.hpp"
#include <algorithm>
#include <cmath>

namespace maya::editor {

/// Euler angles in degrees for display: rotation = Rz * Ry * Rx, so X is applied first.
inline math::Vec3 euler_degrees(const math::Quat& rotation) {
    // Adding zero turns -0 into +0, so fields never show "-0.0".
    const auto clean = [](math::Vec3 v) { return math::Vec3{v.x + 0.0f, v.y + 0.0f, v.z + 0.0f}; };
    const auto m = rotation.to_mat4();
    constexpr auto degrees = 180.0f / math::PI;
    const auto sine_y = std::clamp(-m.at(2, 0), -1.0f, 1.0f);
    const auto y = std::asin(sine_y);
    if (std::abs(sine_y) < 0.99999f)
        return clean({std::atan2(m.at(2, 1), m.at(2, 2)) * degrees, y * degrees, std::atan2(m.at(1, 0), m.at(0, 0)) * degrees});
    // Gimbal lock: X and Z turn about the same axis; report it all as X.
    return clean({std::atan2(-m.at(1, 2), m.at(1, 1)) * degrees, y * degrees, 0.0f});
}

inline math::Quat from_euler_degrees(const math::Vec3& degrees) {
    constexpr auto radians = math::PI / 180.0f;
    const auto x = math::Quat::from_axis_angle({1, 0, 0}, degrees.x * radians);
    const auto y = math::Quat::from_axis_angle({0, 1, 0}, degrees.y * radians);
    const auto z = math::Quat::from_axis_angle({0, 0, 1}, degrees.z * radians);
    auto q = z * y * x;
    q.normalize();
    return q;
}

} // namespace maya::editor
