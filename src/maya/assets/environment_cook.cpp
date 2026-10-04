#include "maya/assets/environment_cook.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <istream>
#include <limits>
#include <numbers>
#include <ostream>
#include <sstream>
#include <stb_image.h>
#include <thread>

namespace maya {
namespace {
constexpr auto pi = std::numbers::pi_v<double>;

bool power_of_two(uint32_t value) { return value && (value & (value - 1)) == 0; }

/// A float RGB image and its mip chain (2 x 2 box filtered), sampled bilinearly, wrapping in u.
struct Equirect {
    struct Level {
        uint32_t width = 0, height = 0;
        std::vector<float> rgb;
    };
    std::vector<Level> levels;

    explicit Equirect(const HdrImage& image) {
        levels.push_back({image.width, image.height, image.rgb});
        while (levels.back().width > 1 || levels.back().height > 1) {
            const auto& from = levels.back();
            auto next = Level{std::max(from.width / 2, 1u), std::max(from.height / 2, 1u), {}};
            next.rgb.resize(size_t(next.width) * next.height * 3);
            for (uint32_t y = 0; y < next.height; ++y)
                for (uint32_t x = 0; x < next.width; ++x)
                    for (uint32_t c = 0; c < 3; ++c) {
                        auto sum = 0.0f;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx) {
                                const auto sx = std::min(x * 2 + dx, from.width - 1), sy = std::min(y * 2 + dy, from.height - 1);
                                sum += from.rgb[(size_t(sy) * from.width + sx) * 3 + c];
                            }
                        next.rgb[(size_t(y) * next.width + x) * 3 + c] = sum * 0.25f;
                    }
            levels.push_back(std::move(next));
        }
    }
    std::array<float, 3> bilinear(const Level& level, float u, float v) const {
        const auto x = u * float(level.width) - 0.5f, y = std::clamp(v * float(level.height) - 0.5f, 0.0f, float(level.height - 1));
        const auto x0 = std::floor(x), y0 = std::floor(y);
        const auto fx = x - x0, fy = y - y0;
        const auto wrap = [&](float column) {
            const auto w = int64_t(level.width);
            return uint32_t(((int64_t(column) % w) + w) % w);
        };
        const uint32_t xs[2] = {wrap(x0), wrap(x0 + 1)};
        const uint32_t ys[2] = {uint32_t(y0), std::min(uint32_t(y0) + 1, level.height - 1)};
        auto out = std::array<float, 3>{};
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const auto weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const auto* p = &level.rgb[(size_t(ys[j]) * level.width + xs[i]) * 3];
                for (int c = 0; c < 3; ++c) out[c] += weight * p[c];
            }
        return out;
    }
    /// Radiance along a direction, at a fractional mip level.
    std::array<float, 3> sample(const math::Vec3& direction, float lod) const {
        const auto uv = equirect_uv(direction);
        lod = std::clamp(lod, 0.0f, float(levels.size() - 1));
        const auto low = size_t(lod);
        const auto high = std::min(low + 1, levels.size() - 1);
        const auto t = lod - float(low);
        const auto a = bilinear(levels[low], uv.x, uv.y), b = bilinear(levels[high], uv.x, uv.y);
        return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
    }
};

void put_half(std::byte* out, const std::array<float, 4>& rgba) {
    for (int c = 0; c < 4; ++c) {
        const auto value = _Float16(std::clamp(std::isfinite(rgba[c]) ? rgba[c] : 0.0f, 0.0f, 65504.0f));
        std::memcpy(out + c * 2, &value, 2);
    }
}

std::array<double, 2> hammersley(uint32_t i, uint32_t count) {
    auto bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return {double(i) / double(count), double(bits) * 2.3283064365386963e-10};
}
/// A GGX-distributed half vector around +Z.
std::array<double, 3> ggx_half(const std::array<double, 2>& xi, double alpha) {
    const auto phi = 2.0 * pi * xi[0];
    const auto cos_theta = std::sqrt((1.0 - xi[1]) / (1.0 + (alpha * alpha - 1.0) * xi[1]));
    const auto sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
    return {sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta};
}
double smith_visibility(double NdotL, double NdotV, double alpha) {
    const auto a2 = alpha * alpha;
    const auto v = NdotL * std::sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const auto l = NdotV * std::sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return v + l > 0.0 ? 0.5 / (v + l) : 0.0;
}
// Real spherical harmonics, l <= 2, in the renderer's world axes.
std::array<double, 9> sh_basis(double x, double y, double z) {
    return {0.282095, 0.488603 * y, 0.488603 * z, 0.488603 * x, 1.092548 * x * y, 1.092548 * y * z,
            0.315392 * (3.0 * z * z - 1.0), 1.092548 * x * z, 0.546274 * (x * x - y * y)};
}

