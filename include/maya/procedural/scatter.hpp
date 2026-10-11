#pragma once
// Scatter (#1093, docs/procedural.md#scatter): items placed over a height field, cell by cell, as #1060's record
// chose. The world is tiled with squares a spacing wide, aligned to it; each square has one candidate at a
// hashed point in its middle half (a quarter of the spacing from its edges), so neighbours are at least half a
// spacing apart. A candidate's ID comes
// from the generator's ID and the square's place in the world, never from the rules: rules only remove
// candidates, so changing them keeps the items that survive, with their IDs.

#include "maya/core/identity.hpp"
#include "maya/math/affine.hpp"
#include "maya/procedural/height_field.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace maya {

inline constexpr size_t scatter_variants = 4; // the meshes a scatter generator chooses among

struct ScatterRules {
    uint64_t seed = 0;
    double spacing = 4.0; // metres; it divides the 128 m cell (2, 4, 8, ... or 1, 0.5, ...)
    float density = 0.6f; // the fraction of candidates kept, before the other rules
    float max_slope = 30.0f; // degrees
    float min_height = -10000.0f, max_height = 10000.0f; // metres
    float min_scale = 0.8f, max_scale = 1.2f;
    bool random_yaw = true;
    std::array<float, scatter_variants> weights{1.0f, 0.0f, 0.0f, 0.0f}; // relative; 0 never chosen
    float base_height = 0.0f; // where there is no height tile
};
/// A volume scatter leaves clear: a box (half extents in its frame) or a sphere (radius in metres, times
/// the frame's largest scale), at a world pose.
struct ScatterExclusion {
    enum class Shape : uint8_t { box, sphere } shape = Shape::box;
    math::Affine pose;
    math::Vec3 half_extents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f;
    bool contains(const math::DVec3& point) const noexcept;
};

struct ScatterItem {
    EntityId id;
    math::DVec3 position; // on the ground
    float yaw = 0.0f; // radians about +y
    float scale = 1.0f;
    uint8_t variant = 0; // which of the generator's meshes
    bool operator==(const ScatterItem&) const = default;
};

/// The ID of the candidate in square (gx, gz) of the world, for the generator `generator`.
EntityId scatter_item_id(const std::array<uint64_t, 2>& generator, int64_t gx, int64_t gz) noexcept;
/// Whether `spacing` tiles the 128 m cell exactly (a power of two from 1/8 m to 64 m). Inline: the property
/// schema (MayaWorld) checks it without linking MayaProcedural.
inline bool scatter_spacing_valid(double spacing) noexcept {
    if (!(spacing >= 0.125 && spacing <= 64.0)) return false;
    const auto squares = double(tile_size) / spacing;
    return squares == std::floor(squares) && std::has_single_bit(uint32_t(squares));
}

/// One cell's items, in square order (z, then x). `tile` is the cell's heights (its index must be `cell`), or
/// null for a flat ground at the base height. Exclusions are world-space. No items when no variant weighs
/// anything (no meshes); none at all when `context` is cancelled.
std::optional<std::vector<ScatterItem>> scatter_cell(const std::array<uint64_t, 2>& generator, const ScatterRules& rules, TileIndex cell,
                                                     const HeightTile* tile, std::span<const ScatterExclusion> exclusions,
                                                     const JobContext* context = nullptr);

/// Cooked bytes (the cook cache's `scatter-cell` entries) and back; nullopt for malformed bytes.
std::vector<std::byte> encode_scatter_items(std::span<const ScatterItem> items);
std::optional<std::vector<ScatterItem>> decode_scatter_items(std::span<const std::byte> bytes);

/// The height of a tile between its samples, at (x, z) metres from its low corner, on the triangles terrain
/// draws (each square split along its diagonal from (0, 0) to (1, 1)); and that triangle's slope in degrees.
float tile_height_at(const HeightTile& tile, double x, double z) noexcept;
float tile_slope_at(const HeightTile& tile, double x, double z) noexcept;

} // namespace maya
