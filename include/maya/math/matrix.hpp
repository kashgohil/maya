#pragma once

#include "maya/math/types.hpp"
#include "maya/math/vector.hpp"

namespace maya {

struct Mat4 {
  f32 m[16]; // Column-major order (OpenGL/Vulkan style)

  Mat4();
  explicit Mat4(f32 diagonal);

  f32 &operator()(u32 row, u32 col);
  f32 operator()(u32 row, u32 col) const;

  Mat4 operator*(const Mat4 &other) const;
  Vec4 operator*(const Vec4 &vec) const;
  Mat4 &operator*=(const Mat4 &other);

  Mat4 transposed() const;
  void transpose();

  static Mat4 identity();
  static Mat4 translate(const Vec3 &translation);
  static Mat4 scale(const Vec3 &scale);
  static Mat4 rotate(f32 angle, const Vec3 &axis);
  static Mat4 perspective(f32 fov, f32 aspect, f32 near, f32 far);
  static Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near,
                           f32 far);
  static Mat4 look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up);
};

} // namespace maya