template<class Work> void parallel(uint32_t tasks, unsigned threads, Work&& work) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min(threads, std::max(tasks, 1u));
    auto next = std::atomic<uint32_t>{0};
    const auto run = [&] {
        for (auto task = next++; task < tasks; task = next++) work(task);
    };
    auto pool = std::vector<std::thread>{};
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(run);
    run();
    for (auto& thread : pool) thread.join();
}
} // namespace

EnvironmentSettingsResult read_environment_settings(std::istream& input) {
    auto result = EnvironmentSettingsResult{};
    const auto fail = [&](size_t line, std::string why) {
        result = {};
        result.error = (line ? "line " + std::to_string(line) + ": " : std::string{}) + std::move(why);
        return result;
    };
    auto text = std::string{};
    if (!std::getline(input, text) || text != "maya-environment 1")
        return fail(1, "expected 'maya-environment 1'");
    auto seen = std::vector<std::string>{};
    auto has_source = false;
    for (size_t line = 2; std::getline(input, text); ++line) {
        auto words = std::istringstream(text);
        auto key = std::string{};
        if (!(words >> key)) continue;
        if (std::ranges::find(seen, key) != seen.end()) return fail(line, "'" + key + "' appears more than once");
        seen.push_back(key);
        auto extra = std::string{};
        if (key == "source") {
            auto path = std::string{};
            if (!(words >> std::quoted(path)) || path.empty() || words >> extra)
                return fail(line, "source needs one quoted file name");
            result.settings.source = path;
            has_source = true;
        } else if (key == "specular_size") {
            auto value = int64_t{};
            if (!(words >> value) || words >> extra || value < 16 || value > 512 || !power_of_two(uint32_t(value)))
                return fail(line, "specular_size must be a power of two from 16 to 512");
            result.settings.specular_size = uint32_t(value);
        } else if (key == "samples") {
            auto value = int64_t{};
            if (!(words >> value) || words >> extra || value < 16 || value > 4096)
                return fail(line, "samples must be a whole number from 16 to 4096");
            result.settings.samples = uint32_t(value);
        } else {
            return fail(line, "unknown setting '" + key + "'; use source, specular_size, or samples");
        }
    }
    if (input.bad()) return fail(0, "cannot read the environment file");
    if (!has_source) return fail(0, "the environment file names no source");
    return result;
}

void write_environment_settings(std::ostream& output, const EnvironmentSettings& settings) {
    output << "maya-environment 1\nsource " << std::quoted(settings.source.generic_string()) << "\nspecular_size "
           << settings.specular_size << "\nsamples " << settings.samples << '\n';
}

HdrImageResult decode_hdr_image(std::span<const std::byte> file, uint32_t max_dimension) {
    auto result = HdrImageResult{};
    if (file.size() > size_t(std::numeric_limits<int>::max())) {
        result.error = "the file is too large to decode";
        return result;
    }
    const auto* bytes = reinterpret_cast<const stbi_uc*>(file.data());
    const auto length = int(file.size());
    int width = 0, height = 0, channels = 0;
    if (!stbi_is_hdr_from_memory(bytes, length) || !stbi_info_from_memory(bytes, length, &width, &height, &channels)) {
        result.error = "not a Radiance .hdr image";
        return result;
    }
    if (width <= 0 || height <= 0 || uint32_t(width) > max_dimension) {
        result.error = "the image is " + std::to_string(width) + " wide; environments are at most " + std::to_string(max_dimension);
        return result;
    }
    if (width != 2 * height) {
        result.error = "the image is " + std::to_string(width) + "x" + std::to_string(height) +
                       "; an equirectangular environment is twice as wide as it is tall";
        return result;
    }
    auto* pixels = stbi_loadf_from_memory(bytes, length, &width, &height, &channels, 3);
    if (!pixels) {
        result.error = std::string("cannot decode the image: ") + stbi_failure_reason();
        return result;
    }
    result.image = {uint32_t(width), uint32_t(height), std::vector<float>(pixels, pixels + size_t(width) * height * 3)};
    stbi_image_free(pixels);
    for (auto& value : result.image.rgb)
        if (!std::isfinite(value) || value < 0.0f) value = 0.0f;
    return result;
}

