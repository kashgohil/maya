#include "maya/procedural/height_field.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace maya {
namespace {
uint64_t mix(uint64_t x) noexcept { // SplitMix64's finalizer
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
// A gradient on the integer lattice, from the seed and the lattice point alone.
float gradient(uint64_t seed, int64_t ix, int64_t iz, float fx, float fz) noexcept {
    const auto h = mix(seed ^ mix(uint64_t(ix) * 0x8da6b343u ^ uint64_t(iz) * 0xd8163841u));
    const auto angle = float(h >> 40) * (6.2831853f / float(1u << 24));
    return std::cos(angle) * fx + std::sin(angle) * fz;
}
float smooth(float t) noexcept { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float gradient_noise(uint64_t seed, double x, double z) noexcept {
    const auto x0 = std::floor(x), z0 = std::floor(z);
    const auto ix = int64_t(x0), iz = int64_t(z0);
    const auto fx = float(x - x0), fz = float(z - z0);
    const auto a = gradient(seed, ix, iz, fx, fz), b = gradient(seed, ix + 1, iz, fx - 1.0f, fz);
    const auto c = gradient(seed, ix, iz + 1, fx, fz - 1.0f), d = gradient(seed, ix + 1, iz + 1, fx - 1.0f, fz - 1.0f);
    const auto u = smooth(fx), v = smooth(fz);
    const auto near = a + (b - a) * u, far = c + (d - c) * u;
    return near + (far - near) * v;
}
float fbm(const HeightfieldNoise& noise, uint64_t seed, double x, double z) noexcept {
    auto sum = 0.0f, amplitude = 1.0f;
    auto frequency = 1.0 / double(noise.wavelength);
    for (uint32_t octave = 0; octave < noise.octaves; ++octave) {
        sum += gradient_noise(seed + octave, x * frequency, z * frequency) * amplitude;
        frequency *= 2.0;
        amplitude *= 0.5f;
    }
    return sum;
}

// Heights on a grid of `spacing` metres from (x0, z0), with one more sample on every side for the normals.
struct Grid {
    int64_t x0, z0, spacing;
    uint32_t width, depth;
};
bool fill(const HeightfieldNoise& noise, const Grid& grid, std::vector<float>& heights, std::vector<uint16_t>& normals,
          float& low, float& high, const JobContext* context, bool parallel) {
    const auto apron_width = grid.width + 2, apron_depth = grid.depth + 2;
    auto apron = std::vector<float>(size_t(apron_width) * apron_depth);
    const auto rows = [&](uint32_t first, uint32_t last) {
        for (auto z = first; z < last; ++z) {
            if (context && context->cancelled()) return;
            for (uint32_t x = 0; x < apron_width; ++x)
                apron[size_t(z) * apron_width + x] = noise_height(noise, grid.x0 + (int64_t(x) - 1) * grid.spacing,
                                                                  grid.z0 + (int64_t(z) - 1) * grid.spacing);
        }
    };
    if (parallel) job_system().parallel_for(JobTier::background, apron_depth, 8, rows);
    else rows(0, apron_depth);
    if (context && context->cancelled()) return false;
    heights.resize(size_t(grid.width) * grid.depth);
    normals.resize(heights.size());
    low = std::numeric_limits<float>::infinity();
    high = -low;
    const auto at = [&](uint32_t x, uint32_t z) { return apron[size_t(z) * apron_width + x]; };
    const auto twice = 2.0f * float(grid.spacing);
    for (uint32_t z = 0; z < grid.depth; ++z)
        for (uint32_t x = 0; x < grid.width; ++x) {
            const auto h = at(x + 1, z + 1);
            heights[size_t(z) * grid.width + x] = h;
            low = std::min(low, h);
            high = std::max(high, h);
            // The surface's normal from central differences: (-dh/dx, 1, -dh/dz), normalized.
            const auto nx = (at(x, z + 1) - at(x + 2, z + 1)) / twice, nz = (at(x + 1, z) - at(x + 1, z + 2)) / twice;
            const auto length = std::sqrt(nx * nx + 1.0f + nz * nz);
            normals[size_t(z) * grid.width + x] = oct_encode(nx / length, 1.0f / length, nz / length);
        }
    return true;
}

template<class T> void put(std::vector<std::byte>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
template<class T> void put_span(std::vector<std::byte>& out, std::span<const T> values) {
    const auto* bytes = reinterpret_cast<const std::byte*>(values.data());
    out.insert(out.end(), bytes, bytes + values.size_bytes());
}
template<class T> bool take(std::span<const std::byte>& in, T& value) {
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&value, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}
template<class T> bool take_span(std::span<const std::byte>& in, std::vector<T>& values, size_t count) {
    if (in.size() < count * sizeof(T)) return false;
    values.resize(count);
    std::memcpy(values.data(), in.data(), count * sizeof(T));
    in = in.subspan(count * sizeof(T));
    return true;
}
constexpr uint32_t tile_magic = 0x31544d48; // "HMT1"
constexpr uint32_t overview_magic = 0x31564d48; // "HMV1"
bool finite_range(std::span<const float> heights, float min, float max) {
    if (!std::isfinite(min) || !std::isfinite(max) || min > max) return false;
    return std::ranges::all_of(heights, [&](float h) { return std::isfinite(h) && h >= min && h <= max; });
}
} // namespace

float noise_height(const HeightfieldNoise& noise, int64_t x, int64_t z) noexcept {
    const auto px = double(x), pz = double(z);
    const auto wx = px + double(noise.warp * fbm(noise, noise.seed ^ 0x77, px, pz));
    const auto wz = pz + double(noise.warp * fbm(noise, noise.seed ^ 0x99, px, pz));
    return noise.base + noise.amplitude * fbm(noise, noise.seed, wx, wz);
}

uint16_t oct_encode(float x, float y, float z) noexcept {
    const auto sum = std::abs(x) + std::abs(y) + std::abs(z);
    auto u = x / sum, v = z / sum;
    if (y < 0.0f) { // the lower hemisphere folds over the diagonals
        const auto fu = (1.0f - std::abs(v)) * (u >= 0.0f ? 1.0f : -1.0f), fv = (1.0f - std::abs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        u = fu;
        v = fv;
    }
    const auto byte = [](float t) { return uint16_t(std::lround(std::clamp(t * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f)); };
    return uint16_t(byte(u) | (byte(v) << 8));
}

std::vector<uint32_t> pack_heights(std::span<const float> heights, std::span<const uint16_t> normals) {
    auto packed = std::vector<uint32_t>(heights.size() * 2);
    for (size_t i = 0; i < heights.size(); ++i) {
        packed[2 * i] = std::bit_cast<uint32_t>(heights[i]);
        packed[2 * i + 1] = normals[i];
    }
    return packed;
}

std::optional<HeightTile> generate_height_tile(const HeightfieldNoise& noise, TileIndex index, const JobContext* context, bool parallel) {
    auto tile = HeightTile{index, {}, {}, 0.0f, 0.0f};
    const auto grid = Grid{int64_t(index.x) * tile_size, int64_t(index.z) * tile_size, 1, tile_samples, tile_samples};
    if (!fill(noise, grid, tile.heights, tile.normals, tile.min, tile.max, context, parallel)) return std::nullopt;
    return tile;
}

std::optional<HeightOverview> generate_height_overview(const HeightfieldNoise& noise, TileIndex low, TileIndex high,
                                                       const JobContext* context, bool parallel) {
    auto overview = HeightOverview{};
    overview.low = low;
    overview.high = high;
    overview.width = uint32_t(high.x - low.x + 1) * uint32_t(tile_size / overview_spacing) + 1;
    overview.depth = uint32_t(high.z - low.z + 1) * uint32_t(tile_size / overview_spacing) + 1;
    const auto grid = Grid{int64_t(low.x) * tile_size, int64_t(low.z) * tile_size, overview_spacing, overview.width, overview.depth};
    if (!fill(noise, grid, overview.heights, overview.normals, overview.min, overview.max, context, parallel)) return std::nullopt;
    return overview;
}

std::vector<std::byte> encode_height_tile(const HeightTile& tile) {
    auto out = std::vector<std::byte>{};
    out.reserve(32 + tile.heights.size() * 6);
    put(out, tile_magic);
    put(out, tile.index.x);
    put(out, tile.index.z);
    put(out, tile.min);
    put(out, tile.max);
    put_span(out, std::span<const float>(tile.heights));
    put_span(out, std::span<const uint16_t>(tile.normals));
    return out;
}

std::optional<HeightTile> decode_height_tile(std::span<const std::byte> in) {
    auto tile = HeightTile{};
    auto magic = uint32_t{0};
    constexpr auto count = size_t(tile_samples) * tile_samples;
    if (!take(in, magic) || magic != tile_magic || !take(in, tile.index.x) || !take(in, tile.index.z) || !take(in, tile.min) ||
        !take(in, tile.max) || !take_span(in, tile.heights, count) || !take_span(in, tile.normals, count) || !in.empty())
        return std::nullopt;
    if (!finite_range(tile.heights, tile.min, tile.max)) return std::nullopt;
    return tile;
}

std::vector<std::byte> encode_height_overview(const HeightOverview& overview) {
    auto out = std::vector<std::byte>{};
    put(out, overview_magic);
    put(out, overview.low.x);
    put(out, overview.low.z);
    put(out, overview.high.x);
    put(out, overview.high.z);
    put(out, overview.min);
    put(out, overview.max);
    put_span(out, std::span<const float>(overview.heights));
    put_span(out, std::span<const uint16_t>(overview.normals));
    return out;
}

std::optional<HeightOverview> decode_height_overview(std::span<const std::byte> in) {
    auto overview = HeightOverview{};
    auto magic = uint32_t{0};
    if (!take(in, magic) || magic != overview_magic || !take(in, overview.low.x) || !take(in, overview.low.z) ||
        !take(in, overview.high.x) || !take(in, overview.high.z) || !take(in, overview.min) || !take(in, overview.max))
        return std::nullopt;
    if (overview.high.x < overview.low.x || overview.high.z < overview.low.z) return std::nullopt;
    const auto cells_x = uint64_t(int64_t(overview.high.x) - overview.low.x + 1), cells_z = uint64_t(int64_t(overview.high.z) - overview.low.z + 1);
    constexpr auto per_cell = uint64_t(tile_size / overview_spacing);
    if (cells_x > 4096 || cells_z > 4096) return std::nullopt; // 512 km a side
    overview.width = uint32_t(cells_x * per_cell + 1);
    overview.depth = uint32_t(cells_z * per_cell + 1);
    const auto count = size_t(overview.width) * overview.depth;
    if (!take_span(in, overview.heights, count) || !take_span(in, overview.normals, count) || !in.empty()) return std::nullopt;
    if (!finite_range(overview.heights, overview.min, overview.max)) return std::nullopt;
    return overview;
}

} // namespace maya
