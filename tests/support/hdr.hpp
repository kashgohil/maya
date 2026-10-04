#pragma once
// Environments made in memory for tests (#1035): an equirectangular radiance image from a function of
// direction, as floats or as a Radiance .hdr file's bytes.

#include "maya/assets/environment_cook.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace maya::test {

/// width x width/2 texels, each the function's radiance along its centre's direction.
inline HdrImage environment_image(uint32_t width, const std::function<math::Vec3(const math::Vec3&)>& radiance) {
    auto image = HdrImage{width, width / 2, {}};
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x) {
            const auto value = radiance(equirect_direction((x + 0.5f) / float(image.width), (y + 0.5f) / float(image.height)));
            image.rgb.insert(image.rgb.end(), {value.x, value.y, value.z});
        }
    return image;
}
inline HdrImage uniform_environment(float radiance, uint32_t width = 64) {
    return environment_image(width, [&](const math::Vec3&) { return math::Vec3{radiance}; });
}

/// A Radiance file of the image: flat (not run-length encoded) RGBE scanlines, which stb_image reads.
/// RGBE keeps about 8 bits of mantissa per channel.
inline std::string radiance_file(const HdrImage& image) {
    auto text = std::string("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(image.height) + " +X " +
                            std::to_string(image.width) + "\n");
    for (size_t i = 0; i < image.rgb.size(); i += 3) {
        const auto largest = std::max({image.rgb[i], image.rgb[i + 1], image.rgb[i + 2]});
        if (largest < 1e-32f) {
            text.append(4, '\0');
            continue;
        }
        int exponent = 0;
        const auto scale = std::frexp(largest, &exponent) * 256.0f / largest;
        for (int c = 0; c < 3; ++c) text.push_back(char(uint8_t(image.rgb[i + c] * scale)));
        text.push_back(char(uint8_t(exponent + 128)));
    }
    return text;
}

} // namespace maya::test
