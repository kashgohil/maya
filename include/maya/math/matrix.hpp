#pragma once

#include "maya/math/vector.hpp"
#include <cstring> // for memset

namespace maya::math {

struct Mat4 {
    // Column-major storage (like OpenGL/Metal/Vulkan)
    // elements[col][row]
    float elements[16];

    Mat4() {
        std::memset(elements, 0, 16 * sizeof(float));
    }

    explicit Mat4(float diagonal) {
        std::memset(elements, 0, 16 * sizeof(float));
        elements[0 + 0 * 4] = diagonal;
        elements[1 + 1 * 4] = diagonal;
        elements[2 + 2 * 4] = diagonal;
        elements[3 + 3 * 4] = diagonal;
    }

    static Mat4 identity() {
        return Mat4(1.0f);
    }

    // Accessors
    float& at(int row, int col) { return elements[row + col * 4]; }
    const float& at(int row, int col) const { return elements[row + col * 4]; }

    // Operations
    Mat4 operator*(const Mat4& other) const {
        Mat4 result;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int e = 0; e < 4; ++e) {
                    sum += at(row, e) * other.at(e, col);
                }
                result.at(row, col) = sum;
            }
        }
        return result;
    }

    Vec4 operator*(const Vec4& v) const {
        return Vec4(
            at(0,0)*v.x + at(0,1)*v.y + at(0,2)*v.z + at(0,3)*v.w,
            at(1,0)*v.x + at(1,1)*v.y + at(1,2)*v.z + at(1,3)*v.w,
            at(2,0)*v.x + at(2,1)*v.y + at(2,2)*v.z + at(2,3)*v.w,
            at(3,0)*v.x + at(3,1)*v.y + at(3,2)*v.z + at(3,3)*v.w
        );
    }

    // Transformations
    static Mat4 translate(const Vec3& translation) {
        Mat4 result(1.0f);
        result.at(0, 3) = translation.x;
        result.at(1, 3) = translation.y;
        result.at(2, 3) = translation.z;
        return result;
    }

    static Mat4 scale(const Vec3& scale) {
        Mat4 result(1.0f);
        result.at(0, 0) = scale.x;
        result.at(1, 1) = scale.y;
        result.at(2, 2) = scale.z;
        return result;
    }

    static Mat4 rotate_x(float angle_radians) {
        Mat4 result(1.0f);
        float c = std::cos(angle_radians);
        float s = std::sin(angle_radians);
        result.at(1, 1) = c;
        result.at(1, 2) = -s;
        result.at(2, 1) = s;
        result.at(2, 2) = c;
        return result;
    }

    static Mat4 rotate_y(float angle_radians) {
        Mat4 result(1.0f);
        float c = std::cos(angle_radians);
        float s = std::sin(angle_radians);
        result.at(0, 0) = c;
        result.at(0, 2) = s;
        result.at(2, 0) = -s;
        result.at(2, 2) = c;
        return result;
    }

    static Mat4 rotate_z(float angle_radians) {
        Mat4 result(1.0f);
        float c = std::cos(angle_radians);
        float s = std::sin(angle_radians);
        result.at(0, 0) = c;
        result.at(0, 1) = -s;
        result.at(1, 0) = s;
        result.at(1, 1) = c;
        return result;
    }

    // Camera Matrices
    static Mat4 perspective(float fov, float aspect, float near, float far) {
        Mat4 result(0.0f);
        float tan_half_fov = std::tan(fov * 0.5f);

        result.at(0, 0) = 1.0f / (aspect * tan_half_fov);
        result.at(1, 1) = 1.0f / tan_half_fov;
        result.at(2, 2) = -(far + near) / (far - near);
        result.at(2, 3) = -(2.0f * far * near) / (far - near);
        result.at(3, 2) = -1.0f;

        return result;
    }

    static Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = (center - eye).normalized(); // forward
        Vec3 s = Vec3::cross(f, up).normalized(); // right
        Vec3 u = Vec3::cross(s, f); // up

        Mat4 result(1.0f);
        result.at(0, 0) = s.x;
        result.at(0, 1) = s.y;
        result.at(0, 2) = s.z;
        result.at(0, 3) = -Vec3::dot(s, eye);

        result.at(1, 0) = u.x;
        result.at(1, 1) = u.y;
        result.at(1, 2) = u.z;
        result.at(1, 3) = -Vec3::dot(u, eye);

        result.at(2, 0) = -f.x;
        result.at(2, 1) = -f.y;
        result.at(2, 2) = -f.z;
        result.at(2, 3) = Vec3::dot(f, eye);

        return result;
    }
};

} // namespace maya::math
