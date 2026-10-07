#include "maya/assets/texture_cook.hpp"
#include "maya/jobs/jobs.hpp"
#include <astcenc.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace maya {
namespace {
/// Tangent-space normals from 8-bit RGB, renormalized; a zero vector becomes straight up.
void renormalize(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        auto x = rgba[i] / 127.5f - 1.0f, y = rgba[i + 1] / 127.5f - 1.0f, z = rgba[i + 2] / 127.5f - 1.0f;
        const auto length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-6f) x = 0.0f, y = 0.0f, z = 1.0f;
        else x /= length, y /= length, z /= length;
        const auto encode = [](float value) { return static_cast<uint8_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.5f + 127.5f)); };
        rgba[i] = encode(x);
        rgba[i + 1] = encode(y);
        rgba[i + 2] = encode(z);
        rgba[i + 3] = 255;
    }
}

struct Level {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

/// Halves `previous` (sRGB-correct for color with alpha-weighted color, every channel independent
/// otherwise) into the next level.
Level downsample(const Level& previous, TextureRole role) {
    auto next = Level{std::max(1u, previous.width / 2), std::max(1u, previous.height / 2), {}};
    next.rgba.resize(size_t{next.width} * next.height * 4);
    if (role == TextureRole::color)
        stbir_resize_uint8_srgb(previous.rgba.data(), int(previous.width), int(previous.height), 0, next.rgba.data(), int(next.width),
                                int(next.height), 0, STBIR_RGBA);
    else
        stbir_resize_uint8_linear(previous.rgba.data(), int(previous.width), int(previous.height), 0, next.rgba.data(), int(next.width),
                                  int(next.height), 0, STBIR_4CHANNEL);
    if (role == TextureRole::normal) renormalize(next.rgba);
    return next;
}

struct ContextDeleter {
    void operator()(astcenc_context* context) const noexcept { astcenc_context_free(context); }
};

/// Compresses every level with one astcenc context, `threads` workers per level.
std::string compress_astc(const std::vector<Level>& levels, Format format, TextureRole role, unsigned threads, JobTier tier, std::vector<std::byte>& out) {
    const auto block = block_extent(format);
    const auto profile = is_srgb_format(format) ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    const auto flags = role == TextureRole::normal ? ASTCENC_FLG_MAP_NORMAL : 0u;
    auto config = astcenc_config{};
    if (const auto status = astcenc_config_init(profile, block, block, 1, ASTCENC_PRE_MEDIUM, flags, &config); status != ASTCENC_SUCCESS)
        return std::string("astcenc rejected the configuration: ") + astcenc_get_error_string(status);
    astcenc_context* raw = nullptr;
    if (const auto status = astcenc_context_alloc(&config, threads, &raw, nullptr); status != ASTCENC_SUCCESS)
        return std::string("astcenc could not start: ") + astcenc_get_error_string(status);
    const auto context = std::unique_ptr<astcenc_context, ContextDeleter>(raw);
    // Normal maps are stored as x in red, green, and blue and y in alpha, as astcenc's normal mode expects.
    const auto swizzle = role == TextureRole::normal ? astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_G}
                                                     : astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    for (const auto& level : levels) {
        void* slices[] = {const_cast<uint8_t*>(level.rgba.data())};
        auto image = astcenc_image{level.width, level.height, 1, ASTCENC_TYPE_U8, slices};
        const auto bytes = mip_level_bytes(format, level.width, level.height, 0);
        const auto start = out.size();
        out.resize(start + bytes);
        auto* destination = reinterpret_cast<uint8_t*>(out.data() + start);
        // astcenc's threads claim blocks as they arrive, so its thread indices can run on however many pool
        // threads are free; each index runs once, and the last to finish has compressed every block.
        auto statuses = std::vector<astcenc_error>(threads, ASTCENC_SUCCESS);
        const auto compress = [&](uint32_t first, uint32_t last) {
            for (auto t = first; t < last; ++t) statuses[t] = astcenc_compress_image(context.get(), &image, &swizzle, destination, bytes, t);
        };
        if (threads == 1) compress(0, 1);
        else job_system().parallel_for(tier, threads, 1, compress);
        for (const auto status : statuses)
            if (status != ASTCENC_SUCCESS) return std::string("astcenc failed: ") + astcenc_get_error_string(status);
        astcenc_compress_reset(context.get());
    }
    return {};
}
} // namespace

SourceImageResult decode_image(std::span<const std::byte> file, uint32_t max_dimension) {
    auto result = SourceImageResult{};
    const auto* bytes = reinterpret_cast<const stbi_uc*>(file.data());
    const auto length = static_cast<int>(std::min<size_t>(file.size(), size_t{std::numeric_limits<int>::max()}));
    if (file.size() != size_t(length)) {
        result.error = "the file is too large to decode";
        return result;
    }
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes, length, &width, &height, &channels)) {
        result.error = std::string("cannot decode the image: ") + stbi_failure_reason();
        return result;
    }
    if (width <= 0 || height <= 0 || uint32_t(width) > max_dimension || uint32_t(height) > max_dimension) {
        result.error = "the image is " + std::to_string(width) + "x" + std::to_string(height) + "; textures can be at most " +
                       std::to_string(max_dimension) + " on each side";
        return result;
    }
    auto* pixels = stbi_load_from_memory(bytes, length, &width, &height, &channels, 4);
    if (!pixels) {
        result.error = std::string("cannot decode the image: ") + stbi_failure_reason();
        return result;
    }
    result.image.width = uint32_t(width);
    result.image.height = uint32_t(height);
    result.image.rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    stbi_image_free(pixels);
    return result;
}

TextureCookResult cook_texture(const SourceImage& source, const TextureSettings& settings, const TextureCookOptions& options) {
    auto result = TextureCookResult{};
    if (source.width == 0 || source.height == 0 || source.rgba.size() != size_t{source.width} * source.height * 4) {
        result.error = "the source image is empty or incomplete";
        return result;
    }
    const auto compression = options.astc ? settings.compression : TextureCompression::rgba8;
    const auto format = texture_format(settings.role, compression);
    const auto count = settings.mips ? full_mip_count(source.width, source.height) : 1u;
    auto levels = std::vector<Level>{};
    levels.reserve(count);
    levels.push_back({source.width, source.height, source.rgba});
    if (settings.role == TextureRole::normal) renormalize(levels.front().rgba);
    while (levels.size() < count) levels.push_back(downsample(levels.back(), settings.role));

    auto& image = result.image;
    image.format = format;
    image.width = source.width;
    image.height = source.height;
    image.mip_levels = count;
    image.data.reserve(texture_bytes(image.desc()));
    if (is_compressed_format(format)) {
        const auto threads = options.threads ? options.threads : unsigned(job_system().workers(options.tier)) + 1;
        if (auto error = compress_astc(levels, format, settings.role, threads, options.tier, image.data); !error.empty()) {
            result.error = std::move(error);
            result.image = {};
        }
        return result;
    }
    for (const auto& level : levels) {
        const auto start = image.data.size();
        image.data.resize(start + level.rgba.size());
        auto* out = reinterpret_cast<uint8_t*>(image.data.data() + start);
        if (settings.role == TextureRole::normal) // x in red, green, and blue; y in alpha, as for ASTC
            for (size_t i = 0; i < level.rgba.size(); i += 4)
                out[i] = out[i + 1] = out[i + 2] = level.rgba[i], out[i + 3] = level.rgba[i + 1];
        else
            std::copy(level.rgba.begin(), level.rgba.end(), out);
    }
    return result;
}
} // namespace maya
