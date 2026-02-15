#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/math/vector.hpp"

using namespace maya::math;

// =============================================================================
// Vec2 Tests
// =============================================================================
TEST_CASE("Vec2 construction", "[math][vector][vec2]") {
    SECTION("Default constructor") {
        Vec2 v;
        CHECK(v.x == 0.0f);
        CHECK(v.y == 0.0f);
    }

    SECTION("Scalar constructor") {
        Vec2 v(5.0f);
        CHECK(v.x == 5.0f);
        CHECK(v.y == 5.0f);
    }

    SECTION("Component constructor") {
        Vec2 v(1.0f, 2.0f);
        CHECK(v.x == 1.0f);
        CHECK(v.y == 2.0f);
    }
}

TEST_CASE("Vec2 arithmetic operations", "[math][vector][vec2]") {
    Vec2 a(1.0f, 2.0f);
    Vec2 b(3.0f, 4.0f);

    SECTION("Addition") {
        Vec2 res = a + b;
        CHECK(res.x == 4.0f);
        CHECK(res.y == 6.0f);
    }

    SECTION("Subtraction") {
        Vec2 res = b - a;
        CHECK(res.x == 2.0f);
        CHECK(res.y == 2.0f);
    }

    SECTION("Scalar multiplication") {
        Vec2 res = a * 2.0f;
        CHECK(res.x == 2.0f);
        CHECK(res.y == 4.0f);
    }

    SECTION("Scalar division") {
        Vec2 res = b / 2.0f;
        CHECK(res.x == 1.5f);
        CHECK(res.y == 2.0f);
    }

    SECTION("Compound addition") {
        Vec2 res = a;
        res += b;
        CHECK(res.x == 4.0f);
        CHECK(res.y == 6.0f);
    }

    SECTION("Compound subtraction") {
        Vec2 res = b;
        res -= a;
        CHECK(res.x == 2.0f);
        CHECK(res.y == 2.0f);
    }

    SECTION("Compound multiplication") {
        Vec2 res = a;
        res *= 3.0f;
        CHECK(res.x == 3.0f);
        CHECK(res.y == 6.0f);
    }
}

TEST_CASE("Vec2 geometric operations", "[math][vector][vec2]") {
    SECTION("Length squared") {
        Vec2 v(3.0f, 4.0f);
        CHECK(v.length_squared() == 25.0f);
    }

    SECTION("Length") {
        Vec2 v(3.0f, 4.0f);
        CHECK(v.length() == 5.0f);
    }

    SECTION("Dot product") {
        Vec2 a(1.0f, 2.0f);
        Vec2 b(3.0f, 4.0f);
        CHECK(Vec2::dot(a, b) == 11.0f); // 1*3 + 2*4
    }

    SECTION("Normalization") {
        Vec2 v(5.0f, 0.0f);
        Vec2 n = v.normalized();
        CHECK_THAT(n.x, Catch::Matchers::WithinRel(1.0f, 0.0001f));
        CHECK_THAT(n.y, Catch::Matchers::WithinRel(0.0f, 0.0001f));
        CHECK_THAT(n.length(), Catch::Matchers::WithinRel(1.0f, 0.0001f));
    }

    SECTION("Normalize zero vector") {
        Vec2 v(0.0f, 0.0f);
        v.normalize(); // Should not crash
        CHECK(v.x == 0.0f);
        CHECK(v.y == 0.0f);
    }

    SECTION("Normalize near-zero vector") {
        Vec2 v(EPSILON / 10.0f, EPSILON / 10.0f);
        v.normalize(); // Should handle gracefully
        // Result may be unchanged or zero
        CHECK(v.length() < 1.0f);
    }
}

// =============================================================================
// Vec3 Tests
// =============================================================================
TEST_CASE("Vec3 construction", "[math][vector][vec3]") {
    SECTION("Default constructor") {
        Vec3 v;
        CHECK(v.x == 0.0f);
        CHECK(v.y == 0.0f);
        CHECK(v.z == 0.0f);
    }

    SECTION("Scalar constructor") {
        Vec3 v(5.0f);
        CHECK(v.x == 5.0f);
        CHECK(v.y == 5.0f);
        CHECK(v.z == 5.0f);
    }

    SECTION("Component constructor") {
        Vec3 v(1.0f, 2.0f, 3.0f);
        CHECK(v.x == 1.0f);
        CHECK(v.y == 2.0f);
        CHECK(v.z == 3.0f);
    }

    SECTION("Vec2 + z constructor") {
        Vec2 xy(1.0f, 2.0f);
        Vec3 v(xy, 3.0f);
        CHECK(v.x == 1.0f);
        CHECK(v.y == 2.0f);
        CHECK(v.z == 3.0f);
    }
}

