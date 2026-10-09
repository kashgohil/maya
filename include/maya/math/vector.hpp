#pragma once

#include "maya/math/math_utils.hpp"
#include <cmath>
#include <iostream> // For debugging/printing

namespace maya::math {

// -----------------------------------------------------------------------------
// Vec2
// -----------------------------------------------------------------------------
struct Vec2 {
    float x, y;

    constexpr Vec2() : x(0.0f), y(0.0f) {}
    constexpr Vec2(float scalar) : x(scalar), y(scalar) {}
    constexpr Vec2(float x, float y) : x(x), y(y) {}

    // Arithmetic
    Vec2 operator+(const Vec2& other) const { return {x + other.x, y + other.y}; }
    Vec2 operator-(const Vec2& other) const { return {x - other.x, y - other.y}; }
    Vec2 operator*(float scalar) const { return {x * scalar, y * scalar}; }
    Vec2 operator/(float scalar) const { return {x / scalar, y / scalar}; }

    Vec2& operator+=(const Vec2& other) { x += other.x; y += other.y; return *this; }
    Vec2& operator-=(const Vec2& other) { x -= other.x; y -= other.y; return *this; }
    Vec2& operator*=(float scalar) { x *= scalar; y *= scalar; return *this; }

    // Geometry
    float length_squared() const { return x * x + y * y; }
    float length() const { return std::sqrt(length_squared()); }

    void normalize() {
        float len = length();
        if (len > EPSILON) {
            *this = *this / len;
        }
    }

    Vec2 normalized() const {
        Vec2 res = *this;
        res.normalize();
        return res;
    }

    static float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
};

// -----------------------------------------------------------------------------
// Vec3
// -----------------------------------------------------------------------------
struct Vec3 {
    float x, y, z;

    constexpr Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    constexpr Vec3(float scalar) : x(scalar), y(scalar), z(scalar) {}
    constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    constexpr Vec3(const Vec2& xy, float z) : x(xy.x), y(xy.y), z(z) {}

    // Arithmetic
    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    Vec3 operator/(float scalar) const { return {x / scalar, y / scalar, z / scalar}; }

    Vec3 operator-() const { return {-x, -y, -z}; }

    Vec3& operator+=(const Vec3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    Vec3& operator-=(const Vec3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }
    Vec3& operator*=(float scalar) { x *= scalar; y *= scalar; z *= scalar; return *this; }

    // Geometry
    float length_squared() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(length_squared()); }

    void normalize() {
        float len = length();
        if (len > EPSILON) {
            *this = *this / len;
        }
    }

    Vec3 normalized() const {
        Vec3 res = *this;
        res.normalize();
        return res;
    }

    static float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
};

// -----------------------------------------------------------------------------
// DVec3
// -----------------------------------------------------------------------------
/// A position in metres in double precision (#1065): world translations, which reach kilometres from the
/// origin, where a float's spacing is a millimetre. An offset between two positions is small, so it is a
/// float Vec3 again (`(a - b).to_float()`). A Vec3 widens to a DVec3 exactly; narrowing is explicit.
struct DVec3 {
    double x, y, z;

    constexpr DVec3() : x(0.0), y(0.0), z(0.0) {}
    constexpr DVec3(double x, double y, double z) : x(x), y(y), z(z) {}
    constexpr DVec3(const Vec3& v) : x(v.x), y(v.y), z(v.z) {} // NOLINT: widening is exact

    DVec3 operator+(const DVec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    DVec3 operator-(const DVec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    DVec3 operator*(double scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    DVec3 operator/(double scalar) const { return {x / scalar, y / scalar, z / scalar}; }
    DVec3 operator-() const { return {-x, -y, -z}; }
    DVec3& operator+=(const DVec3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    DVec3& operator-=(const DVec3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }
    bool operator==(const DVec3&) const = default;

    double length_squared() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(length_squared()); }
    /// Narrowed to float: for offsets and values near the origin.
    Vec3 to_float() const { return {float(x), float(y), float(z)}; }
};

// -----------------------------------------------------------------------------
// Vec4
// -----------------------------------------------------------------------------
struct Vec4 {
    float x, y, z, w;

    constexpr Vec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    constexpr Vec4(float scalar) : x(scalar), y(scalar), z(scalar), w(scalar) {}
    constexpr Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    constexpr Vec4(const Vec3& xyz, float w) : x(xyz.x), y(xyz.y), z(xyz.z), w(w) {}

    // Arithmetic
    Vec4 operator+(const Vec4& other) const { return {x + other.x, y + other.y, z + other.z, w + other.w}; }
    Vec4 operator-(const Vec4& other) const { return {x - other.x, y - other.y, z - other.z, w - other.w}; }
    Vec4 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar, w * scalar}; }
    Vec4 operator/(float scalar) const { return {x / scalar, y / scalar, z / scalar, w / scalar}; }

    static float dot(const Vec4& a, const Vec4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
};

} // namespace maya::math
