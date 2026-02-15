#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/math/matrix.hpp"
#include "maya/math/vector.hpp"

using namespace maya::math;

// =============================================================================
// Mat4 Construction Tests
// =============================================================================
TEST_CASE("Mat4 construction", "[math][matrix]") {
    SECTION("Default constructor zeros matrix") {
        Mat4 m;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                CHECK(m.at(i, j) == 0.0f);
            }
        }
    }

    SECTION("Diagonal constructor") {
        Mat4 m(5.0f);
        CHECK(m.at(0, 0) == 5.0f);
        CHECK(m.at(1, 1) == 5.0f);
        CHECK(m.at(2, 2) == 5.0f);
        CHECK(m.at(3, 3) == 5.0f);
        CHECK(m.at(0, 1) == 0.0f);
        CHECK(m.at(1, 0) == 0.0f);
    }

    SECTION("Identity matrix") {
        Mat4 id = Mat4::identity();
        CHECK(id.at(0, 0) == 1.0f);
        CHECK(id.at(1, 1) == 1.0f);
        CHECK(id.at(2, 2) == 1.0f);
        CHECK(id.at(3, 3) == 1.0f);
        
        // Off-diagonal should be zero
        CHECK(id.at(0, 1) == 0.0f);
        CHECK(id.at(0, 2) == 0.0f);
        CHECK(id.at(1, 0) == 0.0f);
        CHECK(id.at(2, 0) == 0.0f);
    }
}

// =============================================================================
// Mat4 Transformation Tests - Scale
// =============================================================================
TEST_CASE("Mat4 scale transformation", "[math][matrix][transform]") {
    SECTION("Uniform scale") {
        Mat4 s = Mat4::scale(Vec3(2.0f, 2.0f, 2.0f));
        Vec4 point(1.0f, 1.0f, 1.0f, 1.0f);
        Vec4 result = s * point;
        
        CHECK(result.x == 2.0f);
        CHECK(result.y == 2.0f);
        CHECK(result.z == 2.0f);
        CHECK(result.w == 1.0f);
    }

    SECTION("Non-uniform scale") {
        Mat4 s = Mat4::scale(Vec3(2.0f, 3.0f, 4.0f));
        Vec4 point(1.0f, 1.0f, 1.0f, 1.0f);
        Vec4 result = s * point;
        
        CHECK(result.x == 2.0f);
        CHECK(result.y == 3.0f);
        CHECK(result.z == 4.0f);
        CHECK(result.w == 1.0f);
    }

    SECTION("Scale negative values") {
        Mat4 s = Mat4::scale(Vec3(-1.0f, 1.0f, 1.0f));
        Vec4 point(1.0f, 2.0f, 3.0f, 1.0f);
        Vec4 result = s * point;
        
        CHECK(result.x == -1.0f);
        CHECK(result.y == 2.0f);
        CHECK(result.z == 3.0f);
    }

    SECTION("Scale zero") {
        Mat4 s = Mat4::scale(Vec3(0.0f, 1.0f, 1.0f));
        Vec4 point(5.0f, 5.0f, 5.0f, 1.0f);
        Vec4 result = s * point;
        
        CHECK(result.x == 0.0f);
        CHECK(result.y == 5.0f);
        CHECK(result.z == 5.0f);
    }
}

