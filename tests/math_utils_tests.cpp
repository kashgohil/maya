#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/math/math_utils.hpp"

using namespace maya::math;

// =============================================================================
// Math Constants Tests
// =============================================================================
TEST_CASE("Math constants", "[math][utils]") {
    SECTION("PI is correct") {
        CHECK_THAT(PI, Catch::Matchers::WithinAbs(3.14159265359f, 0.00001f));
    }

    SECTION("TWO_PI is 2*PI") {
        CHECK(TWO_PI == 2.0f * PI);
    }

    SECTION("HALF_PI is PI/2") {
        CHECK(HALF_PI == PI / 2.0f);
    }

    SECTION("EPSILON is small positive number") {
        CHECK(EPSILON > 0.0f);
        CHECK(EPSILON < 0.001f);
    }
}

// =============================================================================
// Radians/Degrees Conversion Tests
// =============================================================================
TEST_CASE("to_radians conversion", "[math][utils]") {
    SECTION("0 degrees to radians") {
        CHECK(to_radians(0.0f) == 0.0f);
    }

    SECTION("90 degrees to radians") {
        CHECK_THAT(to_radians(90.0f), Catch::Matchers::WithinAbs(PI / 2.0f, 0.0001f));
    }

    SECTION("180 degrees to radians") {
        CHECK_THAT(to_radians(180.0f), Catch::Matchers::WithinAbs(PI, 0.0001f));
    }

    SECTION("360 degrees to radians") {
        CHECK_THAT(to_radians(360.0f), Catch::Matchers::WithinAbs(TWO_PI, 0.0001f));
    }

    SECTION("Negative degrees to radians") {
        CHECK_THAT(to_radians(-90.0f), Catch::Matchers::WithinAbs(-PI / 2.0f, 0.0001f));
    }

    SECTION("Arbitrary degrees to radians") {
        CHECK_THAT(to_radians(45.0f), Catch::Matchers::WithinAbs(PI / 4.0f, 0.0001f));
        CHECK_THAT(to_radians(60.0f), Catch::Matchers::WithinAbs(PI / 3.0f, 0.0001f));
    }
}

TEST_CASE("to_degrees conversion", "[math][utils]") {
    SECTION("0 radians to degrees") {
        CHECK(to_degrees(0.0f) == 0.0f);
    }

    SECTION("PI/2 radians to degrees") {
        CHECK_THAT(to_degrees(PI / 2.0f), Catch::Matchers::WithinAbs(90.0f, 0.0001f));
    }

    SECTION("PI radians to degrees") {
        CHECK_THAT(to_degrees(PI), Catch::Matchers::WithinAbs(180.0f, 0.0001f));
    }

    SECTION("2*PI radians to degrees") {
        CHECK_THAT(to_degrees(TWO_PI), Catch::Matchers::WithinAbs(360.0f, 0.0001f));
    }

    SECTION("Negative radians to degrees") {
        CHECK_THAT(to_degrees(-PI / 2.0f), Catch::Matchers::WithinAbs(-90.0f, 0.0001f));
    }

    SECTION("Arbitrary radians to degrees") {
        CHECK_THAT(to_degrees(PI / 4.0f), Catch::Matchers::WithinAbs(45.0f, 0.0001f));
        CHECK_THAT(to_degrees(PI / 3.0f), Catch::Matchers::WithinAbs(60.0f, 0.0001f));
    }
}

TEST_CASE("Degrees to radians roundtrip", "[math][utils]") {
    SECTION("Roundtrip conversion") {
        float original_degrees = 123.456f;
        float radians = to_radians(original_degrees);
        float back_to_degrees = to_degrees(radians);
        
        CHECK_THAT(back_to_degrees, Catch::Matchers::WithinRel(original_degrees, 0.0001f));
    }

    SECTION("Various angles roundtrip") {
        float angles[] = {0.0f, 45.0f, 90.0f, 180.0f, 270.0f, 360.0f, -45.0f};
        
        for (float angle : angles) {
            float radians = to_radians(angle);
            float back = to_degrees(radians);
            CHECK_THAT(back, Catch::Matchers::WithinAbs(angle, 0.0001f));
        }
    }
}

