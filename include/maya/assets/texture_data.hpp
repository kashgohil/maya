#pragma once
#include "maya/rhi/resource.hpp"
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace maya {
/// What a texture's texels mean. Color is sRGB-encoded; data and normal maps are linear. Normal maps
/// are stored with x in red, green, and blue and y in alpha (astcenc's normal layout, kept for RGBA8
/// too); shaders rebuild z = sqrt(1 - x² - y²) from tangent-space x and y in [-1, 1].
enum class TextureRole : uint8_t { color, data, normal };
/// The cooked GPU format family: ASTC (6x6 for color and data, 4x4 for normals) or uncompressed RGBA8.
enum class TextureCompression : uint8_t { astc, rgba8 };
const char* texture_role_name(TextureRole role) noexcept; // "color", "data", "normal"
const char* texture_compression_name(TextureCompression compression) noexcept; // "astc", "rgba8"
/// The GPU format a role and compression cook to.
Format texture_format(TextureRole role, TextureCompression compression) noexcept;

/// A texture descriptor file (`.texture`, docs/assets.md#textures): the source image and every setting.
/// Nothing is guessed from file names or contents.
struct TextureSettings {
    std::filesystem::path source; // relative to the descriptor's folder, at or below it
    TextureRole role = TextureRole::color;
    TextureCompression compression = TextureCompression::astc;
    bool mips = true; // a full chain to 1x1; off keeps level 0 only and samples it without mip filtering
    SamplerDesc sampler{Filter::linear, Filter::linear, AddressMode::repeat, AddressMode::repeat, {}, MipFilter::linear, 8};
};
struct TextureSettingsResult {
    TextureSettings settings;
    std::string error; // "line N: ..." when the file is invalid
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Reads `maya-texture 1`: one `key value...` line per setting, each exactly once, in any order.
/// Errors start with "line N: " when they belong to a line.
TextureSettingsResult read_texture_settings(std::istream& input);
/// Writes every setting in a fixed order; read_texture_settings reads it back unchanged.
void write_texture_settings(std::ostream& output, const TextureSettings& settings);

/// Texels ready for upload: every mip level, level 0 first, each tightly packed rows of pixels (or of
/// blocks), texture_bytes(desc()) in all.
struct TextureImage {
    Format format = Format::undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
    std::vector<std::byte> data;
    TextureDesc desc(std::string label = {}) const {
        return {width, height, format, TextureUsage::sampled, std::move(label), mip_levels};
    }
};

/// KTX2 (Khronos KTX 2.0) as Maya writes and reads it: one 2D image with its mip levels, no array
/// layers, cube faces, or supercompression, in RGBA8 (sRGB or linear) or ASTC 4x4/6x6 LDR.
bool ktx2_supports(Format format) noexcept;
struct Ktx2ReadResult {
    TextureImage image;
    std::string error; // why the file was refused
    explicit operator bool() const noexcept { return error.empty(); }
};
inline constexpr uint32_t ktx2_max_dimension = 16384; // the RHI's largest texture
/// Checks the identifier, header, level index, and data format descriptor against the file's size
/// before allocating or reading any texels; a refused file names the first problem found. Images
/// larger than ktx2_max_dimension on a side are refused.
Ktx2ReadResult read_ktx2(std::span<const std::byte> file);
/// The bytes of a KTX2 file holding `image`, with a KTXwriter entry naming `writer`. The image must
/// be complete (texture_bytes of its levels) in a supported format.
std::vector<std::byte> write_ktx2(const TextureImage& image, std::string_view writer = "Maya");
} // namespace maya