// =============================================================================
// Mat4 Transformation Tests - Rotation
// =============================================================================
TEST_CASE("Mat4 rotation X", "[math][matrix][transform]") {
    SECTION("90 degree rotation around X") {
        float angle = to_radians(90.0f);
        Mat4 rot = Mat4::rotate_x(angle);
        
        Vec4 y_axis(0.0f, 1.0f, 0.0f, 1.0f);
        Vec4 result = rot * y_axis;
        
        // Y axis rotated 90 around X should point to +Z (right-hand rule)
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("180 degree rotation around X") {
        float angle = to_radians(180.0f);
        Mat4 rot = Mat4::rotate_x(angle);
        
        Vec4 y_axis(0.0f, 1.0f, 0.0f, 1.0f);
        Vec4 result = rot * y_axis;
        
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("0 degree rotation is identity") {
        Mat4 rot = Mat4::rotate_x(0.0f);
        Vec4 point(1.0f, 2.0f, 3.0f, 1.0f);
        Vec4 result = rot * point;
        
        CHECK(result.x == 1.0f);
        CHECK(result.y == 2.0f);
        CHECK(result.z == 3.0f);
    }
}

TEST_CASE("Mat4 rotation Y", "[math][matrix][transform]") {
    SECTION("90 degree rotation around Y") {
        float angle = to_radians(90.0f);
        Mat4 rot = Mat4::rotate_y(angle);
        
        Vec4 x_axis(1.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = rot * x_axis;
        
        // X axis rotated 90 around Y should point to -Z (right-hand rule)
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
    }

    SECTION("180 degree rotation around Y") {
        float angle = to_radians(180.0f);
        Mat4 rot = Mat4::rotate_y(angle);
        
        Vec4 x_axis(1.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = rot * x_axis;
        
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }
}

TEST_CASE("Mat4 rotation Z", "[math][matrix][transform]") {
    SECTION("90 degree rotation around Z") {
        float angle = to_radians(90.0f);
        Mat4 rot = Mat4::rotate_z(angle);
        
        Vec4 x_axis(1.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = rot * x_axis;
        
        // X axis rotated 90 around Z should point to Y
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("180 degree rotation around Z") {
        float angle = to_radians(180.0f);
        Mat4 rot = Mat4::rotate_z(angle);
        
        Vec4 x_axis(1.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = rot * x_axis;
        
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }
}

TEST_CASE("Mat4 combined rotations", "[math][matrix][transform]") {
    SECTION("Rotation order matters") {
        // Rotate 90 around X then 90 around Y
        Mat4 rot_x = Mat4::rotate_x(to_radians(90.0f));
        Mat4 rot_y = Mat4::rotate_y(to_radians(90.0f));
        
        Mat4 combined1 = rot_y * rot_x; // Y then X
        Mat4 combined2 = rot_x * rot_y; // X then Y
        
        Vec4 point(1.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result1 = combined1 * point;
        Vec4 result2 = combined2 * point;
        
        // Results should be different (matrix multiplication is not commutative)
        bool same = (result1.x == result2.x && result1.y == result2.y && result1.z == result2.z);
        CHECK(!same);
    }
}

// =============================================================================
// Mat4 Transformation Tests - Translation
// =============================================================================
TEST_CASE("Mat4 translation", "[math][matrix][transform]") {
    SECTION("Basic translation") {
        Mat4 t = Mat4::translate(Vec3(10.0f, -5.0f, 3.0f));
        Vec4 point(0.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = t * point;
        
        CHECK(result.x == 10.0f);
        CHECK(result.y == -5.0f);
        CHECK(result.z == 3.0f);
        CHECK(result.w == 1.0f);
    }

    SECTION("Translation doesn't affect vectors (w=0)") {
        Mat4 t = Mat4::translate(Vec3(10.0f, 10.0f, 10.0f));
        Vec4 vector(1.0f, 2.0f, 3.0f, 0.0f);
        Vec4 result = t * vector;
        
        // Vectors shouldn't be translated
        CHECK(result.x == 1.0f);
        CHECK(result.y == 2.0f);
        CHECK(result.z == 3.0f);
        CHECK(result.w == 0.0f);
    }
}

// =============================================================================
// Mat4 Combined Transformations
// =============================================================================
TEST_CASE("Mat4 combined transformations", "[math][matrix][transform]") {
    SECTION("TRS order (Translate * Rotate * Scale)") {
        Mat4 t = Mat4::translate(Vec3(1.0f, 0.0f, 0.0f));
        Mat4 r = Mat4::rotate_y(to_radians(90.0f));
        Mat4 s = Mat4::scale(Vec3(2.0f, 2.0f, 2.0f));
        
        // Apply S, then R, then T
        Mat4 trs = t * r * s;
        
        Vec4 point(1.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = trs * point;
        
        // 1. Scale: (1,0,0) -> (2,0,0)
        // 2. Rotate Y 90: (2,0,0) -> (0,0,-2) (right-hand rule)
        // 3. Translate: (0,0,-2) -> (1,0,-2)
        CHECK_THAT(result.x, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
        CHECK_THAT(result.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(result.z, Catch::Matchers::WithinAbs(-2.0f, 0.0001f));
    }

    SECTION("Identity transformation") {
        Mat4 id = Mat4::identity();
        Vec4 point(1.0f, 2.0f, 3.0f, 1.0f);
        Vec4 result = id * point;
        
        CHECK(result.x == 1.0f);
        CHECK(result.y == 2.0f);
        CHECK(result.z == 3.0f);
        CHECK(result.w == 1.0f);
    }
}

// =============================================================================
// Mat4 Matrix Multiplication
// =============================================================================
TEST_CASE("Mat4 multiplication", "[math][matrix]") {
    SECTION("Multiplication with identity") {
        Mat4 a = Mat4::translate(Vec3(5.0f, 0.0f, 0.0f));
        Mat4 id = Mat4::identity();
        
        Mat4 result = a * id;
        
        // Should be equal to original
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                CHECK(result.at(i, j) == a.at(i, j));
            }
        }
    }

    SECTION("Multiplication is associative") {
        Mat4 a = Mat4::translate(Vec3(1.0f, 0.0f, 0.0f));
        Mat4 b = Mat4::rotate_y(to_radians(90.0f));
        Mat4 c = Mat4::scale(Vec3(2.0f, 2.0f, 2.0f));
        
        Mat4 ab_c = (a * b) * c;
        Mat4 a_bc = a * (b * c);
        
        // Should be equal (associative)
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                CHECK_THAT(ab_c.at(i, j), Catch::Matchers::WithinAbs(a_bc.at(i, j), 0.0001f));
            }
        }
    }
}

// =============================================================================
// Mat4 Camera Matrices
// =============================================================================
TEST_CASE("Mat4 perspective projection", "[math][matrix][camera]") {
    SECTION("Standard perspective") {
        float fov = to_radians(60.0f);
        float aspect = 16.0f / 9.0f;
        float near = 0.1f;
        float far = 100.0f;
        Mat4 p = Mat4::perspective(fov, aspect, near, far);

        // Near plane maps to 0
        Vec4 point_near(0.0f, 0.0f, -near, 1.0f);
        Vec4 res_near = p * point_near;
        float depth_near = res_near.z / res_near.w;
        CHECK_THAT(depth_near, Catch::Matchers::WithinAbs(0.0f, 0.0001f));

        // Far plane maps to 1
        Vec4 point_far(0.0f, 0.0f, -far, 1.0f);
        Vec4 res_far = p * point_far;
        float depth_far = res_far.z / res_far.w;
        CHECK_THAT(depth_far, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("Extreme FOV") {
        // Very narrow FOV
        Mat4 narrow = Mat4::perspective(to_radians(1.0f), 1.0f, 0.1f, 100.0f);
        CHECK(narrow.at(0, 0) > 50.0f); // Large value for narrow FOV
        
        // Very wide FOV
        Mat4 wide = Mat4::perspective(to_radians(120.0f), 1.0f, 0.1f, 100.0f);
        CHECK(wide.at(0, 0) < 1.0f); // Small value for wide FOV
    }
}

TEST_CASE("Mat4 look_at", "[math][matrix][camera]") {
    SECTION("Standard look at") {
        Vec3 eye(0.0f, 0.0f, 3.0f);
        Vec3 target(0.0f, 0.0f, 0.0f);
        Vec3 up(0.0f, 1.0f, 0.0f);
        Mat4 view = Mat4::look_at(eye, target, up);

        // Transforming eye by view matrix should put it at origin
        Vec4 res = view * Vec4(eye.x, eye.y, eye.z, 1.0f);
        CHECK_THAT(res.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(res.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(res.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("Look at from different angles") {
        Vec3 eye(5.0f, 5.0f, 5.0f);
        Vec3 target(0.0f, 0.0f, 0.0f);
        Vec3 up(0.0f, 1.0f, 0.0f);
        Mat4 view = Mat4::look_at(eye, target, up);

        // Target should be along -Z in view space
        Vec4 target_view = view * Vec4(target.x, target.y, target.z, 1.0f);
        CHECK(target_view.z < 0.0f);
    }

    SECTION("Degenerate case - eye equals target") {
        Vec3 eye(0.0f, 0.0f, 0.0f);
        Vec3 target(0.0f, 0.0f, 0.0f);
        Vec3 up(0.0f, 1.0f, 0.0f);
        
        // Should not crash, though result is undefined
        Mat4 view = Mat4::look_at(eye, target, up);
        // Just verify it doesn't crash - result may be garbage
        CHECK(true);
    }
}

// =============================================================================
// Mat4 Edge Cases
// =============================================================================
TEST_CASE("Mat4 edge cases", "[math][matrix]") {
    SECTION("Zero matrix multiplication") {
        Mat4 zero;
        Mat4 id = Mat4::identity();
        
        Mat4 result = zero * id;
        
        // Result should be zero
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                CHECK(result.at(i, j) == 0.0f);
            }
        }
    }

    SECTION("Transform origin") {
        Mat4 t = Mat4::translate(Vec3(5.0f, 10.0f, 15.0f));
        Vec4 origin(0.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = t * origin;
        
        CHECK(result.x == 5.0f);
        CHECK(result.y == 10.0f);
        CHECK(result.z == 15.0f);
    }
}
