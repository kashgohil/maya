// Writes one KTX2 file for each format and layout Maya writes, for Khronos's `ktx validate`
// (cmake/validate_ktx2.cmake). Usage: maya_ktx2_samples <output folder>
#include "maya/assets/texture_cook.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace maya;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: maya_ktx2_samples <output folder>\n");
        return 2;
    }
    const auto folder = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(folder);
    // An odd-sized source, so levels and blocks do not divide evenly.
    auto source = SourceImage{37, 23, std::vector<uint8_t>(37 * 23 * 4)};
    for (uint32_t i = 0; i < 37 * 23; ++i) {
        source.rgba[i * 4] = uint8_t(i * 7), source.rgba[i * 4 + 1] = uint8_t(i * 13);
        source.rgba[i * 4 + 2] = uint8_t(255 - i), source.rgba[i * 4 + 3] = uint8_t(128 + i % 128);
    }
    const auto cook = [&](TextureRole role, TextureCompression compression, bool mips) {
        auto settings = TextureSettings{};
        settings.role = role;
        settings.compression = compression;
        settings.mips = mips;
        auto cooked = cook_texture(source, settings, {true, 1});
        if (!cooked) {
            std::fprintf(stderr, "cooking failed: %s\n", cooked.error.c_str());
            std::exit(1);
        }
        return cooked.image;
    };
    auto astc_4x4_srgb = cook(TextureRole::normal, TextureCompression::astc, true);
    astc_4x4_srgb.format = Format::astc_4x4_srgb; // the same blocks, declared sRGB: no role cooks to it
    const std::pair<const char*, TextureImage> files[] = {
        {"rgba8_srgb_mips.ktx2", cook(TextureRole::color, TextureCompression::rgba8, true)},
        {"rgba8_unorm_mips.ktx2", cook(TextureRole::data, TextureCompression::rgba8, true)},
        {"rgba8_srgb_single.ktx2", cook(TextureRole::color, TextureCompression::rgba8, false)},
        {"astc_6x6_srgb_mips.ktx2", cook(TextureRole::color, TextureCompression::astc, true)},
        {"astc_6x6_unorm_mips.ktx2", cook(TextureRole::data, TextureCompression::astc, true)},
        {"astc_4x4_unorm_mips.ktx2", cook(TextureRole::normal, TextureCompression::astc, true)},
        {"astc_4x4_srgb_mips.ktx2", astc_4x4_srgb},
    };
    for (const auto& [name, image] : files) {
        const auto bytes = write_ktx2(image, "Maya ktx2 samples");
        auto out = std::ofstream(folder / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) return 1;
        std::printf("%s\n", name);
    }
    return 0;
}