math::Vec2 equirect_uv(const math::Vec3& d) noexcept {
    const auto length = std::sqrt(double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z);
    if (!(length > 0.0)) return {0.5f, 0.5f};
    const auto phi = std::atan2(double(d.x), -double(d.z));
    const auto theta = std::acos(std::clamp(double(d.y) / length, -1.0, 1.0));
    return {float(0.5 + phi / (2.0 * pi)), float(theta / pi)};
}

math::Vec3 equirect_direction(float u, float v) noexcept {
    const auto phi = (double(u) - 0.5) * 2.0 * pi, theta = double(v) * pi;
    return {float(std::sin(theta) * std::sin(phi)), float(std::cos(theta)), float(-std::sin(theta) * std::cos(phi))};
}

math::Vec3 cube_direction(uint32_t face, float s, float t) noexcept {
    auto d = math::Vec3{};
    switch (face) {
    case 0: d = {1, -t, -s}; break;
    case 1: d = {-1, -t, s}; break;
    case 2: d = {s, 1, t}; break;
    case 3: d = {s, -1, -t}; break;
    case 4: d = {s, -t, 1}; break;
    default: d = {-s, -t, -1}; break;
    }
    return d.normalized();
}

math::Vec3 evaluate_irradiance(const std::array<math::Vec3, 9>& c, const math::Vec3& n) noexcept {
    const auto y = sh_basis(n.x, n.y, n.z);
    auto e = math::Vec3{0.0f};
    for (size_t i = 0; i < 9; ++i) e += c[i] * float(y[i]);
    return {std::max(e.x, 0.0f), std::max(e.y, 0.0f), std::max(e.z, 0.0f)};
}

CookedEnvironment cook_environment(const HdrImage& image, const EnvironmentSettings& settings, unsigned threads) {
    const auto start = std::chrono::steady_clock::now();
    auto cooked = CookedEnvironment{};
    const auto source = Equirect(image);

    // The background: the source and its mips, as half floats.
    cooked.background_width = image.width;
    cooked.background_height = image.height;
    cooked.background_levels = uint32_t(source.levels.size());
    for (const auto& level : source.levels) {
        const auto offset = cooked.background.size();
        cooked.background.resize(offset + size_t(level.width) * level.height * 8);
        for (size_t i = 0; i < size_t(level.width) * level.height; ++i)
            put_half(cooked.background.data() + offset + i * 8, {level.rgb[i * 3], level.rgb[i * 3 + 1], level.rgb[i * 3 + 2], 1.0f});
    }

    // Irradiance: project radiance onto the basis, weighting each texel by its solid angle, then
    // convolve with the cosine lobe (pi, 2 pi / 3, pi / 4 by band).
    auto radiance = std::array<std::array<double, 3>, 9>{};
    for (uint32_t j = 0; j < image.height; ++j) {
        const auto v = (double(j) + 0.5) / image.height;
        const auto solid_angle = (2.0 * pi / image.width) * (pi / image.height) * std::sin(v * pi);
        for (uint32_t i = 0; i < image.width; ++i) {
            const auto d = equirect_direction(float((double(i) + 0.5) / image.width), float(v));
            const auto y = sh_basis(d.x, d.y, d.z);
            const auto* p = &image.rgb[(size_t(j) * image.width + i) * 3];
            for (size_t k = 0; k < 9; ++k)
                for (int c = 0; c < 3; ++c) radiance[k][c] += double(p[c]) * y[k] * solid_angle;
        }
    }
    constexpr double band[9] = {pi, 2 * pi / 3, 2 * pi / 3, 2 * pi / 3, pi / 4, pi / 4, pi / 4, pi / 4, pi / 4};
    for (size_t k = 0; k < 9; ++k)
        cooked.irradiance[k] = {float(radiance[k][0] * band[k]), float(radiance[k][1] * band[k]), float(radiance[k][2] * band[k])};

    // Specular: each level's texels are the GGX-weighted radiance around their direction (N = V = R),
    // from importance samples that read the source at a mip matching their solid angle (Karis 2013).
    cooked.specular_size = settings.specular_size;
    cooked.specular_levels = std::min(6u, uint32_t(std::bit_width(settings.specular_size)) - 1u);
    auto offsets = std::vector<size_t>{};
    auto total = size_t{0};
    for (uint32_t level = 0; level < cooked.specular_levels; ++level) {
        offsets.push_back(total);
        const auto size = settings.specular_size >> level;
        total += size_t(size) * size * 8 * 6;
    }
    cooked.specular.resize(total);
    const auto texel_solid_angle = 4.0 * pi / (double(image.width) * image.height);
    const auto mirror_lod = float(std::max(0.0, std::log2(double(image.width) / (4.0 * settings.specular_size))));
    parallel(cooked.specular_levels * 6, threads, [&](uint32_t task) {
        const auto level = task / 6, face = task % 6;
        const auto size = settings.specular_size >> level;
        const auto roughness = cooked.specular_levels > 1 ? double(level) / double(cooked.specular_levels - 1) : 0.0;
        const auto alpha = roughness * roughness;
        auto* out = cooked.specular.data() + offsets[level] + size_t(face) * size * size * 8;
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                const auto n = cube_direction(face, float(2.0 * (x + 0.5) / size - 1.0), float(2.0 * (y + 0.5) / size - 1.0));
                auto rgb = std::array<double, 3>{};
                if (level == 0) {
                    const auto s = source.sample(n, mirror_lod);
                    rgb = {s[0], s[1], s[2]};
                } else {
                    const auto up = std::abs(n.y) < 0.999f ? math::Vec3{0, 1, 0} : math::Vec3{1, 0, 0};
                    const auto tangent = math::Vec3::cross(up, n).normalized();
                    const auto bitangent = math::Vec3::cross(n, tangent);
                    auto weight = 0.0;
                    for (uint32_t i = 0; i < settings.samples; ++i) {
                        const auto h = ggx_half(hammersley(i, settings.samples), alpha);
                        const auto H = tangent * float(h[0]) + bitangent * float(h[1]) + n * float(h[2]);
                        const auto NdotH = h[2];
                        const auto L = H * float(2.0 * NdotH) - n;
                        const auto NdotL = double(math::Vec3::dot(n, L));
                        if (NdotL <= 0.0) continue;
                        const auto a2 = alpha * alpha;
                        const auto d = NdotH * NdotH * (a2 - 1.0) + 1.0;
                        const auto pdf = a2 / (pi * d * d) / 4.0; // D x NdotH / (4 VdotH), with N = V
                        const auto sample_solid_angle = 1.0 / (double(settings.samples) * pdf + 1e-12);
                        const auto lod = float(std::max(0.0, 0.5 * std::log2(sample_solid_angle / texel_solid_angle) + 1.0));
                        const auto s = source.sample(L, lod);
                        for (int c = 0; c < 3; ++c) rgb[c] += double(s[c]) * NdotL;
                        weight += NdotL;
                    }
                    for (auto& c : rgb) c = weight > 0.0 ? c / weight : 0.0;
                }
                put_half(out + (size_t(y) * size + x) * 8, {float(rgb[0]), float(rgb[1]), float(rgb[2]), 1.0f});
            }
    });
    cooked.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return cooked;
}