// =============================================================================
// Clamp Tests
// =============================================================================
TEST_CASE("clamp function", "[math][utils]") {
    SECTION("Value within range") {
        CHECK(clamp(5.0f, 0.0f, 10.0f) == 5.0f);
        CHECK(clamp(0.5f, 0.0f, 1.0f) == 0.5f);
        CHECK(clamp(-3.0f, -10.0f, 10.0f) == -3.0f);
    }

    SECTION("Value below minimum") {
        CHECK(clamp(-5.0f, 0.0f, 10.0f) == 0.0f);
        CHECK(clamp(-100.0f, -10.0f, 10.0f) == -10.0f);
    }

    SECTION("Value above maximum") {
        CHECK(clamp(15.0f, 0.0f, 10.0f) == 10.0f);
        CHECK(clamp(100.0f, -10.0f, 10.0f) == 10.0f);
    }

    SECTION("Value at boundaries") {
        CHECK(clamp(0.0f, 0.0f, 10.0f) == 0.0f);
        CHECK(clamp(10.0f, 0.0f, 10.0f) == 10.0f);
    }

    SECTION("Integer clamp") {
        CHECK(clamp(5, 0, 10) == 5);
        CHECK(clamp(-5, 0, 10) == 0);
        CHECK(clamp(15, 0, 10) == 10);
    }

    SECTION("Double clamp") {
        CHECK(clamp(5.5, 0.0, 10.0) == 5.5);
        CHECK(clamp(-5.5, 0.0, 10.0) == 0.0);
        CHECK(clamp(15.5, 0.0, 10.0) == 10.0);
    }

    SECTION("Negative range") {
        CHECK(clamp(-5.0f, -10.0f, -1.0f) == -5.0f);
        CHECK(clamp(-15.0f, -10.0f, -1.0f) == -10.0f);
        CHECK(clamp(0.0f, -10.0f, -1.0f) == -1.0f);
    }

    SECTION("Zero range (min == max)") {
        CHECK(clamp(5.0f, 10.0f, 10.0f) == 10.0f);
        CHECK(clamp(15.0f, 10.0f, 10.0f) == 10.0f);
        CHECK(clamp(5.0f, 10.0f, 10.0f) == 10.0f);
    }
}

// =============================================================================
// Lerp Tests
// =============================================================================
TEST_CASE("lerp function", "[math][utils]") {
    SECTION("Lerp at t=0") {
        CHECK(lerp(0.0f, 10.0f, 0.0f) == 0.0f);
        CHECK(lerp(5.0f, 15.0f, 0.0f) == 5.0f);
        CHECK(lerp(-10.0f, 10.0f, 0.0f) == -10.0f);
    }

    SECTION("Lerp at t=1") {
        CHECK(lerp(0.0f, 10.0f, 1.0f) == 10.0f);
        CHECK(lerp(5.0f, 15.0f, 1.0f) == 15.0f);
        CHECK(lerp(-10.0f, 10.0f, 1.0f) == 10.0f);
    }

    SECTION("Lerp at t=0.5") {
        CHECK(lerp(0.0f, 10.0f, 0.5f) == 5.0f);
        CHECK(lerp(5.0f, 15.0f, 0.5f) == 10.0f);
        CHECK(lerp(-10.0f, 10.0f, 0.5f) == 0.0f);
    }

    SECTION("Lerp at t=0.25") {
        CHECK(lerp(0.0f, 10.0f, 0.25f) == 2.5f);
        CHECK(lerp(0.0f, 100.0f, 0.25f) == 25.0f);
    }

    SECTION("Lerp at t=0.75") {
        CHECK(lerp(0.0f, 10.0f, 0.75f) == 7.5f);
        CHECK(lerp(0.0f, 100.0f, 0.75f) == 75.0f);
    }

    SECTION("Lerp with negative values") {
        CHECK(lerp(-10.0f, 10.0f, 0.5f) == 0.0f);
        CHECK(lerp(-20.0f, -10.0f, 0.5f) == -15.0f);
    }

    SECTION("Lerp with same start and end") {
        CHECK(lerp(5.0f, 5.0f, 0.0f) == 5.0f);
        CHECK(lerp(5.0f, 5.0f, 0.5f) == 5.0f);
        CHECK(lerp(5.0f, 5.0f, 1.0f) == 5.0f);
    }

    SECTION("Integer lerp") {
        CHECK(lerp(0, 10, 0) == 0);
        CHECK(lerp(0, 10, 1) == 10);
        // Note: lerp with integers and float t requires explicit types
        // lerp<int>(0, 10, 0.5f) or use all floats
        CHECK(lerp(0.0f, 10.0f, 0.5f) == 5.0f);
    }

    SECTION("Double lerp") {
        CHECK(lerp(0.0, 10.0, 0.0) == 0.0);
        CHECK(lerp(0.0, 10.0, 1.0) == 10.0);
        CHECK_THAT(lerp(0.0, 10.0, 0.5), Catch::Matchers::WithinAbs(5.0, 0.0001));
    }

    SECTION("Lerp extrapolation (t outside 0-1)") {
        // Lerp can extrapolate
        CHECK(lerp(0.0f, 10.0f, -0.5f) == -5.0f);
        CHECK(lerp(0.0f, 10.0f, 1.5f) == 15.0f);
    }
}

