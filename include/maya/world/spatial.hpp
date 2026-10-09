#pragma once

#include "maya/world/components.hpp"
#include <optional>

namespace maya {
// Dimensionless tolerances for orthogonality, rigid poses and TRS reconstruction.
inline constexpr float spatial_tolerance = 1.0e-5f;
inline constexpr double spatial_singularity_tolerance = 1.0e-8;
// Rotations this close to unit length are kept bit-exact, so revalidation (e.g. save/load) is
// idempotent. Float-normalized quaternions deviate by well under 1e-7.
inline constexpr double quaternion_unit_tolerance = 4.0e-7;

/// Validates finite TRS and normalizes any finite, nonzero quaternion.
std::optional<TransformComponent> validated_transform(TransformComponent value) noexcept;
/// The transform as a pose: rotation and scale in float, the translation in double (#1065).
math::Affine local_pose(const TransformComponent& value) noexcept;
/// The pose `t` of the way from `a` to `b` (0 to 1): translation (in double) and scale linearly,
/// rotation along the shortest arc (the rotations are normalized first).
TransformComponent interpolate_transform(const TransformComponent& a, const TransformComponent& b, float t) noexcept;

// Float affine matrices: local frames, offsets, and poses relative to a nearby origin.
/// Rejects singular, numerically degenerate, or unrepresentable inverses.
std::optional<math::Mat4> inverse_affine(const math::Mat4& value) noexcept;
/// Positive-scale TRS only: shear/reflection is rejected, not silently discarded.
std::optional<TransformComponent> decompose_transform(const math::Mat4& value) noexcept;
std::optional<math::Mat4> compose_affine(const math::Mat4& parent,
                                      const math::Mat4& local) noexcept;

// World poses (#1065): the same, with the translation kept in double.
std::optional<math::Affine> inverse_pose(const math::Affine& value) noexcept;
std::optional<math::Affine> compose_pose(const math::Affine& parent, const math::Affine& local) noexcept;
std::optional<TransformComponent> decompose_transform(const math::Affine& value) noexcept;
bool unit_scale(const math::Vec3& scale) noexcept;

/// A camera's matrices, camera-relative (#1065): the view has no translation, so everything drawn is
/// placed relative to `origin`, the camera's world position (`Affine::relative_to(origin)`).
struct CameraMatrices {
    math::Mat4 view;
    math::Mat4 projection;
    math::Mat4 view_projection;
    math::DVec3 origin;
};
/// Pure calculation: rigid right-handed pose, radians, and depth range [0,1].
std::optional<CameraMatrices> camera_matrices(const CameraComponent& camera,
                                            const math::Affine& pose, float aspect) noexcept;
} // namespace maya
