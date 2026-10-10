#pragma once
// Generator assets (#1068, docs/procedural.md#generator-assets): what a generator makes, from what, and where.
// A generator file saves its kind, its algorithm version, its seed, its parameters, and its region; its
// output is cooked per cell, keyed by all of them. Heights are the one kind so far (`heightfield.noise`);
// scatter comes with #1093.

#include "maya/procedural/noise.hpp"
#include <cstdint>

namespace maya {

enum class GeneratorKind : uint32_t { heightfield_noise };
/// The algorithm versions this build generates, by kind. A file names the version it was made with; a
/// version this build does not know is refused, so output never changes silently.
inline constexpr uint32_t heightfield_noise_version = 1;

struct GeneratorAsset {
    GeneratorKind kind = GeneratorKind::heightfield_noise;
    uint32_t version = heightfield_noise_version;
    int32_t seed = 990; // 0..2^31-1
    // heightfield.noise (HeightfieldNoise)
    int32_t octaves = 6;
    float wavelength = 900.0f; // metres
    float amplitude = 140.0f; // metres
    float warp = 120.0f; // metres
    float base = 0.0f; // metres
    // The region: 128 m cells from the low corner to the high one, inclusive.
    int32_t region_min_x = 0, region_min_z = 0;
    int32_t region_max_x = 3, region_max_z = 3;
    bool operator==(const GeneratorAsset&) const = default;
};

inline HeightfieldNoise heightfield_noise(const GeneratorAsset& generator) noexcept {
    return {uint64_t(uint32_t(generator.seed)), uint32_t(generator.octaves), generator.wavelength, generator.amplitude, generator.warp,
            generator.base};
}

} // namespace maya
