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
math::Mat4 local_matrix(const TransformComponent& value) noexcept;
/// Rejects singular, numerically degenerate, or unrepresentable inverses.
std::optional<math::Mat4> inverse_affine(const math::Mat4& value) noexcept;
/// Positive-scale TRS only: shear/reflection is rejected, not silently discarded.
std::optional<TransformComponent> decompose_transform(const math::Mat4& value) noexcept;
std::optional<math::Mat4> compose_affine(const math::Mat4& parent,
                                      const math::Mat4& local) noexcept;
bool unit_scale(const math::Vec3& scale) noexcept;

struct CameraMatrices {
    math::Mat4 view;
    math::Mat4 projection;
    math::Mat4 view_projection;
};
/// Pure calculation: rigid right-handed pose, radians, and depth range [0,1].
std::optional<CameraMatrices> camera_matrices(const CameraComponent& camera,
                                            const math::Mat4& pose, float aspect) noexcept;
} // namespace maya
