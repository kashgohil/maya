#pragma once
#include "maya/assets/texture_data.hpp"
#include "maya/jobs/jobs.hpp"

namespace maya {
/// A decoded source image as straight-alpha RGBA8 rows, exactly as stored in the file: no color
/// management or premultiplication.
struct SourceImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};
struct SourceImageResult {
    SourceImage image;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Decodes a PNG or JPEG file's bytes with stb_image. stb_image is not hardened against hostile
/// files; cook only the project's own content. Images above `max_dimension` are refused before
/// their pixels are decoded.
SourceImageResult decode_image(std::span<const std::byte> file, uint32_t max_dimension = 16384);

struct TextureCookOptions {
    bool astc = true; // when false, ASTC settings cook to RGBA8 (the device cannot sample ASTC)
    unsigned threads = 0; // ASTC compression threads: 1 is the calling thread alone; 0 is the tier's workers and the caller
    JobTier tier = JobTier::frame; // the job system tier that compresses; frame while a caller waits (docs/jobs.md#cooking)
};
struct TextureCookResult {
    TextureImage image;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Builds the GPU image a texture's settings describe: mips from stb_image_resize2 (sRGB-correct for
/// color, linear for data, renormalized for normals), then ASTC from astcenc at medium quality or
/// RGBA8. The same source, settings, and astc option always give the same bytes.
TextureCookResult cook_texture(const SourceImage& source, const TextureSettings& settings, const TextureCookOptions& options = {});
} // namespace maya
