#include "maya/math/quaternion.hpp"
#include <cmath>

namespace maya {

Quat Quat::operator*(const Quat &other) const {
  return Quat(w * other.x + x * other.w + y * other.z - z * other.y,
              w * other.y - x * other.z + y * other.w + z * other.x,
              w * other.z + x * other.y - y * other.x + z * other.w,
              w * other.w - x * other.x - y * other.y - z * other.z);
}

Quat &Quat::operator*=(const Quat &other) {
  *this = *this * other;
  return *this;
}

f32 Quat::length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

f32 Quat::length_squared() const { return x * x + y * y + z * z + w * w; }

Quat Quat::normalized() const {
  f32 len = length();
  if (len > 0.0f) {
    return Quat(x / len, y / len, z / len, w / len);
  }
  return Quat();
}

void Quat::normalize() { *this = normalized(); }

Quat Quat::conjugate() const { return Quat(-x, -y, -z, w); }

Quat Quat::inverse() const {
  f32 len_sq = length_squared();
  if (len_sq > 0.0f) {
    Quat conj = conjugate();
    return Quat(conj.x / len_sq, conj.y / len_sq, conj.z / len_sq,
                conj.w / len_sq);
  }
  return Quat();
}

Vec3 Quat::rotate(const Vec3 &vec) const {
  Quat vec_quat(vec.x, vec.y, vec.z, 0.0f);
  Quat result = (*this) * vec_quat * conjugate();
  return Vec3(result.x, result.y, result.z);
}

Mat4 Quat::to_matrix() const {
  Mat4 result;

  f32 xx = x * x;
  f32 yy = y * y;
  f32 zz = z * z;
  f32 xy = x * y;
  f32 xz = x * z;
  f32 yz = y * z;
  f32 wx = w * x;
  f32 wy = w * y;
  f32 wz = w * z;

  result(0, 0) = 1.0f - 2.0f * (yy + zz);
  result(0, 1) = 2.0f * (xy - wz);
  result(0, 2) = 2.0f * (xz + wy);
  result(0, 3) = 0.0f;

  result(1, 0) = 2.0f * (xy + wz);
  result(1, 1) = 1.0f - 2.0f * (xx + zz);
  result(1, 2) = 2.0f * (yz - wx);
  result(1, 3) = 0.0f;

  result(2, 0) = 2.0f * (xz - wy);
  result(2, 1) = 2.0f * (yz + wx);
  result(2, 2) = 1.0f - 2.0f * (xx + yy);
  result(2, 3) = 0.0f;

  result(3, 0) = 0.0f;
  result(3, 1) = 0.0f;
  result(3, 2) = 0.0f;
  result(3, 3) = 1.0f;

  return result;
}

Quat Quat::from_axis_angle(const Vec3 &axis, f32 angle) {
  f32 half_angle = angle * 0.5f;
  f32 s = std::sin(half_angle);
  Vec3 a = axis.normalized();
  return Quat(a.x * s, a.y * s, a.z * s, std::cos(half_angle));
}

Quat Quat::from_euler(f32 pitch, f32 yaw, f32 roll) {
  f32 cy = std::cos(yaw * 0.5f);
  f32 sy = std::sin(yaw * 0.5f);
  f32 cp = std::cos(pitch * 0.5f);
  f32 sp = std::sin(pitch * 0.5f);
  f32 cr = std::cos(roll * 0.5f);
  f32 sr = std::sin(roll * 0.5f);

  return Quat(sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
              cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy);
}

f32 Quat::dot(const Quat &a, const Quat &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quat Quat::slerp(const Quat &a, const Quat &b, f32 t) {
  f32 cos_theta = dot(a, b);

  Quat b_corrected = b;
  if (cos_theta < 0.0f) {
    b_corrected = Quat(-b.x, -b.y, -b.z, -b.w);
    cos_theta = -cos_theta;
  }

  if (cos_theta > 0.9995f) {
    return Quat(
               a.x + t * (b_corrected.x - a.x), a.y + t * (b_corrected.y - a.y),
               a.z + t * (b_corrected.z - a.z), a.w + t * (b_corrected.w - a.w))
        .normalized();
  }

  f32 theta = std::acos(cos_theta);
  f32 sin_theta = std::sin(theta);
  f32 weight_a = std::sin((1.0f - t) * theta) / sin_theta;
  f32 weight_b = std::sin(t * theta) / sin_theta;

  return Quat(a.x * weight_a + b_corrected.x * weight_b,
              a.y * weight_a + b_corrected.y * weight_b,
              a.z * weight_a + b_corrected.z * weight_b,
              a.w * weight_a + b_corrected.w * weight_b);
}

} // namespace maya