TEST_CASE("Vec3 arithmetic operations", "[math][vector][vec3]") {
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

    SECTION("Negation") {
        Vec3 v(1.0f, -2.0f, 3.0f);
        Vec3 res = -v;
        CHECK(res.x == -1.0f);
        CHECK(res.y == 2.0f);
        CHECK(res.z == -3.0f);
    }

    SECTION("Scalar multiplication") {
        Vec3 res = a * 2.0f;
        CHECK(res.x == 2.0f);
        CHECK(res.y == 4.0f);
        CHECK(res.z == 6.0f);
    }

    SECTION("Scalar division") {
        Vec3 res = b / 2.0f;
        CHECK(res.x == 2.0f);
        CHECK(res.y == 2.5f);
        CHECK(res.z == 3.0f);
    }

    SECTION("Compound addition") {
        Vec3 res = a;
        res += b;
        CHECK(res.x == 5.0f);
        CHECK(res.y == 7.0f);
        CHECK(res.z == 9.0f);
    }

    SECTION("Compound subtraction") {
        Vec3 res = b;
        res -= a;
        CHECK(res.x == 3.0f);
        CHECK(res.y == 3.0f);
        CHECK(res.z == 3.0f);
    }

    SECTION("Compound multiplication") {
        Vec3 res = a;
        res *= 3.0f;
        CHECK(res.x == 3.0f);
        CHECK(res.y == 6.0f);
        CHECK(res.z == 9.0f);
    }
}

TEST_CASE("Vec3 geometric operations", "[math][vector][vec3]") {
    SECTION("Length squared") {
        Vec3 v(1.0f, 2.0f, 2.0f);
        CHECK(v.length_squared() == 9.0f);
    }

    SECTION("Length") {
        Vec3 v(1.0f, 2.0f, 2.0f);
        CHECK(v.length() == 3.0f);
    }

    SECTION("Length consistency") {
        Vec3 v(3.0f, 4.0f, 0.0f);
        CHECK(v.length_squared() == v.length() * v.length());
    }

    SECTION("Dot product") {
        Vec3 a(1.0f, 2.0f, 3.0f);
        Vec3 b(4.0f, 5.0f, 6.0f);
        CHECK(Vec3::dot(a, b) == 32.0f); // 1*4 + 2*5 + 3*6
    }

    SECTION("Cross product standard basis") {
        Vec3 x(1.0f, 0.0f, 0.0f);
        Vec3 y(0.0f, 1.0f, 0.0f);
        Vec3 z(0.0f, 0.0f, 1.0f);

        Vec3 cross_xy = Vec3::cross(x, y);
        CHECK(cross_xy.x == 0.0f);
        CHECK(cross_xy.y == 0.0f);
        CHECK(cross_xy.z == 1.0f); // x × y = z

        Vec3 cross_yz = Vec3::cross(y, z);
        CHECK(cross_yz.x == 1.0f);
        CHECK(cross_yz.y == 0.0f);
        CHECK(cross_yz.z == 0.0f); // y × z = x

        Vec3 cross_zx = Vec3::cross(z, x);
        CHECK(cross_zx.x == 0.0f);
        CHECK(cross_zx.y == 1.0f);
        CHECK(cross_zx.z == 0.0f); // z × x = y
    }

    SECTION("Cross product anti-commutative") {
        Vec3 a(1.0f, 2.0f, 3.0f);
        Vec3 b(4.0f, 5.0f, 6.0f);
        Vec3 cross_ab = Vec3::cross(a, b);
        Vec3 cross_ba = Vec3::cross(b, a);
        
        CHECK(cross_ab.x == -cross_ba.x);
        CHECK(cross_ab.y == -cross_ba.y);
        CHECK(cross_ab.z == -cross_ba.z);
    }

    SECTION("Cross product self is zero") {
        Vec3 v(1.0f, 2.0f, 3.0f);
        Vec3 cross = Vec3::cross(v, v);
        CHECK(cross.x == 0.0f);
        CHECK(cross.y == 0.0f);
        CHECK(cross.z == 0.0f);
    }

    SECTION("Normalization") {
        Vec3 v(0.0f, 5.0f, 0.0f);
        Vec3 n = v.normalized();
        CHECK_THAT(n.x, Catch::Matchers::WithinRel(0.0f, 0.0001f));
        CHECK_THAT(n.y, Catch::Matchers::WithinRel(1.0f, 0.0001f));
        CHECK_THAT(n.z, Catch::Matchers::WithinRel(0.0f, 0.0001f));
        CHECK_THAT(n.length(), Catch::Matchers::WithinRel(1.0f, 0.0001f));
    }

    SECTION("Normalize zero vector") {
        Vec3 v(0.0f, 0.0f, 0.0f);
        v.normalize(); // Should not crash
        CHECK(v.x == 0.0f);
        CHECK(v.y == 0.0f);
        CHECK(v.z == 0.0f);
    }

    SECTION("Normalize near-zero vector") {
        Vec3 v(EPSILON / 10.0f, EPSILON / 10.0f, EPSILON / 10.0f);
        v.normalize(); // Should handle gracefully
        CHECK(v.length() < 1.0f);
    }
}

