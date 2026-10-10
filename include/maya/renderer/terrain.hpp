#pragma once
// Terrain drawing (#1068, docs/renderer.md#terrain): what a terrain hands the renderer (its tiles' and
// overview's packed heights on the device), and CDLOD selection: a quadtree aligned to the world, whose
// nodes are patches of 32 x 32 quads from 32 m (1 m quads) to 4096 m, each level drawn out to four times
// its size and morphing into the next over the last quarter of that range. Distances are horizontal, plus
// one height for the whole terrain (the eye's height above or below its band of heights), the same on the CPU
// and the GPU. With those numbers a node never borders one two levels coarser, a finer node has finished
// morphing where it meets a coarser one, and the coarser has not begun (docs/renderer.md#terrain): levels
// meet without cracks.

#include "maya/assets/generator.hpp"
#include "maya/math/vector.hpp"
#include "maya/procedural/height_field.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/world/components.hpp"
#include "maya/world/world.hpp"
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace maya {

struct Frustum; // draw_batches.hpp

/// Packed heights (pack_heights) in a device buffer, destroyed through the device when the last holder lets
/// it go: snapshots hold it until their frame is encoded.
class HeightBuffer {
public:
    HeightBuffer(GraphicsDevice& device, std::span<const uint32_t> packed, const std::string& label);
    ~HeightBuffer();
    HeightBuffer(const HeightBuffer&) = delete;
    HeightBuffer& operator=(const HeightBuffer&) = delete;
    bool valid() const noexcept { return !m_lifetime.expired() && m_handle.valid(); }
    BufferHandle handle() const noexcept { return m_handle; }
    size_t bytes() const noexcept { return m_bytes; }

private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    BufferHandle m_handle{};
    size_t m_bytes = 0;
};

/// A tile a terrain draws: its heights and, given a device, their buffer.
struct TerrainTileView {
    TileIndex index;
    std::shared_ptr<const HeightTile> heights;
    std::shared_ptr<const HeightBuffer> buffer;
};
/// One terrain as the renderer and picking see it.
struct TerrainView {
    EntityHandle entity;
    EntityId id;
    TerrainComponent component;
    GeneratorAsset generator;
    std::shared_ptr<const HeightOverview> overview; // none until generated
    std::shared_ptr<const HeightBuffer> overview_buffer;
    std::vector<TerrainTileView> tiles; // loaded, by index; their buffers once uploaded
};

inline constexpr uint32_t terrain_levels = 8; // patches of 32 m to 4096 m
inline constexpr uint32_t terrain_tile_levels = 3; // levels 0 to 2 (32 m to 128 m patches) read tiles; the rest, the overview
inline constexpr uint32_t terrain_quads = 32; // per patch side
/// How far a level's patches are drawn: four times their size (level 0, 128 m; level 2, the tiles' last, 512 m).
constexpr double terrain_range(uint32_t level) noexcept { return 128.0 * double(1u << level); }
/// Where a level's morph into the next coarser one starts: the last quarter of its range.
constexpr double terrain_morph_start(uint32_t level) noexcept {
    const auto previous = level == 0 ? 0.0 : terrain_range(level - 1);
    return terrain_range(level) - 0.25 * (terrain_range(level) - previous);
}
/// How far from the eye a tile can be read: the tiles' last range, and the diagonal of the node above it.
inline constexpr double terrain_tile_reach = 128.0 * 4.0 + 256.0 * 1.4142135623730951;
/// The height part of every terrain distance: how far the eye is above or below the terrain's band of heights.
constexpr double terrain_height_gap(double eye_y, double lowest, double highest) noexcept {
    return eye_y > highest ? eye_y - highest : eye_y < lowest ? lowest - eye_y : 0.0;
}

/// What selection needs of a terrain: its region, its tiles' height ranges, and its overview's. Culling uses
/// each node's own heights; distances use the whole terrain's band (terrain_height_gap).
struct TerrainExtent {
    TileIndex low, high; // the region, in cells
    float overview_min = 0.0f, overview_max = 0.0f;
    std::span<const TileIndex> tiles; // resident, sorted
    std::span<const std::pair<float, float>> tile_ranges; // each tile's lowest and highest height
};
/// One patch to draw.
struct TerrainPatchChoice {
    double x = 0.0, z = 0.0; // its corner, in world metres
    uint32_t level = 0;
    int32_t tile = -1; // into TerrainExtent::tiles when it reads a tile; -1 for the overview
    bool blend = false; // a tile's coarsest level: its heights blend toward the overview as it morphs
    double size() const noexcept { return 32.0 * double(1u << level); }
};
/// The patches that cover the region for an eye, cut to `volume` (in the frame of `origin`, as the renderer's
/// views are, #1065) when given, sorted by the tile they read, overview last.
void select_terrain_patches(const TerrainExtent& terrain, const math::DVec3& eye, const Frustum* volume, const math::DVec3& origin,
                            std::vector<TerrainPatchChoice>& out);
/// The terrain's band of heights: its overview's and its tiles' lowest and highest.
std::pair<double, double> terrain_band(const TerrainExtent& terrain) noexcept;

} // namespace maya
