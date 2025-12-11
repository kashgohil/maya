#pragma once

#include "maya/math/types.hpp"

namespace maya {

struct Vec2 {
  f32 x, y;

  Vec2() : x(0.0f), y(0.0f) {}
  Vec2(f32 x, f32 y) : x(x), y(y) {}

  Vec2 operator+(const Vec2 &other) const;
  Vec2 operator-(const Vec2 &other) const;
  Vec2 operator*(f32 scalar) const;
  Vec2 operator/(f32 scalar) const;

  Vec2 &operator+=(const Vec2 &other);
  Vec2 &operator-=(const Vec2 &other);
  Vec2 &operator*=(f32 scalar);
  Vec2 &operator/=(f32 scalar);

  f32 length() const;
  f32 length_squared() const;
  Vec2 normalized() const;
  void normalize();

  static f32 dot(const Vec2 &a, const Vec2 &b);
  static f32 distance(const Vec2 &a, const Vec2 &b);
};

struct Vec3 {
  f32 x, y, z;

  Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
  Vec3(f32 x, f32 y, f32 z) : x(x), y(y), z(z) {}
  Vec3(f32 value) : x(value), y(value), z(value) {}

  Vec3 operator+(const Vec3 &other) const;
  Vec3 operator-(const Vec3 &other) const;
  Vec3 operator*(f32 scalar) const;
  Vec3 operator/(f32 scalar) const;

  Vec3 &operator+=(const Vec3 &other);
  Vec3 &operator-=(const Vec3 &other);
  Vec3 &operator*=(f32 scalar);
  Vec3 &operator/=(f32 scalar);

  f32 length() const;
  f32 length_squared() const;
  Vec3 normalized() const;
  void normalize();

  static f32 dot(const Vec3 &a, const Vec3 &b);
  static Vec3 cross(const Vec3 &a, const Vec3 &b);
  static f32 distance(const Vec3 &a, const Vec3 &b);
};

struct Vec4 {
  f32 x, y, z, w;

  Vec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
  Vec4(f32 x, f32 y, f32 z, f32 w) : x(x), y(y), z(z), w(w) {}
  Vec4(const Vec3 &xyz, f32 w) : x(xyz.x), y(xyz.y), z(xyz.z), w(w) {}

  Vec3 xyz() const { return Vec3(x, y, z); }

  Vec4 operator+(const Vec4 &other) const;
  Vec4 operator-(const Vec4 &other) const;
  Vec4 operator*(f32 scalar) const;
  Vec4 operator/(f32 scalar) const;

  Vec4 &operator+=(const Vec4 &other);
  Vec4 &operator-=(const Vec4 &other);
  Vec4 &operator*=(f32 scalar);
  Vec4 &operator/=(f32 scalar);

  f32 length() const;
  f32 length_squared() const;
  Vec4 normalized() const;
  void normalize();

  static f32 dot(const Vec4 &a, const Vec4 &b);
};

} // namespace maya
