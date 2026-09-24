#include "maya/world/spatial.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace maya {
namespace {
bool finite(const math::Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool finite(const math::Mat4& m) noexcept {
    for (const auto v : m.elements) if (!std::isfinite(v)) return false;
    return true;
}
bool affine(const math::Mat4& m) noexcept {
    return finite(m) && m.at(3,0) == 0 && m.at(3,1) == 0 &&
        m.at(3,2) == 0 && m.at(3,3) == 1;
}
bool representable(double v) noexcept {
    return std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max();
}
} // namespace

math::Mat4 local_matrix(const TransformComponent& v) noexcept {
    // Scale the basis directly to avoid intermediate products for TRS.
    auto m = v.rotation.to_mat4();
    for (int r = 0; r < 3; ++r) {
        m.at(r,0) *= v.scale.x;
        m.at(r,1) *= v.scale.y;
        m.at(r,2) *= v.scale.z;
    }
    m.at(0,3) = v.translation.x;
    m.at(1,3) = v.translation.y;
    m.at(2,3) = v.translation.z;
    return m;
}

std::optional<TransformComponent> validated_transform(TransformComponent v) noexcept {
    if (!finite(v.translation) || !finite(v.scale) ||
        v.scale.x <= 0 || v.scale.y <= 0 || v.scale.z <= 0) return std::nullopt;
    const auto& q = v.rotation;
    const auto length = std::hypot(std::hypot(double(q.x), double(q.y)),
                                   std::hypot(double(q.z), double(q.w)));
    if (!std::isfinite(length) || length == 0) return std::nullopt;
    if (std::abs(length - 1.0) > quaternion_unit_tolerance)
        v.rotation = {float(q.x / length), float(q.y / length),
                      float(q.z / length), float(q.w / length)};
    if (!inverse_affine(local_matrix(v))) return std::nullopt;
    return v;
}

std::optional<math::Mat4> inverse_affine(const math::Mat4& m) noexcept {
    if (!affine(m)) return std::nullopt;
    // Double intermediates avoid overflow for finite float scales/translations.
    double a[3][3];
    double lengths[3];
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) a[r][c] = m.at(r,c);
        lengths[c] = std::hypot(a[0][c], a[1][c], a[2][c]);
    }
    const auto det = a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
                   -a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
                   +a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    const auto volume = lengths[0]*lengths[1]*lengths[2];
    if (volume == 0 || std::abs(det) <= spatial_singularity_tolerance * volume)
        return std::nullopt;
    auto result = math::Mat4::identity();
    for (int r = 0; r < 3; ++r) {
        double translation = 0;
        for (int c = 0; c < 3; ++c) {
            const auto value = (a[(c+1)%3][(r+1)%3]*a[(c+2)%3][(r+2)%3] -
                                a[(c+1)%3][(r+2)%3]*a[(c+2)%3][(r+1)%3]) / det;
            if (!representable(value)) return std::nullopt;
            result.at(r,c) = static_cast<float>(value);
            translation -= value * m.at(c,3);
        }
        if (!representable(translation)) return std::nullopt;
        result.at(r,3) = static_cast<float>(translation);
    }
    return result;
}

std::optional<math::Mat4> compose_affine(const math::Mat4& parent,
                                       const math::Mat4& local) noexcept {
    if (!affine(parent) || !affine(local)) return std::nullopt;
    auto result = math::Mat4::identity();
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) {
        double value = c == 3 ? parent.at(r,3) : 0;
        for (int k = 0; k < 3; ++k) value += double(parent.at(r,k))*local.at(k,c);
        if (!representable(value)) return std::nullopt;
        result.at(r,c) = static_cast<float>(value);
    }
    if (!inverse_affine(result)) return std::nullopt;
    return result;
}

