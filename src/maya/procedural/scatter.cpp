#include "maya/procedural/scatter.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace maya {
namespace {
uint64_t mix(uint64_t x) noexcept { // SplitMix64's finalizer, as the noise's
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
/// The square's own random stream: its seed and its place, nothing else.
uint64_t square_hash(uint64_t seed, int64_t gx, int64_t gz) noexcept {
    return mix(seed ^ mix(uint64_t(gx) * 0x9e3779b1ull ^ mix(uint64_t(gz) * 0x85ebca77ull)));
}
float unit(uint64_t h, uint64_t k) noexcept { return float(mix(h + k) >> 40) / float(1u << 24); } // [0, 1)

struct Triangle {
    float height, dx, dz; // at the point, and the plane's gradient
};
Triangle triangle_at(const HeightTile& tile, double x, double z) noexcept {
    const auto fx = std::clamp(x, 0.0, double(tile_size)), fz = std::clamp(z, 0.0, double(tile_size));
    const auto ix = std::min(uint32_t(fx), tile_samples - 2), iz = std::min(uint32_t(fz), tile_samples - 2);
    const auto tx = float(fx - ix), tz = float(fz - iz);
    const auto h00 = tile.height(ix, iz), h10 = tile.height(ix + 1, iz), h01 = tile.height(ix, iz + 1), h11 = tile.height(ix + 1, iz + 1);
    if (tx >= tz) return {h00 + (h10 - h00) * tx + (h11 - h10) * tz, h10 - h00, h11 - h10};
    return {h00 + (h01 - h00) * tz + (h11 - h01) * tx, h11 - h01, h01 - h00};
}

template <class T> void put(std::vector<std::byte>& out, const T& value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}
template <class T> bool take(std::span<const std::byte>& in, T& value) {
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&value, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}
constexpr uint32_t scatter_magic = 0x31544353; // "SCT1"
} // namespace

bool ScatterExclusion::contains(const math::DVec3& point) const noexcept {
    const auto offset = (point - pose.translation).to_float();
    if (shape == Shape::sphere) {
        auto scale = 0.0f;
        for (int c = 0; c < 3; ++c) scale = std::max(scale, pose.axis(c).length());
        return offset.length() <= radius * scale;
    }
    // In the box's frame: along each axis, the offset's projection over the axis's length (scale).
    const float half[3] = {half_extents.x, half_extents.y, half_extents.z};
    for (int c = 0; c < 3; ++c) {
        const auto axis = pose.axis(c);
        const auto length2 = math::Vec3::dot(axis, axis);
        if (length2 <= 0.0f) return false;
        if (std::abs(math::Vec3::dot(offset, axis) / length2) > half[c]) return false;
    }
    return true;
}

EntityId scatter_item_id(const std::array<uint64_t, 2>& generator, int64_t gx, int64_t gz) noexcept {
    const auto place = mix(uint64_t(gx) * 0xd6e8feb86659fd93ull ^ mix(uint64_t(gz) ^ 0x5ca7ull));
    auto id = EntityId{mix(generator[0] ^ place), mix(generator[1] ^ mix(place))};
    if (!id.valid()) id.low = 1; // never the invalid ID
    return id;
}

float tile_height_at(const HeightTile& tile, double x, double z) noexcept { return triangle_at(tile, x, z).height; }

float tile_slope_at(const HeightTile& tile, double x, double z) noexcept {
    const auto t = triangle_at(tile, x, z);
    return std::atan(std::sqrt(t.dx * t.dx + t.dz * t.dz)) * float(180.0 / std::numbers::pi);
}

std::optional<std::vector<ScatterItem>> scatter_cell(const std::array<uint64_t, 2>& generator, const ScatterRules& rules, TileIndex cell,
                                                     const HeightTile* tile, std::span<const ScatterExclusion> exclusions,
                                                     const JobContext* context) {
    if (!scatter_spacing_valid(rules.spacing)) return std::vector<ScatterItem>{};
    const auto squares = int64_t(double(tile_size) / rules.spacing);
    auto total = 0.0f;
    for (const auto weight : rules.weights) total += std::max(weight, 0.0f);
    auto items = std::vector<ScatterItem>{};
    if (total <= 0.0f) return items; // nothing to place
    for (int64_t sz = 0; sz < squares; ++sz) {
        if (context && context->cancelled()) return std::nullopt;
        for (int64_t sx = 0; sx < squares; ++sx) {
            const auto gx = int64_t(cell.x) * squares + sx, gz = int64_t(cell.z) * squares + sz;
            const auto h = square_hash(rules.seed, gx, gz);
            if (unit(h, 0) >= rules.density) continue;
            // Within the square, half the spacing clear of its edges: two items are at least half a spacing apart.
            const auto x = (double(gx) + 0.25 + 0.5 * unit(h, 1)) * rules.spacing;
            const auto z = (double(gz) + 0.25 + 0.5 * unit(h, 2)) * rules.spacing;
            const auto local_x = x - double(cell.x) * tile_size, local_z = z - double(cell.z) * tile_size;
            const auto y = tile ? double(tile_height_at(*tile, local_x, local_z)) : double(rules.base_height);
            if (y < rules.min_height || y > rules.max_height) continue;
            if (tile && tile_slope_at(*tile, local_x, local_z) > rules.max_slope) continue;
            const auto point = math::DVec3{x, y, z};
            if (std::ranges::any_of(exclusions, [&](const ScatterExclusion& exclusion) { return exclusion.contains(point); })) continue;
            auto variant = uint8_t{0};
            if (total > 0.0f) { // weighted: each variant's share of the total
                auto pick = unit(h, 3) * total;
                for (size_t v = 0; v < scatter_variants; ++v) {
                    const auto weight = std::max(rules.weights[v], 0.0f);
                    if (weight <= 0.0f) continue;
                    variant = uint8_t(v);
                    if (pick < weight) break;
                    pick -= weight;
                }
            }
            const auto yaw = rules.random_yaw ? unit(h, 4) * float(2.0 * std::numbers::pi) : 0.0f;
            const auto scale = rules.min_scale + (rules.max_scale - rules.min_scale) * unit(h, 5);
            items.push_back({scatter_item_id(generator, gx, gz), point, yaw, scale, variant});
        }
    }
    return items;
}

std::vector<std::byte> encode_scatter_items(std::span<const ScatterItem> items) {
    auto out = std::vector<std::byte>{};
    out.reserve(8 + items.size() * 49);
    put(out, scatter_magic);
    put(out, uint32_t(items.size()));
    for (const auto& item : items) {
        put(out, item.id.high);
        put(out, item.id.low);
        put(out, item.position.x);
        put(out, item.position.y);
        put(out, item.position.z);
        put(out, item.yaw);
        put(out, item.scale);
        put(out, item.variant);
    }
    return out;
}

std::optional<std::vector<ScatterItem>> decode_scatter_items(std::span<const std::byte> bytes) {
    auto magic = uint32_t{0}, count = uint32_t{0};
    if (!take(bytes, magic) || magic != scatter_magic || !take(bytes, count)) return std::nullopt;
    constexpr size_t item_bytes = 8 + 8 + 3 * 8 + 4 + 4 + 1;
    if (bytes.size() != size_t(count) * item_bytes) return std::nullopt;
    auto items = std::vector<ScatterItem>(count);
    for (auto& item : items) {
        take(bytes, item.id.high);
        take(bytes, item.id.low);
        take(bytes, item.position.x);
        take(bytes, item.position.y);
        take(bytes, item.position.z);
        take(bytes, item.yaw);
        take(bytes, item.scale);
        take(bytes, item.variant);
        if (!std::isfinite(item.position.x) || !std::isfinite(item.position.y) || !std::isfinite(item.position.z) || !std::isfinite(item.yaw) ||
            !std::isfinite(item.scale) || item.variant >= scatter_variants || !item.id.valid())
            return std::nullopt;
    }
    return items;
}

} // namespace maya
