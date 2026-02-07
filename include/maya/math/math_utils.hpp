#pragma once

#include <cmath>
#include <limits>
#include <algorithm>

namespace maya::math {

// Constants
constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = PI * 2.0f;
constexpr float HALF_PI = PI * 0.5f;
constexpr float EPSILON = std::numeric_limits<float>::epsilon();

// Utils
template <typename T>
constexpr T to_radians(T degrees) {
    return degrees * (static_cast<T>(PI) / static_cast<T>(180.0));
}

template <typename T>
constexpr T to_degrees(T radians) {
    return radians * (static_cast<T>(180.0) / static_cast<T>(PI));
}

template <typename T>
constexpr T clamp(T value, T min, T max) {
    return std::max(min, std::min(value, max));
}

template <typename T>
constexpr T lerp(T a, T b, T t) {
    return a + (b - a) * t;
}

} // namespace maya::math
