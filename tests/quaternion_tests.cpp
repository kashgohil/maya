#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/math/quaternion.hpp"
#include "maya/math/vector.hpp"
#include "maya/math/matrix.hpp"

using namespace maya::math;

// =============================================================================
// Quaternion Construction Tests
// =============================================================================
TEST_CASE("Quaternion construction", "[math][quaternion]") {
    SECTION("Default constructor is identity") {
        Quat q;
        CHECK(q.x == 0.0f);
        CHECK(q.y == 0.0f);
        CHECK(q.z == 0.0f);
        CHECK(q.w == 1.0f);
    }

    SECTION("Explicit constructor") {
        Quat q(0.5f, 0.5f, 0.5f, 0.5f);
        CHECK(q.x == 0.5f);
        CHECK(q.y == 0.5f);
        CHECK(q.z == 0.5f);
        CHECK(q.w == 0.5f);
    }

    SECTION("Identity factory method") {
        Quat q = Quat::identity();
        CHECK(q.x == 0.0f);
        CHECK(q.y == 0.0f);
        CHECK(q.z == 0.0f);
        CHECK(q.w == 1.0f);
    }
}

// =============================================================================
// Quaternion from Axis-Angle Tests
// =============================================================================
TEST_CASE("Quaternion from_axis_angle", "[math][quaternion]") {
    SECTION("90 degree rotation around X axis") {
        Quat q = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(90.0f));
        
        // For 90 degree rotation around X:
        // w = cos(45) = ~0.707
        // x = sin(45) = ~0.707
        // y = z = 0
        CHECK_THAT(q.w, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
        CHECK_THAT(q.x, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
        CHECK_THAT(q.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("90 degree rotation around Y axis") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        
        CHECK_THAT(q.w, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
        CHECK_THAT(q.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.y, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
        CHECK_THAT(q.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("90 degree rotation around Z axis") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 0.0f, 1.0f), to_radians(90.0f));
        
        CHECK_THAT(q.w, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
        CHECK_THAT(q.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.z, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
    }

    SECTION("180 degree rotation") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(180.0f));
        
        // For 180 degrees: cos(90) = 0, sin(90) = 1
        CHECK_THAT(q.w, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.y, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(q.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("0 degree rotation is identity") {
        Quat q = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), 0.0f);
        CHECK_THAT(q.w, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(q.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(q.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("360 degree rotation is identity") {
        Quat q = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(360.0f));
        // 360° rotation: w = cos(180°) = -1, but (-1, 0, 0, 0) represents same rotation as (1, 0, 0, 0)
        // Accept either w = 1 or w = -1 since both represent identity rotation
        bool is_identity_w = (std::abs(q.w - 1.0f) < 0.0001f) || (std::abs(q.w + 1.0f) < 0.0001f);
        CHECK(is_identity_w);
        CHECK_THAT(q.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("Arbitrary axis rotation") {
        Vec3 axis(1.0f, 1.0f, 1.0f);
        axis.normalize();
        Quat q = Quat::from_axis_angle(axis, to_radians(90.0f));
        
        // Should be unit quaternion (approximately)
        float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        CHECK_THAT(len, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }
}

// =============================================================================
// Quaternion Rotation Tests
// =============================================================================
TEST_CASE("Quaternion rotate vector", "[math][quaternion]") {
    SECTION("Identity rotation") {
        Quat q = Quat::identity();
        Vec3 v(1.0f, 2.0f, 3.0f);
        Vec3 rotated = q.rotate(v);
        
        CHECK(rotated.x == v.x);
        CHECK(rotated.y == v.y);
        CHECK(rotated.z == v.z);
    }

    SECTION("90 degree Y rotation") {
        // Rotate (1, 0, 0) 90 degrees around Y -> (0, 0, -1)
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec3 rotated = q.rotate(v);
        
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.z, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
    }

    SECTION("90 degree X rotation") {
        // Rotate (0, 1, 0) 90 degrees around X -> (0, 0, 1)
        Quat q = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(90.0f));
        Vec3 v(0.0f, 1.0f, 0.0f);
        Vec3 rotated = q.rotate(v);
        
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.z, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("90 degree Z rotation") {
        // Rotate (1, 0, 0) 90 degrees around Z -> (0, 1, 0)
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 0.0f, 1.0f), to_radians(90.0f));
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec3 rotated = q.rotate(v);
        
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.y, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(rotated.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("180 degree rotation") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(180.0f));
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec3 rotated = q.rotate(v);
        
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
        CHECK_THAT(rotated.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("Rotate zero vector") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        Vec3 v(0.0f, 0.0f, 0.0f);
        Vec3 rotated = q.rotate(v);
        
        CHECK(rotated.x == 0.0f);
        CHECK(rotated.y == 0.0f);
        CHECK(rotated.z == 0.0f);
    }

    SECTION("Preserves vector length") {
        Vec3 axis(1.0f, 1.0f, 1.0f);
        axis.normalize(); // Must normalize axis for proper quaternion
        Quat q = Quat::from_axis_angle(axis, to_radians(45.0f));
        Vec3 v(3.0f, 4.0f, 5.0f);
        Vec3 rotated = q.rotate(v);
        
        float original_len = v.length();
        float rotated_len = rotated.length();
        CHECK_THAT(rotated_len, Catch::Matchers::WithinRel(original_len, 0.01f));
    }
}

// =============================================================================
// Quaternion Composition Tests
// =============================================================================
TEST_CASE("Quaternion multiplication", "[math][quaternion]") {
    SECTION("Identity multiplication") {
        Quat id = Quat::identity();
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        
        Quat result = id * q;
        CHECK(result.x == q.x);
        CHECK(result.y == q.y);
        CHECK(result.z == q.z);
        CHECK(result.w == q.w);
    }

    SECTION("Composition order matters") {
        // Rotate 90 around X then 90 around Y
        Quat rot_x = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(90.0f));
        Quat rot_y = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        
        Quat y_then_x = rot_x * rot_y;
        Quat x_then_y = rot_y * rot_x;
        
        // Different orders should give different results
        bool same = (y_then_x.x == x_then_y.x && y_then_x.y == x_then_y.y && 
                     y_then_x.z == x_then_y.z && y_then_x.w == x_then_y.w);
        CHECK(!same);
    }

    SECTION("90 degree X followed by 90 degree X = 180 degree X") {
        Quat rot90 = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(90.0f));
        Quat rot180 = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(180.0f));
        
        Quat combined = rot90 * rot90;
        
        CHECK_THAT(combined.x, Catch::Matchers::WithinAbs(rot180.x, 0.0001f));
        CHECK_THAT(combined.y, Catch::Matchers::WithinAbs(rot180.y, 0.0001f));
        CHECK_THAT(combined.z, Catch::Matchers::WithinAbs(rot180.z, 0.0001f));
        CHECK_THAT(combined.w, Catch::Matchers::WithinAbs(rot180.w, 0.0001f));
    }

    SECTION("Composition rotates correctly") {
        // First rotate 90 around Y, then 90 around X
        Quat rot_y = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        Quat rot_x = Quat::from_axis_angle(Vec3(1.0f, 0.0f, 0.0f), to_radians(90.0f));
        
        Quat combined = rot_x * rot_y; // Apply Y then X
        
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec3 rotated = combined.rotate(v);
        
        // After Y rotation: (0, 0, -1)
        // After X rotation: (0, 1, 0)
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.y, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(rotated.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }
}

// =============================================================================
// Quaternion to Matrix Conversion Tests
// =============================================================================
TEST_CASE("Quaternion to_mat4", "[math][quaternion]") {
    SECTION("Identity quaternion to identity matrix") {
        Quat q = Quat::identity();
        Mat4 m = q.to_mat4();
        
        CHECK(m.at(0, 0) == 1.0f);
        CHECK(m.at(1, 1) == 1.0f);
        CHECK(m.at(2, 2) == 1.0f);
        CHECK(m.at(3, 3) == 1.0f);
        CHECK(m.at(0, 1) == 0.0f);
        CHECK(m.at(1, 0) == 0.0f);
    }

    SECTION("90 degree Y rotation matches matrix") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        Mat4 mat_from_quat = q.to_mat4();
        Mat4 mat_direct = Mat4::rotate_y(to_radians(90.0f));
        
        // Compare matrices (only rotation part)
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                CHECK_THAT(mat_from_quat.at(i, j), 
                    Catch::Matchers::WithinAbs(mat_direct.at(i, j), 0.0001f));
            }
        }
    }

    SECTION("Matrix transforms same as quaternion rotation") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        Mat4 m = q.to_mat4();
        
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec4 v4(v.x, v.y, v.z, 1.0f);
        
        Vec3 quat_rotated = q.rotate(v);
        Vec4 mat_result = m * v4;
        
        CHECK_THAT(quat_rotated.x, Catch::Matchers::WithinAbs(mat_result.x, 0.0001f));
        CHECK_THAT(quat_rotated.y, Catch::Matchers::WithinAbs(mat_result.y, 0.0001f));
        CHECK_THAT(quat_rotated.z, Catch::Matchers::WithinAbs(mat_result.z, 0.0001f));
    }
}

// =============================================================================
// Quaternion Normalization Tests
// =============================================================================
TEST_CASE("Quaternion normalize", "[math][quaternion]") {
    SECTION("Already unit quaternion") {
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        float len_before = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        
        q.normalize();
        float len_after = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        
        CHECK_THAT(len_before, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(len_after, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("Normalize scaled quaternion") {
        Quat q(10.0f, 0.0f, 0.0f, 10.0f); // Non-unit quaternion
        q.normalize();
        
        float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        CHECK_THAT(len, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("Normalize zero quaternion") {
        Quat q(0.0f, 0.0f, 0.0f, 0.0f);
        q.normalize(); // Should not crash
        
        // Zero quaternion stays zero (no division)
        CHECK(q.x == 0.0f);
        CHECK(q.y == 0.0f);
        CHECK(q.z == 0.0f);
        CHECK(q.w == 0.0f);
    }

    SECTION("Normalize near-zero quaternion") {
        Quat q(EPSILON / 10.0f, 0.0f, 0.0f, 0.0f);
        q.normalize(); // Should handle gracefully
        
        // Should remain approximately unchanged
        CHECK(q.x < 1.0f);
    }
}

// =============================================================================
// Quaternion Edge Cases
// =============================================================================
TEST_CASE("Quaternion edge cases", "[math][quaternion]") {
    SECTION("Rotation around zero axis") {
        // Axis with zero length should still work (though mathematically undefined)
        Vec3 zero_axis(0.0f, 0.0f, 0.0f);
        Quat q = Quat::from_axis_angle(zero_axis, to_radians(90.0f));
        
        // Result should be all zeros except w = cos(45)
        CHECK_THAT(q.w, Catch::Matchers::WithinAbs(0.70710678f, 0.0001f));
        CHECK(q.x == 0.0f);
        CHECK(q.y == 0.0f);
        CHECK(q.z == 0.0f);
    }

    SECTION("Negative angle rotation") {
        Quat q_neg = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(-90.0f));
        Quat q_pos = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), to_radians(90.0f));
        
        // Negative angle should be inverse rotation
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec3 rotated_neg = q_neg.rotate(v);
        Vec3 rotated_pos = q_pos.rotate(v);
        
        // Should be opposite directions (check with tolerance for -0.0 vs 0.0)
        CHECK_THAT(rotated_neg.x, Catch::Matchers::WithinAbs(-rotated_pos.x, 0.0001f));
    }

    SECTION("Very small angle rotation") {
        float small_angle = 0.001f; // ~0.057 degrees
        Quat q = Quat::from_axis_angle(Vec3(0.0f, 1.0f, 0.0f), small_angle);
        
        // For small angles, w should be close to 1
        CHECK(q.w > 0.999f);
        
        Vec3 v(1.0f, 0.0f, 0.0f);
        Vec3 rotated = q.rotate(v);
        
        // Vector should be almost unchanged
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(1.0f, 0.01f));
    }
}
