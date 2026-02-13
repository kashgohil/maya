#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/math/vector.hpp"
#include "maya/math/matrix.hpp"
#include "maya/math/quaternion.hpp"

using namespace maya::math;

TEST_CASE("Vector3 basic operations", "[math][vector]") {
    Vec3 a(1.0f, 2.0f, 3.0f);
    Vec3 b(4.0f, 5.0f, 6.0f);

    SECTION("Addition") {
        Vec3 res = a + b;
        CHECK(res.x == 5.0f);
        CHECK(res.y == 7.0f);
        CHECK(res.z == 9.0f);
    }

    SECTION("Subtraction") {
        Vec3 res = b - a;
        CHECK(res.x == 3.0f);
        CHECK(res.y == 3.0f);
        CHECK(res.z == 3.0f);
    }

    SECTION("Dot Product") {
        float dot = Vec3::dot(a, b);
        CHECK(dot == (1*4 + 2*5 + 3*6));
    }

    SECTION("Normalization") {
        Vec3 v(5.0f, 0.0f, 0.0f);
        Vec3 n = v.normalized();
        CHECK(n.x == 1.0f);
        CHECK(n.y == 0.0f);
        CHECK(n.z == 0.0f);
        CHECK_THAT(n.length(), Catch::Matchers::WithinRel(1.0f, 0.0001f));
    }
}

TEST_CASE("Matrix4 basic operations", "[math][matrix]") {
    SECTION("Identity") {
        Mat4 id = Mat4::identity();
        CHECK(id.at(0, 0) == 1.0f);
        CHECK(id.at(1, 1) == 1.0f);
        CHECK(id.at(2, 2) == 1.0f);
        CHECK(id.at(3, 3) == 1.0f);
        CHECK(id.at(0, 1) == 0.0f);
    }

    SECTION("Translation") {
        Vec3 pos(10.0f, -5.0f, 2.0f);
        Mat4 t = Mat4::translate(pos);
        
        Vec4 point(0.0f, 0.0f, 0.0f, 1.0f);
        Vec4 result = t * point;
        
        CHECK(result.x == 10.0f);
        CHECK(result.y == -5.0f);
        CHECK(result.z == 2.0f);
        CHECK(result.w == 1.0f);
    }

    SECTION("Multiplication") {
        Mat4 a = Mat4::translate(Vec3(1, 0, 0));
        Mat4 b = Mat4::translate(Vec3(0, 1, 0));
        Mat4 res = a * b; // Should be translate(1, 1, 0)

        Vec4 point(0, 0, 0, 1);
        Vec4 final_point = res * point;
        
        CHECK(final_point.x == 1.0f);
        CHECK(final_point.y == 1.0f);
        CHECK(final_point.z == 0.0f);
    }

    SECTION("Perspective Projection") {
        float fov = to_radians(60.0f);
        float aspect = 16.0f / 9.0f;
        float near = 0.1f;
        float far = 100.0f;
        Mat4 p = Mat4::perspective(fov, aspect, near, far);

        // A point on the near plane should have Z = -1 (or 0 depending on convention, 
        // Maya/Metal uses 0 to 1 for depth)
        Vec4 point_near(0, 0, -near, 1);
        Vec4 res_near = p * point_near;
        
        // After perspective divide, Z should be 0.0 for Metal
        float depth = res_near.z / res_near.w;
        CHECK_THAT(depth, Catch::Matchers::WithinAbs(0.0f, 0.0001f));

        // A point on the far plane should have Z = 1.0
        Vec4 point_far(0, 0, -far, 1);
        Vec4 res_far = p * point_far;
        float depth_far = res_far.z / res_far.w;
        CHECK_THAT(depth_far, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("Look At") {
        Vec3 eye(0, 0, 3);
        Vec3 target(0, 0, 0);
        Vec3 up(0, 1, 0);
        Mat4 v = Mat4::look_at(eye, target, up);

        // Transforming the eye position by the view matrix should put it at (0, 0, 0)
        Vec4 res = v * Vec4(eye.x, eye.y, eye.z, 1.0f);
        CHECK_THAT(res.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(res.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(res.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }
}

TEST_CASE("Quaternion operations", "[math][quaternion]") {
    SECTION("Identity rotation") {
        Quat q = Quat::identity();
        Vec3 v(1, 2, 3);
        Vec3 rotated = q.rotate(v);
        
        CHECK(rotated.x == v.x);
        CHECK(rotated.y == v.y);
        CHECK(rotated.z == v.z);
    }

    SECTION("90 degree Y rotation") {
        // Rotate (1, 0, 0) 90 degrees around Y axis -> should be (0, 0, -1)
        Quat q = Quat::from_axis_angle(Vec3(0, 1, 0), to_radians(90.0f));
        Vec3 v(1, 0, 0);
        Vec3 rotated = q.rotate(v);
        
        CHECK_THAT(rotated.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(rotated.z, Catch::Matchers::WithinAbs(-1.0f, 0.0001f));
    }
}
