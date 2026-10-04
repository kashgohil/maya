#pragma once
// A CPU reference of renderer.metal's glTF metallic-roughness shading (docs/renderer.md#materials), for
// checking what the GPU writes to the HDR scene target. Keep it in step with the shader.

#include "maya/assets/environment_cook.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace maya::test {

using Rgb = std::array<double, 3>;
using Direction = std::array<double, 3>;

inline constexpr double min_roughness = 0.045;

inline double dot(const Direction& a, const Direction& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Direction normalize(Direction v) {
    const auto length = std::sqrt(dot(v, v));
    return {v[0] / length, v[1] / length, v[2] / length};
}

/// A surface's shading inputs, after its maps.
struct Surface {
    Rgb base{1, 1, 1};
    double metallic = 0;
    double roughness = 1; // perceptual, clamped as the shader does
};

inline double ggx(double NdotH, double alpha) {
    const auto a2 = alpha * alpha;
    const auto d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (std::numbers::pi * d * d);
}
inline double smith_visibility(double NdotL, double NdotV, double alpha) {
    const auto a2 = alpha * alpha;
    const auto v = NdotL * std::sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const auto l = NdotV * std::sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return v + l > 0.0 ? 0.5 / (v + l) : 0.0;
}

/// The BRDF times NdotL: what one unit of light arriving from L sends toward V.
inline Rgb reflected(const Surface& s, const Direction& N, const Direction& V, const Direction& L) {
    const auto NdotL = dot(N, L);
    if (NdotL <= 0.0) return {0, 0, 0};
    const auto roughness = std::clamp(s.roughness, min_roughness, 1.0);
    const auto alpha = roughness * roughness;
    const auto metallic = std::clamp(s.metallic, 0.0, 1.0);
    const auto H = normalize({L[0] + V[0], L[1] + V[1], L[2] + V[2]});
    const auto NdotV = std::max(dot(N, V), 1e-4);
    const auto NdotH = std::clamp(dot(N, H), 0.0, 1.0), VdotH = std::clamp(dot(V, H), 0.0, 1.0);
    auto out = Rgb{};
    for (int c = 0; c < 3; ++c) {
        const auto f0 = 0.04 + (s.base[c] - 0.04) * metallic;
        const auto F = f0 + (1.0 - f0) * std::pow(1.0 - VdotH, 5.0);
        const auto diffuse = (1.0 - F) * s.base[c] * (1.0 - metallic) / std::numbers::pi;
        const auto specular = F * ggx(NdotH, alpha) * smith_visibility(NdotL, NdotV, alpha);
        out[c] = (diffuse + specular) * NdotL;
    }
    return out;
}

/// The specular's share of light from the surroundings: F0 x scale + bias from the split-sum table
/// (environment_cook.hpp), per channel.
inline Rgb specular_share(const Surface& s, double NdotV) {
    const auto roughness = std::clamp(s.roughness, min_roughness, 1.0);
    const auto metallic = std::clamp(s.metallic, 0.0, 1.0);
    const auto [scale, bias] = brdf_scale_bias(std::max(NdotV, 1e-4), roughness);
    auto out = Rgb{};
    for (int c = 0; c < 3; ++c) out[c] = (0.04 + (s.base[c] - 0.04) * metallic) * scale + bias;
    return out;
}
/// What light from the surroundings sends toward V, before occlusion: `diffuse` light (irradiance / pi)
/// to the diffuse share, and `specular` light (the prefiltered radiance) to the specular's.
inline Rgb surroundings(const Surface& s, double NdotV, const Rgb& diffuse, const Rgb& specular) {
    const auto share = specular_share(s, NdotV);
    const auto metallic = std::clamp(s.metallic, 0.0, 1.0);
    auto out = Rgb{};
    for (int c = 0; c < 3; ++c) out[c] = s.base[c] * (1.0 - metallic) * (1.0 - share[c]) * diffuse[c] + share[c] * specular[c];
    return out;
}
/// What a uniform environment of radiance 1 (the ambient light) sends toward V, before occlusion.
inline Rgb environment(const Surface& s, double NdotV) { return surroundings(s, NdotV, {1, 1, 1}, {1, 1, 1}); }

/// The fraction of light arriving from all directions over the hemisphere that leaves toward V (the
/// directional albedo), by midpoint integration in `steps` x 4 `steps` cells. A furnace that conserves
/// energy never exceeds 1.
inline Rgb directional_albedo(const Surface& s, double NdotV, int steps = 256) {
    const auto N = Direction{0, 0, 1};
    const auto V = Direction{std::sqrt(std::max(0.0, 1.0 - NdotV * NdotV)), 0, NdotV};
    auto total = Rgb{};
    const auto pi = std::numbers::pi;
    for (int i = 0; i < steps; ++i) {
        const auto theta = (i + 0.5) / steps * pi / 2;
        for (int j = 0; j < 4 * steps; ++j) {
            const auto phi = (j + 0.5) / (4 * steps) * 2 * pi;
            const auto L = Direction{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
            const auto solid_angle = std::sin(theta) * (pi / 2 / steps) * (2 * pi / (4 * steps));
            const auto f = reflected(s, N, V, L); // already times NdotL
            for (int c = 0; c < 3; ++c) total[c] += f[c] * solid_angle;
        }
    }
    return total;
}

} // namespace maya::test