std::optional<TransformComponent> decompose_transform(const math::Mat4& m) noexcept {
    if (!inverse_affine(m)) return std::nullopt;
    double basis[3][3];
    float scales[3];
    for (int c = 0; c < 3; ++c) {
        const auto length = std::hypot(double(m.at(0,c)), double(m.at(1,c)), double(m.at(2,c)));
        if (!representable(length) || length == 0) return std::nullopt;
        scales[c] = static_cast<float>(length);
        for (int r = 0; r < 3; ++r) basis[r][c] = m.at(r,c) / length;
    }
    for (int a = 0; a < 3; ++a) for (int b = a+1; b < 3; ++b) {
        double dot = 0;
        for (int r = 0; r < 3; ++r) dot += basis[r][a]*basis[r][b];
        if (std::abs(dot) > spatial_tolerance) return std::nullopt;
    }
    const auto det = basis[0][0]*(basis[1][1]*basis[2][2]-basis[1][2]*basis[2][1])
                   -basis[0][1]*(basis[1][0]*basis[2][2]-basis[1][2]*basis[2][0])
                   +basis[0][2]*(basis[1][0]*basis[2][1]-basis[1][1]*basis[2][0]);
    if (det <= 0) return std::nullopt;
    // Largest quaternion component avoids cancellation near half-turn rotations.
    const double candidates[] = {1+basis[0][0]-basis[1][1]-basis[2][2],
        1-basis[0][0]+basis[1][1]-basis[2][2],
        1-basis[0][0]-basis[1][1]+basis[2][2],
        1+basis[0][0]+basis[1][1]+basis[2][2]};
    const auto index = std::max_element(candidates, candidates+4) - candidates;
    double q[4]{};
    q[index] = std::sqrt(candidates[index]) * 0.5;
    const auto divisor = 4*q[index];
    if (index == 3) {
        q[0] = (basis[2][1]-basis[1][2])/divisor;
        q[1] = (basis[0][2]-basis[2][0])/divisor;
        q[2] = (basis[1][0]-basis[0][1])/divisor;
    } else {
        const auto j = (index+1)%3, k = (index+2)%3;
        q[j] = (basis[j][index]+basis[index][j])/divisor;
        q[k] = (basis[k][index]+basis[index][k])/divisor;
        q[3] = (basis[k][j]-basis[j][k])/divisor;
    }
    auto result = validated_transform({{m.at(0,3),m.at(1,3),m.at(2,3)},
        {float(q[0]),float(q[1]),float(q[2]),float(q[3])}, {scales[0],scales[1],scales[2]}});
    if (!result) return std::nullopt;
    const auto rebuilt = local_matrix(*result);
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r)
        if (std::abs(double(rebuilt.at(r,c))-m.at(r,c)) > spatial_tolerance*scales[c])
            return std::nullopt;
    return result;
}

bool unit_scale(const math::Vec3& v) noexcept {
    return finite(v) && std::abs(v.x-1) <= spatial_tolerance &&
        std::abs(v.y-1) <= spatial_tolerance && std::abs(v.z-1) <= spatial_tolerance;
}

std::optional<CameraMatrices> camera_matrices(const CameraComponent& camera,
                                            const math::Mat4& pose, float aspect) noexcept {
    if (!std::isfinite(camera.vertical_fov) || !std::isfinite(aspect) ||
        !std::isfinite(camera.near_clip) || !std::isfinite(camera.far_clip) ||
        camera.vertical_fov <= 0 || camera.vertical_fov >= math::PI || aspect <= 0 ||
        camera.near_clip <= 0 || camera.far_clip <= camera.near_clip) return std::nullopt;
    const auto trs = decompose_transform(pose);
    if (!trs || !unit_scale(trs->scale)) return std::nullopt;
    const auto view = inverse_affine(pose);
    if (!view) return std::nullopt;
    auto projection = math::Mat4{};
    const auto cot = 1.0 / std::tan(double(camera.vertical_fov)*0.5);
    const auto depth = double(camera.near_clip)-camera.far_clip;
    const double values[] = {cot/aspect, cot, camera.far_clip/depth,
                            double(camera.far_clip)*camera.near_clip/depth};
    for (auto value : values) if (!representable(value)) return std::nullopt;
    projection.at(0,0) = static_cast<float>(values[0]);
    projection.at(1,1) = static_cast<float>(values[1]);
    projection.at(2,2) = static_cast<float>(values[2]);
    projection.at(2,3) = static_cast<float>(values[3]);
    projection.at(3,2) = -1;
    if (projection.at(0,0) == 0 || projection.at(1,1) == 0 || projection.at(2,3) == 0)
        return std::nullopt;
    const auto combined = projection * *view;
    if (!finite(combined)) return std::nullopt;
    return CameraMatrices{*view, projection, combined};
}
} // namespace maya
