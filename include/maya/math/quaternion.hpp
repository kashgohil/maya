#pragma once

#include "maya/math/matrix.hpp"
#include <cmath>

namespace maya::math {

struct Quat {
    float x, y, z, w;

    constexpr Quat() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {} // Identity
    constexpr Quat(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    static Quat identity() { return Quat(0, 0, 0, 1); }

    // Construction from Axis-Angle
    static Quat from_axis_angle(const Vec3& axis, float angle) {
        float half_angle = angle * 0.5f;
        float s = std::sin(half_angle);
        return Quat(
            axis.x * s,
            axis.y * s,
            axis.z * s,
            std::cos(half_angle)
        );
    }

    // Multiplication (Rotation composition)
    Quat operator*(const Quat& other) const {
        return Quat(
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w,
            w * other.w - x * other.x - y * other.y - z * other.z
        );
    }

    // Convert to Rotation Matrix
    Mat4 to_mat4() const {
        Mat4 result(1.0f);
        float xx = x * x; float yy = y * y; float zz = z * z;
        float xy = x * y; float xz = x * z; float yz = y * z;
        float wx = w * x; float wy = w * y; float wz = w * z;

        result.at(0, 0) = 1.0f - 2.0f * (yy + zz);
        result.at(0, 1) = 2.0f * (xy - wz);
        result.at(0, 2) = 2.0f * (xz + wy);

        result.at(1, 0) = 2.0f * (xy + wz);
        result.at(1, 1) = 1.0f - 2.0f * (xx + zz);
        result.at(1, 2) = 2.0f * (yz - wx);

        result.at(2, 0) = 2.0f * (xz - wy);
        result.at(2, 1) = 2.0f * (yz + wx);
        result.at(2, 2) = 1.0f - 2.0f * (xx + yy);

        return result;
    }

    // Normalize
    void normalize() {
        float len = std::sqrt(x*x + y*y + z*z + w*w);
        if (len > EPSILON) {
            float inv_len = 1.0f / len;
            x *= inv_len;
            y *= inv_len;
            z *= inv_len;
            w *= inv_len;
        }
    }

    // Rotate a vector by this quaternion
    Vec3 rotate(const Vec3& v) const {
        // v' = q * v * q_conjugate
        // Optimized formula: v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        Vec3 u(x, y, z);
        float s = w;

        return v + Vec3::cross(u, Vec3::cross(u, v) + v * s) * 2.0f;
    }
};

} // namespace maya::math
