#pragma once
// Generator assets (#1068, docs/procedural.md#generator-assets): what a generator makes, from what, and where.
// A generator file saves its kind, its algorithm version, its seed, its parameters, and its region; its
// output is cooked per cell, keyed by all of them. Two kinds: heights (`heightfield.noise`), and scatter
// (#1093, docs/procedural.md#scatter), items placed over a height field's terrain.

#include "maya/assets/asset_ref.hpp"
#include "maya/procedural/noise.hpp"
#include "maya/procedural/scatter.hpp"
#include <cstdint>

namespace maya {

enum class GeneratorKind : uint32_t { heightfield_noise, scatter };
/// The algorithm versions this build generates, by kind. A file names the version it was made with; a
/// version this build does not know is refused, so output never changes silently.
inline constexpr uint32_t heightfield_noise_version = 1;
inline constexpr uint32_t scatter_version = 1;

struct GeneratorAsset {
    GeneratorKind kind = GeneratorKind::heightfield_noise;
    uint32_t version = heightfield_noise_version;
    int32_t seed = 990; // 0..2^31-1
    // heightfield.noise (HeightfieldNoise)
    int32_t octaves = 6;
    float wavelength = 900.0f; // metres
    float amplitude = 140.0f; // metres
    float warp = 120.0f; // metres
    float base = 0.0f; // metres; scatter: the ground's height where it names no heights
    // scatter (ScatterRules), over `heights`' terrain
    AssetRef<GeneratorAsset> heights{}; // a heightfield.noise generator; none for a flat ground
    float spacing = 4.0f; // metres; a power of two that divides 128 m
    float density = 0.6f; // 0..1
    float max_slope = 30.0f; // degrees
    float min_height = -10000.0f, max_height = 10000.0f; // metres
    float min_scale = 0.8f, max_scale = 1.2f;
    bool random_yaw = true;
    // Its meshes: each a mesh, its material, a weight (a variant without a mesh is never chosen), and a
    // sphere collider's radius in metres before the item's scale (0 for none).
    AssetRef<MeshAsset> mesh0{}, mesh1{}, mesh2{}, mesh3{};
    AssetRef<MaterialAsset> material0{}, material1{}, material2{}, material3{};
    float weight0 = 1.0f, weight1 = 1.0f, weight2 = 1.0f, weight3 = 1.0f;
    float collider0 = 0.0f, collider1 = 0.0f, collider2 = 0.0f, collider3 = 0.0f;
    // The region: 128 m cells from the low corner to the high one, inclusive.
    int32_t region_min_x = 0, region_min_z = 0;
    int32_t region_max_x = 3, region_max_z = 3;
    bool operator==(const GeneratorAsset&) const = default;
};

/// A scatter generator's rules: variants without a mesh weigh nothing.
inline ScatterRules scatter_rules(const GeneratorAsset& generator) noexcept {
    auto rules = ScatterRules{};
    rules.seed = uint64_t(uint32_t(generator.seed));
    rules.spacing = generator.spacing;
    rules.density = generator.density;
    rules.max_slope = generator.max_slope;
    rules.min_height = generator.min_height;
    rules.max_height = generator.max_height;
    rules.min_scale = generator.min_scale;
    rules.max_scale = generator.max_scale;
    rules.random_yaw = generator.random_yaw;
    rules.weights = {generator.mesh0.valid() ? generator.weight0 : 0.0f, generator.mesh1.valid() ? generator.weight1 : 0.0f,
                     generator.mesh2.valid() ? generator.weight2 : 0.0f, generator.mesh3.valid() ? generator.weight3 : 0.0f};
    rules.base_height = generator.base;
    return rules;
}

inline HeightfieldNoise heightfield_noise(const GeneratorAsset& generator) noexcept {
    return {uint64_t(uint32_t(generator.seed)), uint32_t(generator.octaves), generator.wavelength, generator.amplitude, generator.warp,
            generator.base};
}

} // namespace maya
