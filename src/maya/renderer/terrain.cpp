#include "maya/renderer/terrain.hpp"
#include "maya/renderer/draw_batches.hpp"
#include <algorithm>
#include <cmath>

namespace maya {

HeightBuffer::HeightBuffer(GraphicsDevice& device, std::span<const uint32_t> packed, const std::string& label)
    : m_device(device), m_lifetime(device.resource_lifetime()) {
    if (packed.empty() || m_lifetime.expired()) return;
    auto created = m_device.create_buffer({packed.size_bytes(), BufferUsage::uniform, label}, packed.data());
    if (!created) return;
    m_handle = created.handle;
    m_bytes = packed.size_bytes();
}
HeightBuffer::~HeightBuffer() {
    if (!m_lifetime.expired() && m_handle.valid()) m_device.destroy(m_handle);
}

namespace {
struct Selector {
    const TerrainExtent& terrain;
    math::DVec3 eye;
    const Frustum* volume;
    math::DVec3 origin;
    double region_low_x, region_low_z, region_high_x, region_high_z;
    std::vector<TerrainPatchChoice>& out;
    double lowest = 0.0, highest = 0.0; // the whole terrain's heights, for distances

    int32_t tile_of(double x, double z) const {
        const auto index = TileIndex{int32_t(std::floor(x / tile_size)), int32_t(std::floor(z / tile_size))};
        const auto found = std::ranges::lower_bound(terrain.tiles, index);
        return found != terrain.tiles.end() && *found == index ? int32_t(found - terrain.tiles.begin()) : -1;
    }
    void visit(double x, double z, uint32_t level) {
        const auto size = 32.0 * double(1u << level);
        if (x >= region_high_x || z >= region_high_z || x + size <= region_low_x || z + size <= region_low_z) return;
        // Its heights' range: its tile's, below the overview's levels, and the overview's otherwise.
        const auto tile = level < terrain_tile_levels ? tile_of(x, z) : -1;
        auto low = double(terrain.overview_min), high = double(terrain.overview_max);
        if (tile >= 0) std::tie(low, high) = std::pair{double(terrain.tile_ranges[size_t(tile)].first), double(terrain.tile_ranges[size_t(tile)].second)};
        if (volume) {
            const auto lo = math::Vec3{float(x - origin.x), float(low - origin.y), float(z - origin.z)};
            const auto hi = math::Vec3{float(x + size - origin.x), float(high - origin.y), float(z + size - origin.z)};
            if (volume->classify(lo, hi) == Containment::outside) return;
        }
        const auto dx = std::max({x - eye.x, 0.0, eye.x - (x + size)});
        const auto dy = terrain_height_gap(eye.y, lowest, highest); // the vertex shader's, exactly
        const auto dz = std::max({z - eye.z, 0.0, eye.z - (z + size)});
        const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (level == 0 || distance > terrain_range(level - 1)) {
            out.push_back({x, z, level, tile, tile >= 0 && level + 1 == terrain_tile_levels});
            return;
        }
        const auto half = size * 0.5;
        visit(x, z, level - 1);
        visit(x + half, z, level - 1);
        visit(x, z + half, level - 1);
        visit(x + half, z + half, level - 1);
    }
};
} // namespace

std::pair<double, double> terrain_band(const TerrainExtent& terrain) noexcept {
    auto lowest = double(terrain.overview_min), highest = double(terrain.overview_max);
    for (const auto& [low, high] : terrain.tile_ranges) {
        lowest = std::min(lowest, double(low));
        highest = std::max(highest, double(high));
    }
    return {lowest, highest};
}

void select_terrain_patches(const TerrainExtent& terrain, const math::DVec3& eye, const Frustum* volume, const math::DVec3& origin,
                            std::vector<TerrainPatchChoice>& out) {
    const auto first = out.size();
    auto selector = Selector{terrain, eye, volume, origin, double(terrain.low.x) * tile_size, double(terrain.low.z) * tile_size,
                             double(terrain.high.x + 1) * tile_size, double(terrain.high.z + 1) * tile_size, out};
    std::tie(selector.lowest, selector.highest) = terrain_band(terrain);
    // Roots: the coarsest level's squares, aligned to the world's grid, over the region.
    const auto root = 32.0 * double(1u << (terrain_levels - 1));
    const auto x0 = std::floor(selector.region_low_x / root) * root, z0 = std::floor(selector.region_low_z / root) * root;
    for (auto z = z0; z < selector.region_high_z; z += root)
        for (auto x = x0; x < selector.region_high_x; x += root) selector.visit(x, z, terrain_levels - 1);
    // Grouped by what they read, so each tile is one instanced draw; the overview's last.
    std::stable_sort(out.begin() + ptrdiff_t(first), out.end(), [](const TerrainPatchChoice& a, const TerrainPatchChoice& b) {
        return uint32_t(a.tile) < uint32_t(b.tile); // -1 sorts last
    });
}

} // namespace maya
