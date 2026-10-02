#pragma once
// CPU references of renderer.metal's exposure and tone mapping (docs/renderer.md#exposure-and-tone-mapping),
// for checking what the GPU writes. Keep them in step with the shader.

#include "maya/math/vector.hpp"
#include "maya/world/components.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace maya::test {

inline std::array<double, 3> multiply(const double (&m)[9], const std::array<double, 3>& v) {
    // Column-major, as Metal's float3x3 constructor takes columns.
    return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
            m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}

inline std::array<double, 3> agx(std::array<double, 3> v) {
    constexpr double inset[9] = {0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                 0.0784335999999992, 0.878468636469772, 0.0784336,
                                 0.0792237451477643, 0.0791661274605434, 0.879142973793104};
    constexpr double outset[9] = {1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                  -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                  -0.0990297440797205, -0.0989611768448433, 1.15107367264116};
    constexpr double min_ev = -12.47393, max_ev = 4.026069;
    v = multiply(inset, v);
    for (auto& x : v) {
        x = std::clamp(std::log2(std::max(x, 1e-10)), min_ev, max_ev);
        x = (x - min_ev) / (max_ev - min_ev);
        const auto x2 = x * x, x4 = x2 * x2;
        x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    }
    v = multiply(outset, v);
    for (auto& x : v) x = std::pow(std::max(x, 0.0), 2.2);
    return v;
}

inline std::array<double, 3> pbr_neutral(std::array<double, 3> c) {
    constexpr double start = 0.8 - 0.04, desaturation = 0.15;
    const auto x = std::min({c[0], c[1], c[2]});
    const auto offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    for (auto& v : c) v -= offset;
    const auto peak = std::max({c[0], c[1], c[2]});
    if (peak < start) return c;
    const auto d = 1.0 - start;
    const auto new_peak = 1.0 - d * d / (peak + d - start);
    for (auto& v : c) v *= new_peak / peak;
    const auto g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    for (auto& v : c) v = v + (new_peak - v) * g;
    return c;
}

inline double srgb_encode(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

/// The 8-bit value the tone-mapping pass writes for scene light `light` (linear RGB) at `exposure`.
inline std::array<int, 3> displayed(std::array<double, 3> light, double exposure, ToneMapping tone = ToneMapping::agx) {
    for (auto& v : light) v = std::max(v, 0.0) * exposure;
    const auto mapped = tone == ToneMapping::pbr_neutral ? pbr_neutral(light) : agx(light);
    return {int(std::lround(srgb_encode(mapped[0]) * 255.0)), int(std::lround(srgb_encode(mapped[1]) * 255.0)),
            int(std::lround(srgb_encode(mapped[2]) * 255.0))};
}
inline int displayed_grey(double light, double exposure, ToneMapping tone = ToneMapping::agx) {
    return displayed({light, light, light}, exposure, tone)[0];
}

} // namespace maya::test
