#pragma once

#include "maya/math/matrix.hpp"
#include "maya/math/types.hpp"
#include "maya/math/vector.hpp"

namespace maya {

struct Quat {
  f32 x, y, z, w;

  Quat() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
  Quat(f32 x, f32 y, f32 z, f32 w) : x(x), y(y), z(z), w(w) {}

  Quat operator*(const Quat &other) const;
  Quat &operator*=(const Quat &other);

  f32 length() const;
  f32 length_squared() const;
  Quat normalized() const;
  void normalize();

  Quat conjugate() const;
  Quat inverse() const;

  Vec3 rotate(const Vec3 &vec) const;
  Mat4 to_matrix() const;

  static Quat from_axis_angle(const Vec3 &axis, f32 angle);
  static Quat from_euler(f32 pitch, f32 yaw, f32 roll);
  static f32 dot(const Quat &a, const Quat &b);
  static Quat slerp(const Quat &a, const Quat &b, f32 t);
};

} // namespace maya
