#pragma once
// Maya's own seeded gradient noise (#1068, docs/procedural.md#noise): fBm with a domain warp on an integer
// lattice hashed with the seed, with no library, as #1060's record chose.

#include <cstdint>

namespace maya {

/// The noise height field's parameters (generator kind `heightfield.noise`). Heights are
/// base + amplitude x fBm(position warped by warp x fBm), in metres.
struct HeightfieldNoise {
    uint64_t seed = 990;
    uint32_t octaves = 6; // 1..12
    float wavelength = 900.0f; // metres, of the first octave
    float amplitude = 140.0f; // metres
    float warp = 120.0f; // metres of domain warp
    float base = 0.0f; // metres added to every height
    bool operator==(const HeightfieldNoise&) const = default;
};

/// The height at a world position in whole metres. The same inputs give the same bits on every thread and
/// run; results are not promised identical across compilers or platforms (cook them where they are used).
float noise_height(const HeightfieldNoise& noise, int64_t x, int64_t z) noexcept;

} // namespace maya