// =============================================================================
// Combined Math Tests
// =============================================================================
TEST_CASE("Combined math operations", "[math][utils]") {
    SECTION("Clamp then lerp") {
        float value = 15.0f;
        float clamped = clamp(value, 0.0f, 10.0f);
        float result = lerp(0.0f, 100.0f, clamped / 10.0f);
        
        CHECK(clamped == 10.0f);
        CHECK(result == 100.0f);
    }

    SECTION("Angle interpolation with clamp") {
        float angle1 = 30.0f;
        float angle2 = 90.0f;
        float t = 1.5f; // Outside normal range
        
        float clamped_t = clamp(t, 0.0f, 1.0f);
        float result = lerp(angle1, angle2, clamped_t);
        
        CHECK(clamped_t == 1.0f);
        CHECK(result == 90.0f);
    }

    SECTION("Converting clamped value to radians") {
        float value = 150.0f;
        float clamped = clamp(value, 0.0f, 90.0f);
        float radians = to_radians(clamped);
        
        CHECK(clamped == 90.0f);
        CHECK_THAT(radians, Catch::Matchers::WithinAbs(PI / 2.0f, 0.0001f));
    }
}

// =============================================================================
// Edge Cases
// =============================================================================
TEST_CASE("Math utils edge cases", "[math][utils]") {
    SECTION("Very small values") {
        float small = 1e-7f;
        CHECK(to_radians(small) > 0.0f);
        CHECK(to_degrees(small) > 0.0f);
        CHECK(clamp(small, 0.0f, 1.0f) == small);
        CHECK(lerp(0.0f, 1.0f, small) == small);
    }

    SECTION("Very large values") {
        float large = 1e7f;
        CHECK_NOTHROW(to_radians(large));
        CHECK_NOTHROW(to_degrees(large));
        CHECK_NOTHROW(clamp(large, 0.0f, 100.0f));
        CHECK_NOTHROW(lerp(0.0f, large, 0.5f));
    }

    SECTION("to_radians with large angles") {
        CHECK_THAT(to_radians(720.0f), Catch::Matchers::WithinAbs(TWO_PI * 2.0f, 0.0001f));
        CHECK_THAT(to_radians(1080.0f), Catch::Matchers::WithinAbs(TWO_PI * 3.0f, 0.0001f));
    }

    SECTION("to_degrees with large angles") {
        CHECK_THAT(to_degrees(TWO_PI * 2.0f), Catch::Matchers::WithinAbs(720.0f, 0.0001f));
        CHECK_THAT(to_degrees(TWO_PI * 3.0f), Catch::Matchers::WithinAbs(1080.0f, 0.0001f));
    }
}