std::array<double, 2> brdf_scale_bias(double NdotV, double roughness, uint32_t samples) noexcept {
    NdotV = std::clamp(NdotV, 1e-4, 1.0);
    const auto alpha = roughness * roughness;
    const auto V = std::array<double, 3>{std::sqrt(1.0 - NdotV * NdotV), 0.0, NdotV};
    auto scale = 0.0, bias = 0.0;
    for (uint32_t i = 0; i < samples; ++i) {
        const auto H = ggx_half(hammersley(i, samples), alpha);
        const auto VdotH = V[0] * H[0] + V[1] * H[1] + V[2] * H[2];
        const auto NdotL = 2.0 * VdotH * H[2] - V[2];
        if (NdotL <= 0.0 || VdotH <= 0.0) continue;
        // The specular's value over the half vector's probability, without F: Vis x 4 NdotL VdotH / NdotH.
        const auto g = smith_visibility(NdotL, NdotV, alpha) * 4.0 * NdotL * VdotH / std::max(H[2], 1e-8);
        const auto fc = std::pow(1.0 - VdotH, 5.0);
        scale += (1.0 - fc) * g;
        bias += fc * g;
    }
    return {scale / samples, bias / samples};
}

std::vector<std::byte> brdf_table(uint32_t size, uint32_t samples) {
    auto table = std::vector<std::byte>(size_t(size) * size * 8);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            // A grid that includes both ends, so N.V 1 and roughness 1 are exact; the shader samples texel centres.
            const auto [scale, bias] = brdf_scale_bias(double(x) / (size - 1), double(y) / (size - 1), samples);
            put_half(table.data() + (size_t(y) * size + x) * 8, {float(scale), float(bias), 0.0f, 1.0f});
        }
    return table;
}
} // namespace maya
