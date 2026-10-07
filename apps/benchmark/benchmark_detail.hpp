#pragma once

// Pieces the benchmark runner's files share; not part of its interface.

#include "benchmark.hpp"
#include "maya/rhi/graphics_device.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace maya::benchmark::detail {

inline constexpr auto camera_id = EntityId{0x62656e63, 1}; // "benc"
inline constexpr auto light_id = EntityId{0x62656e63, 2};
inline constexpr uint64_t first_instance = 0x100;

/// The rotation that looks from `from` towards `to`, upright.
inline math::Quat looking(const math::Vec3& from, const math::Vec3& to) {
    auto forward = to - from;
    const auto length = forward.length();
    forward = length > 1e-6f ? forward * (1.0f / length) : math::Vec3{0.0f, 0.0f, -1.0f};
    const auto yaw = std::atan2(-forward.x, -forward.z);
    const auto pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    auto rotation = math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) * math::Quat::from_axis_angle({1.0f, 0.0f, 0.0f}, pitch);
    rotation.normalize();
    return rotation;
}

/// SplitMix64: a small, portable generator, so the same seed selects the same entities everywhere.
constexpr uint64_t mix(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

/// P1: runs every worker configuration and run of the manifest, and records them in `result`.
void run_physics(Result& result, const Manifest& manifest);
/// Import: imports and loads each model of the manifest in every run, and records them in `result`.
void run_import(Result& result, const Manifest& manifest, GraphicsDevice& device);
/// A1: imports the manifest's model into a new project at `root` and returns its scene file.
std::filesystem::path prepare_animation(const Manifest& manifest, const std::filesystem::path& root);
/// A1: `count` copies of the model's scene on a grid, each starting its clip (of `duration` seconds) at a
/// time chosen by seed, without their skins unless the manifest skins; a camera; a sun that casts shadows.
SceneDocument animated_scene(const Manifest& manifest, const SceneDocument& model, float duration);

} // namespace maya::benchmark::detail
