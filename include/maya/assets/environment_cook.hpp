#pragma once
// Environments for image-based lighting (#1035, docs/assets.md#environments): an HDR image of the
// surroundings, cooked on the CPU into what the renderer samples, as the rendering and content record
// chose. No GPU compute is used.

#include "maya/math/vector.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace maya {
/// An environment file (`.environment`): `maya-environment 1`, then one `key value` line per setting.
struct EnvironmentSettings {
    std::filesystem::path source; // a Radiance .hdr, equirectangular (2:1), at or below the file's folder
    uint32_t specular_size = 128; // texels per side of the prefiltered cube's first level: 16 to 512, a power of two
    uint32_t samples = 256; // importance samples per prefiltered texel: 16 to 4096
};
struct EnvironmentSettingsResult {
    EnvironmentSettings settings;
    std::string error; // "line N: ..." when the file is invalid
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Reads `maya-environment 1`; `source` is required, the others default. Each key appears at most once.
EnvironmentSettingsResult read_environment_settings(std::istream& input);
void write_environment_settings(std::ostream& output, const EnvironmentSettings& settings);

/// Linear RGB radiance, rows top to bottom, as floats.
struct HdrImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgb;
};
struct HdrImageResult {
    HdrImage image;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Decodes a Radiance .hdr file with stb_image. It must be equirectangular (twice as wide as tall)
/// and at most `max_dimension` wide; negative or nonfinite values are read as 0.
HdrImageResult decode_hdr_image(std::span<const std::byte> file, uint32_t max_dimension = 8192);

/// Equirectangular mapping (docs/renderer.md#environments): the image's centre (u 0.5) lies along -Z,
/// the default view direction, u grows toward +X, and v runs from straight up (0) to straight down (1).
math::Vec2 equirect_uv(const math::Vec3& direction) noexcept;
math::Vec3 equirect_direction(float u, float v) noexcept;
/// The direction through a cube face's texel, Metal's convention: faces +X, -X, +Y, -Y, +Z, -Z,
/// s and t in [-1, 1] from the face's left and top.
math::Vec3 cube_direction(uint32_t face, float s, float t) noexcept;

/// What the renderer samples. Every texture is RGBA16F (`half` RGBA), tightly packed as texture_bytes
/// expects.
struct CookedEnvironment {
    /// Irradiance as 9 spherical-harmonic coefficients (l <= 2), already convolved with the cosine lobe:
    /// irradiance(n) = sum of coefficient[i] x Y_i(n), so a diffuse surface reflects albedo x irradiance / pi.
    std::array<math::Vec3, 9> irradiance{};
    uint32_t background_width = 0, background_height = 0, background_levels = 0; // the source, with mips
    std::vector<std::byte> background;
    /// GGX-prefiltered radiance: level L is for perceptual roughness L / (levels - 1); level 0 mirrors.
    uint32_t specular_size = 0, specular_levels = 0;
    std::vector<std::byte> specular;
    double milliseconds = 0; // cooking time, decoding excluded
};
/// Cooks an environment on `threads` threads (0: every core). Deterministic: the same image and
/// settings give the same bytes, whatever the thread count.
CookedEnvironment cook_environment(const HdrImage& image, const EnvironmentSettings& settings, unsigned threads = 0);
/// The irradiance the coefficients give for a direction (the shader's formula).
math::Vec3 evaluate_irradiance(const std::array<math::Vec3, 9>& irradiance, const math::Vec3& normal) noexcept;

/// The split-sum table: for N.V (x, left to right) and perceptual roughness (y, top to bottom), from 0 to 1
/// at the first and last texels' centres, the scale and bias that turn F0 into the GGX specular's directional albedo,
/// F0 x scale + bias, with height-correlated Smith visibility. RGBA16F, scale in red, bias in green.
inline constexpr uint32_t brdf_table_size = 64;
std::vector<std::byte> brdf_table(uint32_t size = brdf_table_size, uint32_t samples = 512);
/// One entry of the table, computed directly.
std::array<double, 2> brdf_scale_bias(double NdotV, double roughness, uint32_t samples = 512) noexcept;
} // namespace maya
