#include "maya/math/matrix.hpp"
#include <cmath>
#include <cstring>

namespace maya {

Mat4::Mat4() { std::memset(m, 0, sizeof(m)); }

Mat4::Mat4(f32 diagonal) : Mat4() {
  m[0] = diagonal;
  m[5] = diagonal;
  m[10] = diagonal;
  m[15] = diagonal;
}

f32 &Mat4::operator()(u32 row, u32 col) { return m[col * 4 + row]; }

f32 Mat4::operator()(u32 row, u32 col) const { return m[col * 4 + row]; }

Mat4 Mat4::operator*(const Mat4 &other) const {
  Mat4 result;
  for (u32 col = 0; col < 4; ++col) {
    for (u32 row = 0; row < 4; ++row) {
      f32 sum = 0.0f;
      for (u32 i = 0; i < 4; ++i) {
        sum += (*this)(row, i) * other(i, col);
      }
      result(row, col) = sum;
    }
  }
  return result;
}

Vec4 Mat4::operator*(const Vec4 &vec) const {
  return Vec4((*this)(0, 0) * vec.x + (*this)(0, 1) * vec.y +
                  (*this)(0, 2) * vec.z + (*this)(0, 3) * vec.w,
              (*this)(1, 0) * vec.x + (*this)(1, 1) * vec.y +
                  (*this)(1, 2) * vec.z + (*this)(1, 3) * vec.w,
              (*this)(2, 0) * vec.x + (*this)(2, 1) * vec.y +
                  (*this)(2, 2) * vec.z + (*this)(2, 3) * vec.w,
              (*this)(3, 0) * vec.x + (*this)(3, 1) * vec.y +
                  (*this)(3, 2) * vec.z + (*this)(3, 3) * vec.w);
}

Mat4 &Mat4::operator*=(const Mat4 &other) {
  *this = *this * other;
  return *this;
}

Mat4 Mat4::transposed() const {
  Mat4 result;
  for (u32 row = 0; row < 4; ++row) {
    for (u32 col = 0; col < 4; ++col) {
      result(col, row) = (*this)(row, col);
    }
  }
  return result;
}

void Mat4::transpose() { *this = transposed(); }

Mat4 Mat4::identity() { return Mat4(1.0f); }

Mat4 Mat4::translate(const Vec3 &translation) {
  Mat4 result = identity();
  result(0, 3) = translation.x;
  result(1, 3) = translation.y;
  result(2, 3) = translation.z;
  return result;
}

Mat4 Mat4::scale(const Vec3 &scale) {
  Mat4 result = identity();
  result(0, 0) = scale.x;
  result(1, 1) = scale.y;
  result(2, 2) = scale.z;
  return result;
}

Mat4 Mat4::rotate(f32 angle, const Vec3 &axis) {
  f32 c = std::cos(angle);
  f32 s = std::sin(angle);
  f32 one_minus_c = 1.0f - c;

  Vec3 a = axis.normalized();
  f32 x = a.x;
  f32 y = a.y;
  f32 z = a.z;

  Mat4 result;
  result(0, 0) = c + x * x * one_minus_c;
  result(0, 1) = x * y * one_minus_c - z * s;
  result(0, 2) = x * z * one_minus_c + y * s;
  result(0, 3) = 0.0f;

  result(1, 0) = y * x * one_minus_c + z * s;
  result(1, 1) = c + y * y * one_minus_c;
  result(1, 2) = y * z * one_minus_c - x * s;
  result(1, 3) = 0.0f;

  result(2, 0) = z * x * one_minus_c - y * s;
  result(2, 1) = z * y * one_minus_c + x * s;
  result(2, 2) = c + z * z * one_minus_c;
  result(2, 3) = 0.0f;

  result(3, 0) = 0.0f;
  result(3, 1) = 0.0f;
  result(3, 2) = 0.0f;
  result(3, 3) = 1.0f;

  return result;
}

Mat4 Mat4::perspective(f32 fov, f32 aspect, f32 near, f32 far) {
  f32 tan_half_fov = std::tan(fov * 0.5f);

  Mat4 result;
  result(0, 0) = 1.0f / (aspect * tan_half_fov);
  result(1, 1) = 1.0f / tan_half_fov;
  result(2, 2) = -(far + near) / (far - near);
  result(2, 3) = -(2.0f * far * near) / (far - near);
  result(3, 2) = -1.0f;
  result(3, 3) = 0.0f;

  return result;
}

Mat4 Mat4::orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near,
                        f32 far) {
  Mat4 result = identity();
  result(0, 0) = 2.0f / (right - left);
  result(1, 1) = 2.0f / (top - bottom);
  result(2, 2) = -2.0f / (far - near);
  result(0, 3) = -(right + left) / (right - left);
  result(1, 3) = -(top + bottom) / (top - bottom);
  result(2, 3) = -(far + near) / (far - near);
  return result;
}

Mat4 Mat4::look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up) {
  Vec3 f = (target - eye).normalized();
  Vec3 s = Vec3::cross(f, up).normalized();
  Vec3 u = Vec3::cross(s, f);

  Mat4 result = identity();
  result(0, 0) = s.x;
  result(0, 1) = s.y;
  result(0, 2) = s.z;
  result(1, 0) = u.x;
  result(1, 1) = u.y;
  result(1, 2) = u.z;
  result(2, 0) = -f.x;
  result(2, 1) = -f.y;
  result(2, 2) = -f.z;
  result(0, 3) = -Vec3::dot(s, eye);
  result(1, 3) = -Vec3::dot(u, eye);
  result(2, 3) = Vec3::dot(f, eye);

  return result;
}

} // namespace maya