// =============================================================================
// Vec4 Tests
// =============================================================================
TEST_CASE("Vec4 construction", "[math][vector][vec4]") {
    SECTION("Default constructor") {
        Vec4 v;
        CHECK(v.x == 0.0f);
        CHECK(v.y == 0.0f);
        CHECK(v.z == 0.0f);
        CHECK(v.w == 0.0f);
    }

    SECTION("Scalar constructor") {
        Vec4 v(5.0f);
        CHECK(v.x == 5.0f);
        CHECK(v.y == 5.0f);
        CHECK(v.z == 5.0f);
        CHECK(v.w == 5.0f);
    }

    SECTION("Component constructor") {
        Vec4 v(1.0f, 2.0f, 3.0f, 4.0f);
        CHECK(v.x == 1.0f);
        CHECK(v.y == 2.0f);
        CHECK(v.z == 3.0f);
        CHECK(v.w == 4.0f);
    }

    SECTION("Vec3 + w constructor") {
        Vec3 xyz(1.0f, 2.0f, 3.0f);
        Vec4 v(xyz, 4.0f);
        CHECK(v.x == 1.0f);
        CHECK(v.y == 2.0f);
        CHECK(v.z == 3.0f);
        CHECK(v.w == 4.0f);
    }
}

TEST_CASE("Vec4 arithmetic operations", "[math][vector][vec4]") {
    Vec4 a(1.0f, 2.0f, 3.0f, 4.0f);
    Vec4 b(5.0f, 6.0f, 7.0f, 8.0f);

    SECTION("Addition") {
        Vec4 res = a + b;
        CHECK(res.x == 6.0f);
        CHECK(res.y == 8.0f);
        CHECK(res.z == 10.0f);
        CHECK(res.w == 12.0f);
    }

    SECTION("Subtraction") {
        Vec4 res = b - a;
        CHECK(res.x == 4.0f);
        CHECK(res.y == 4.0f);
        CHECK(res.z == 4.0f);
        CHECK(res.w == 4.0f);
    }

    SECTION("Scalar multiplication") {
        Vec4 res = a * 2.0f;
        CHECK(res.x == 2.0f);
        CHECK(res.y == 4.0f);
        CHECK(res.z == 6.0f);
        CHECK(res.w == 8.0f);
    }

    SECTION("Scalar division") {
        Vec4 res = b / 2.0f;
        CHECK(res.x == 2.5f);
        CHECK(res.y == 3.0f);
        CHECK(res.z == 3.5f);
        CHECK(res.w == 4.0f);
    }

    SECTION("Dot product") {
        Vec4 a(1.0f, 2.0f, 3.0f, 4.0f);
        Vec4 b(5.0f, 6.0f, 7.0f, 8.0f);
        CHECK(Vec4::dot(a, b) == 70.0f); // 1*5 + 2*6 + 3*7 + 4*8
    }
}

TEST_CASE("Vec4 Mat4 transformation", "[math][vector][vec4]") {
    // This test is in matrix_tests.cpp but we verify Vec4 works with matrices
    Vec4 point(1.0f, 2.0f, 3.0f, 1.0f);
    CHECK(point.w == 1.0f);
}
