#pragma once
// Height fields (#1068, docs/procedural.md#height-fields): Maya's own seeded gradient noise, and the tiles of
// heights terrain draws and collides with. #1060's record chose them: one tile per 128 m cell, 129 x 129
// samples a metre apart, sharing its edge row and column with its neighbours, so neighbouring tiles agree
// exactly; and an overview of the whole region at 16 m for what lies beyond the tiles loaded.

#include "maya/jobs/jobs.hpp"
#include "maya/procedural/noise.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace maya {

inline constexpr uint32_t tile_samples = 129; // per side
inline constexpr int32_t tile_size = 128; // metres a side: a world cell
inline constexpr int32_t overview_spacing = 16; // metres between the overview's samples

/// A tile's place on the grid of 128 m cells: floor(position / 128) in x and z.
struct TileIndex {
    int32_t x = 0;
    int32_t z = 0;
    auto operator<=>(const TileIndex&) const = default;
};

/// One cell's heights, with a normal per sample taken from its neighbours (across the tile's edges too, so
/// lighting is continuous between tiles).
struct HeightTile {
    TileIndex index;
    std::vector<float> heights; // tile_samples x tile_samples, rows along x, one row per metre of z
    std::vector<uint16_t> normals; // octahedral, 8 bits each of x and z (oct_encode)
    float min = 0.0f, max = 0.0f;
    float height(uint32_t x, uint32_t z) const noexcept { return heights[size_t(z) * tile_samples + x]; }
};
/// Generates a tile. Rows are split across `job_system()` when `parallel` (the result is the same either way);
/// none when the context is cancelled meanwhile.
std::optional<HeightTile> generate_height_tile(const HeightfieldNoise& noise, TileIndex index, const JobContext* context = nullptr,
                                               bool parallel = false);

/// The whole region at 16 m: cells `low` to `high` inclusive, (cells x 8 + 1) samples a side, each equal to
/// the tiles' sample at the same point.
struct HeightOverview {
    TileIndex low, high;
    uint32_t width = 0, depth = 0; // samples along x and z
    std::vector<float> heights;
    std::vector<uint16_t> normals;
    float min = 0.0f, max = 0.0f;
};
std::optional<HeightOverview> generate_height_overview(const HeightfieldNoise& noise, TileIndex low, TileIndex high,
                                                       const JobContext* context = nullptr, bool parallel = false);

/// A unit normal as two bytes: its octahedral projection onto x and z (y up).
uint16_t oct_encode(float x, float y, float z) noexcept;
/// The GPU's form of a tile or the overview: two words per sample, its height as a float and its normal
/// (docs/procedural.md#on-the-gpu). Whole floats, not 16 bits over each tile's range as #1060's record
/// proposed: neighbouring tiles must decode their shared edge to the same bits, or it cracks.
std::vector<uint32_t> pack_heights(std::span<const float> heights, std::span<const uint16_t> normals);

/// Cooked bytes (the cook cache's `height-tile` entries) and back; nullopt for malformed bytes.
std::vector<std::byte> encode_height_tile(const HeightTile& tile);
std::optional<HeightTile> decode_height_tile(std::span<const std::byte> bytes);
std::vector<std::byte> encode_height_overview(const HeightOverview& overview);
std::optional<HeightOverview> decode_height_overview(std::span<const std::byte> bytes);

} // namespace maya
