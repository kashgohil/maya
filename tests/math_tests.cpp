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
