#pragma once

#include "maya/math/matrix.hpp"

namespace maya::math {

/// A world pose (#1065, docs/architecture/runtime-world-contracts.md#coordinates): rotation and scale as a
/// float 3×3 (in `linear`, whose translation column is zero) and the translation in double. Composing
/// and inverting poses keeps the translation in double, so positions kilometres from the origin keep
/// their precision. Rendering and other float consumers take a pose relative to an origin near them,
/// usually the camera (`relative_to`), never the absolute float matrix far from the origin.
struct Affine {
    Mat4 linear = Mat4::identity();
    DVec3 translation{};

    /// A float matrix's pose: its translation widened exactly.
    static Affine from_matrix(const Mat4& m) {
        auto result = Affine{m, {m.at(0, 3), m.at(1, 3), m.at(2, 3)}};
        result.linear.at(0, 3) = result.linear.at(1, 3) = result.linear.at(2, 3) = 0.0f;
        return result;
    }
    /// The pose as a float matrix in a frame whose origin is `origin`: the translation is subtracted in
    /// double before it is narrowed, so the result is precise near `origin`.
    Mat4 relative_to(const DVec3& origin) const {
        auto m = linear;
        const auto offset = (translation - origin).to_float();
        m.at(0, 3) = offset.x;
        m.at(1, 3) = offset.y;
        m.at(2, 3) = offset.z;
        return m;
    }
    /// The absolute float matrix: only for poses known to be near the origin (local poses, tests).
    Mat4 matrix() const { return relative_to({}); }
    /// A direction or offset in the pose's frame, carried into the parent frame (no translation).
    Vec3 vector(const Vec3& v) const {
        const auto& m = linear;
        return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z, m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
                m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
    }
    /// A point in the pose's frame, carried into the parent frame, in double.
    DVec3 point(const Vec3& v) const {
        const auto& m = linear;
        return {translation.x + (double(m.at(0, 0)) * v.x + double(m.at(0, 1)) * v.y + double(m.at(0, 2)) * v.z),
                translation.y + (double(m.at(1, 0)) * v.x + double(m.at(1, 1)) * v.y + double(m.at(1, 2)) * v.z),
                translation.z + (double(m.at(2, 0)) * v.x + double(m.at(2, 1)) * v.y + double(m.at(2, 2)) * v.z)};
    }
    /// The basis vector of column `c` (0 x, 1 y, 2 z): with scale, the pose's axis.
    Vec3 axis(int c) const { return {linear.at(0, c), linear.at(1, c), linear.at(2, c)}; }
    bool operator==(const Affine& other) const {
        for (int i = 0; i < 16; ++i)
            if (linear.elements[i] != other.linear.elements[i]) return false;
        return translation == other.translation;
    }
};

} // namespace maya::math
