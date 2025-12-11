#include "maya/math/vector.hpp"
#include <cmath>

namespace maya {

// Vec2 implementation
Vec2 Vec2::operator+(const Vec2 &other) const {
  return Vec2(x + other.x, y + other.y);
}

Vec2 Vec2::operator-(const Vec2 &other) const {
  return Vec2(x - other.x, y - other.y);
}

Vec2 Vec2::operator*(f32 scalar) const { return Vec2(x * scalar, y * scalar); }

Vec2 Vec2::operator/(f32 scalar) const { return Vec2(x / scalar, y / scalar); }

Vec2 &Vec2::operator+=(const Vec2 &other) {
  x += other.x;
  y += other.y;
  return *this;
}

Vec2 &Vec2::operator-=(const Vec2 &other) {
  x -= other.x;
  y -= other.y;
  return *this;
}

Vec2 &Vec2::operator*=(f32 scalar) {
  x *= scalar;
  y *= scalar;
  return *this;
}

Vec2 &Vec2::operator/=(f32 scalar) {
  x /= scalar;
  y /= scalar;
  return *this;
}

f32 Vec2::length() const { return std::sqrt(x * x + y * y); }

f32 Vec2::length_squared() const { return x * x + y * y; }

Vec2 Vec2::normalized() const {
  f32 len = length();
  if (len > 0.0f) {
    return *this / len;
  }
  return Vec2(0.0f, 0.0f);
}

void Vec2::normalize() { *this = normalized(); }

f32 Vec2::dot(const Vec2 &a, const Vec2 &b) { return a.x * b.x + a.y * b.y; }

f32 Vec2::distance(const Vec2 &a, const Vec2 &b) { return (b - a).length(); }

// Vec3 implementation
Vec3 Vec3::operator+(const Vec3 &other) const {
  return Vec3(x + other.x, y + other.y, z + other.z);
}

Vec3 Vec3::operator-(const Vec3 &other) const {
  return Vec3(x - other.x, y - other.y, z - other.z);
}

Vec3 Vec3::operator*(f32 scalar) const {
  return Vec3(x * scalar, y * scalar, z * scalar);
}

Vec3 Vec3::operator/(f32 scalar) const {
  return Vec3(x / scalar, y / scalar, z / scalar);
}

Vec3 &Vec3::operator+=(const Vec3 &other) {
  x += other.x;
  y += other.y;
  z += other.z;
  return *this;
}

Vec3 &Vec3::operator-=(const Vec3 &other) {
  x -= other.x;
  y -= other.y;
  z -= other.z;
  return *this;
}

Vec3 &Vec3::operator*=(f32 scalar) {
  x *= scalar;
  y *= scalar;
  z *= scalar;
  return *this;
}

Vec3 &Vec3::operator/=(f32 scalar) {
  x /= scalar;
  y /= scalar;
  z /= scalar;
  return *this;
}

f32 Vec3::length() const { return std::sqrt(x * x + y * y + z * z); }

f32 Vec3::length_squared() const { return x * x + y * y + z * z; }

Vec3 Vec3::normalized() const {
  f32 len = length();
  if (len > 0.0f) {
    return *this / len;
  }
  return Vec3(0.0f, 0.0f, 0.0f);
}

void Vec3::normalize() { *this = normalized(); }

f32 Vec3::dot(const Vec3 &a, const Vec3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Vec3::cross(const Vec3 &a, const Vec3 &b) {
  return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

f32 Vec3::distance(const Vec3 &a, const Vec3 &b) { return (b - a).length(); }

// Vec4 implementation
Vec4 Vec4::operator+(const Vec4 &other) const {
  return Vec4(x + other.x, y + other.y, z + other.z, w + other.w);
}

Vec4 Vec4::operator-(const Vec4 &other) const {
  return Vec4(x - other.x, y - other.y, z - other.z, w - other.w);
}

Vec4 Vec4::operator*(f32 scalar) const {
  return Vec4(x * scalar, y * scalar, z * scalar, w * scalar);
}

Vec4 Vec4::operator/(f32 scalar) const {
  return Vec4(x / scalar, y / scalar, z / scalar, w / scalar);
}

Vec4 &Vec4::operator+=(const Vec4 &other) {
  x += other.x;
  y += other.y;
  z += other.z;
  w += other.w;
  return *this;
}

Vec4 &Vec4::operator-=(const Vec4 &other) {
  x -= other.x;
  y -= other.y;
  z -= other.z;
  w -= other.w;
  return *this;
}

Vec4 &Vec4::operator*=(f32 scalar) {
  x *= scalar;
  y *= scalar;
  z *= scalar;
  w *= scalar;
  return *this;
}

Vec4 &Vec4::operator/=(f32 scalar) {
  x /= scalar;
  y /= scalar;
  z /= scalar;
  w /= scalar;
  return *this;
}

f32 Vec4::length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

f32 Vec4::length_squared() const { return x * x + y * y + z * z + w * w; }

Vec4 Vec4::normalized() const {
  f32 len = length();
  if (len > 0.0f) {
    return *this / len;
  }
  return Vec4(0.0f, 0.0f, 0.0f, 0.0f);
}

void Vec4::normalize() { *this = normalized(); }

f32 Vec4::dot(const Vec4 &a, const Vec4 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

} // namespace maya
