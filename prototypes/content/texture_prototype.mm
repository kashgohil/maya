// Texture and environment prototype for #1030: decoding (stb_image against Apple's ImageIO), sRGB-correct
// mip chains, ASTC compression quality and cost (astcenc), the GPU formats Metal offers, and building an
// environment's diffuse irradiance and prefiltered specular levels on the CPU.
// Usage: maya_texture_prototype <render-samples folder>
#include <astcenc.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

Image decode_stb(const fs::path& file) {
    auto image = Image{};
    int channels = 0;
    if (auto* data = stbi_load(file.c_str(), &image.width, &image.height, &channels, 4)) {
        image.rgba.assign(data, data + size_t(image.width) * image.height * 4);
        stbi_image_free(data);
    }
    return image;
}

/// ImageIO, drawn into an sRGB RGBA8 bitmap: what a macOS-only decoder would give.
Image decode_imageio(const fs::path& file) {
    auto image = Image{};
    @autoreleasepool {
        auto* url = (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:file.c_str()]];
        auto source = CGImageSourceCreateWithURL(url, nullptr);
        if (!source) return image;
        auto cg = CGImageSourceCreateImageAtIndex(source, 0, (__bridge CFDictionaryRef) @{(id)kCGImageSourceShouldCacheImmediately: @YES});
        CFRelease(source);
        if (!cg) return image;
        image.width = int(CGImageGetWidth(cg));
        image.height = int(CGImageGetHeight(cg));
        image.rgba.resize(size_t(image.width) * image.height * 4);
        auto space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        auto context = CGBitmapContextCreate(image.rgba.data(), image.width, image.height, 8, size_t(image.width) * 4, space,
                                             uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
        CGContextDrawImage(context, CGRectMake(0, 0, image.width, image.height), cg);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        CGImageRelease(cg);
    }
    return image;
}

bool is_data(const fs::path& file) { // normal, occlusion-roughness-metallic, and similar maps are linear data
    auto name = file.filename().string();
    std::ranges::transform(name, name.begin(), ::tolower);
    for (const auto* word : {"normal", "orm", "rough", "metal", "occlusion", "specular", "bump"})
        if (name.find(word) != std::string::npos) return true;
    return false;
}

/// A full mip chain below level 0, sRGB-correct for color and linear for data. Returns the bytes.
size_t mip_chain(const Image& image, bool data) {
    auto bytes = size_t{0};
    auto w = image.width, h = image.height;
    auto previous = image.rgba;
    while (w > 1 || h > 1) {
        const auto nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        auto next = std::vector<uint8_t>(size_t(nw) * nh * 4);
        if (data) stbir_resize_uint8_linear(previous.data(), w, h, 0, next.data(), nw, nh, 0, STBIR_RGBA);
        else stbir_resize_uint8_srgb(previous.data(), w, h, 0, next.data(), nw, nh, 0, STBIR_RGBA);
        bytes += next.size();
        previous = std::move(next);
        w = nw;
        h = nh;
    }
    return bytes;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channels_used) {
    auto squared = 0.0;
    auto count = size_t{0};
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < channels_used; ++c) {
            const auto d = double(a[i + c]) - double(b[i + c]);
            squared += d * d;
            ++count;
        }
    const auto mse = squared / double(count);
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// Mean angle, in degrees, between normals from two tangent-space normal maps (x and y stored, z rebuilt).
double normal_error_degrees(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& decoded, bool decoded_in_ra) {
    auto total = 0.0;
    const auto unpack = [](float x, float y) {
        x = x / 127.5f - 1.0f;
        y = y / 127.5f - 1.0f;
        const auto z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
        return std::array<float, 3>{x, y, z};
    };
    for (size_t i = 0; i < reference.size(); i += 4) {
        const auto a = unpack(reference[i], reference[i + 1]);
        const auto b = decoded_in_ra ? unpack(decoded[i], decoded[i + 3]) : unpack(decoded[i], decoded[i + 1]);
        const auto la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        const auto dot = std::clamp((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb), -1.0f, 1.0f);
        total += std::acos(dot) * 180.0 / M_PI;
    }
    return total / double(reference.size() / 4);
}

struct Compressed {
    double ms = 0.0;
    std::vector<uint8_t> decoded;
};

/// Compresses with every hardware thread, then decompresses for comparison.
Compressed astc(const Image& image, unsigned block, float quality, astcenc_profile profile, bool normal) {
    auto config = astcenc_config{};
    const auto flags = normal ? ASTCENC_FLG_MAP_NORMAL : 0u;
    if (astcenc_config_init(profile, block, block, 1, quality, flags, &config) != ASTCENC_SUCCESS) return {};
    const auto threads = std::max(1u, std::thread::hardware_concurrency());
    astcenc_context* context = nullptr;
    if (astcenc_context_alloc(&config, threads, &context, nullptr) != ASTCENC_SUCCESS) return {};
    auto* slice = const_cast<uint8_t*>(image.rgba.data());
    void* slices[] = {slice};
    auto input = astcenc_image{unsigned(image.width), unsigned(image.height), 1, ASTCENC_TYPE_U8, slices};
    // Normal maps keep x in RGB and y in alpha, as astcenc's normal mode expects.
    const auto swizzle = normal ? astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_G}
                                : astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const auto blocks = size_t((image.width + block - 1) / block) * size_t((image.height + block - 1) / block);
    auto out = std::vector<uint8_t>(blocks * 16);
    const auto start = Clock::now();
    auto workers = std::vector<std::thread>{};
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&, t] { astcenc_compress_image(context, &input, &swizzle, out.data(), out.size(), t); });
    for (auto& w : workers) w.join();
    auto result = Compressed{ms_since(start), std::vector<uint8_t>(image.rgba.size())};
    void* decoded_slices[] = {result.decoded.data()};
    auto decoded = astcenc_image{unsigned(image.width), unsigned(image.height), 1, ASTCENC_TYPE_U8, decoded_slices};
    const auto identity = astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    workers.clear();
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&, t] { astcenc_decompress_image(context, out.data(), out.size(), &decoded, &identity, t); });
    for (auto& w : workers) w.join();
    astcenc_context_free(context);
    return result;
}

// Environment: radiance from an equirectangular HDR image.
struct Hdr {
    int width = 0, height = 0;
    std::vector<float> rgb;
    std::array<float, 3> sample(float x, float y, float z) const { // a direction to texel, nearest
        const auto u = 0.5f + std::atan2(x, -z) / (2.0f * float(M_PI));
        const auto v = std::acos(std::clamp(y, -1.0f, 1.0f)) / float(M_PI);
        const auto i = std::min(width - 1, int(u * width)), j = std::min(height - 1, int(v * height));
        const auto* p = &rgb[(size_t(j) * width + i) * 3];
        return {p[0], p[1], p[2]};
    }
};

/// Nine spherical-harmonic coefficients of irradiance (per color channel), integrated over every texel.
std::array<std::array<float, 3>, 9> irradiance_sh(const Hdr& hdr) {
    auto sh = std::array<std::array<float, 3>, 9>{};
    for (int j = 0; j < hdr.height; ++j) {
        const auto theta = (float(j) + 0.5f) / float(hdr.height) * float(M_PI);
        const auto weight = std::sin(theta) * (2.0f * float(M_PI) / float(hdr.width)) * (float(M_PI) / float(hdr.height));
        for (int i = 0; i < hdr.width; ++i) {
            const auto phi = ((float(i) + 0.5f) / float(hdr.width) - 0.5f) * 2.0f * float(M_PI);
            const auto x = std::sin(theta) * std::sin(phi), y = std::cos(theta), z = -std::sin(theta) * std::cos(phi);
            const float basis[9] = {0.282095f, 0.488603f * y, 0.488603f * z, 0.488603f * x, 1.092548f * x * y,
                                    1.092548f * y * z, 0.315392f * (3 * z * z - 1), 1.092548f * x * z, 0.546274f * (x * x - y * y)};
            const auto* p = &hdr.rgb[(size_t(j) * hdr.width + i) * 3];
            for (int k = 0; k < 9; ++k)
                for (int c = 0; c < 3; ++c) sh[k][c] += p[c] * basis[k] * weight;
        }
    }
    return sh;
}

float radical_inverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

/// A cube of `face` texels per side and `levels` roughness levels, each texel the GGX-weighted radiance
/// around its direction from `samples` importance samples. Returns the texel count.
size_t prefilter(const Hdr& hdr, int face, int levels, int samples) {
    auto texels = std::atomic<size_t>{0};
    const auto threads = std::max(1u, std::thread::hardware_concurrency());
    auto workers = std::vector<std::thread>{};
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&, t] {
            auto local = size_t{0};
            auto sink = 0.0f;
            for (int level = 0; level < levels; ++level) {
                const auto size = std::max(1, face >> level);
                const auto roughness = float(level) / float(std::max(1, levels - 1));
                const auto a = roughness * roughness;
                for (int f = 0; f < 6; ++f)
                    for (int row = int(t); row < size; row += int(threads))
                        for (int col = 0; col < size; ++col) {
                            const auto s = (float(col) + 0.5f) / float(size) * 2 - 1, u = (float(row) + 0.5f) / float(size) * 2 - 1;
                            float n[3];
                            switch (f) {
                            case 0: n[0] = 1; n[1] = -u; n[2] = -s; break;
                            case 1: n[0] = -1; n[1] = -u; n[2] = s; break;
                            case 2: n[0] = s; n[1] = 1; n[2] = u; break;
                            case 3: n[0] = s; n[1] = -1; n[2] = -u; break;
                            case 4: n[0] = s; n[1] = -u; n[2] = 1; break;
                            default: n[0] = -s; n[1] = -u; n[2] = -1; break;
                            }
                            const auto length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            for (auto& v : n) v /= length;
                            const float up[3] = {std::abs(n[2]) < 0.999f ? 0.0f : 1.0f, 0.0f, std::abs(n[2]) < 0.999f ? 1.0f : 0.0f};
                            float tx[3] = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
                            const auto tl = std::sqrt(tx[0] * tx[0] + tx[1] * tx[1] + tx[2] * tx[2]);
                            for (auto& v : tx) v /= tl;
                            const float ty[3] = {n[1] * tx[2] - n[2] * tx[1], n[2] * tx[0] - n[0] * tx[2], n[0] * tx[1] - n[1] * tx[0]};
                            float sum[3] = {0, 0, 0}, weight = 0;
                            for (int k = 0; k < samples; ++k) {
                                const auto xi1 = float(k) / float(samples), xi2 = radical_inverse(uint32_t(k));
                                const auto phi = 2.0f * float(M_PI) * xi1;
                                const auto cos_theta = std::sqrt((1 - xi2) / (1 + (a * a - 1) * xi2));
                                const auto sin_theta = std::sqrt(1 - cos_theta * cos_theta);
                                float h[3];
                                for (int c = 0; c < 3; ++c)
                                    h[c] = tx[c] * std::cos(phi) * sin_theta + ty[c] * std::sin(phi) * sin_theta + n[c] * cos_theta;
                                const auto nh = h[0] * n[0] + h[1] * n[1] + h[2] * n[2];
                                float l[3];
                                for (int c = 0; c < 3; ++c) l[c] = 2 * nh * h[c] - n[c];
                                const auto nl = l[0] * n[0] + l[1] * n[1] + l[2] * n[2];
                                if (nl <= 0) continue;
                                const auto radiance = hdr.sample(l[0], l[1], l[2]);
                                for (int c = 0; c < 3; ++c) sum[c] += radiance[c] * nl;
                                weight += nl;
                            }
                            sink += weight > 0 ? sum[0] / weight : 0;
                            ++local;
                        }
            }
            texels += local + (sink < -1 ? 1 : 0); // keep the work from being optimized away
        });
    for (auto& w : workers) w.join();
    return texels;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: maya_texture_prototype <render-samples folder>\n");
        return 2;
    }
    if (!fs::exists(fs::path(argv[1]) / "Models")) {
        std::printf("No sample content in %s; run tools/fetch_render_samples.sh\n", argv[1]);
        return 77; // CTest's skip code for these prototypes
    }
    const auto samples = fs::path(argv[1]);
    const auto models = samples / "Models";

    // What the decision relies on fails the run (CTest's `prototype` label); the rest is reported.
    auto failures = 0;
    const auto unexpected = [&](const std::string& what) {
        std::printf("UNEXPECTED: %s\n", what.c_str());
        ++failures;
    };

    @autoreleasepool {
        auto device = MTLCreateSystemDefaultDevice();
        std::printf("Metal device: %s\n", device.name.UTF8String);
        std::printf("  BC compression: %s; ASTC (Apple2): %s; ASTC HDR (Apple6): %s; Apple7: %s; Apple9: %s\n\n",
                    device.supportsBCTextureCompression ? "yes" : "no", [device supportsFamily:MTLGPUFamilyApple2] ? "yes" : "no",
                    [device supportsFamily:MTLGPUFamilyApple6] ? "yes" : "no", [device supportsFamily:MTLGPUFamilyApple7] ? "yes" : "no",
                    [device supportsFamily:MTLGPUFamilyApple9] ? "yes" : "no");
        if (![device supportsFamily:MTLGPUFamilyApple2]) unexpected("the device does not support ASTC");
    }

    // Decoding every image of four models, one thread, then across every core.
    auto files = std::vector<fs::path>{};
    for (const auto* folder : {"Sponza/glTF", "ABeautifulGame/glTF", "FlightHelmet/glTF", "DamagedHelmet/glTF"})
        for (const auto& entry : fs::directory_iterator(models / folder)) {
            const auto ext = entry.path().extension().string();
            if (ext == ".jpg" || ext == ".png" || ext == ".jpeg") files.push_back(entry.path());
        }
    std::ranges::sort(files);
    auto bytes_on_disk = size_t{0};
    for (const auto& file : files) bytes_on_disk += fs::file_size(file);
    std::printf("Decoding %zu images (%.0f MiB on disk) to RGBA8\n", files.size(), double(bytes_on_disk) / (1 << 20));
    auto decoded = std::vector<Image>(files.size());
    auto pixels = size_t{0};
    auto start = Clock::now();
    for (size_t i = 0; i < files.size(); ++i) decoded[i] = decode_stb(files[i]);
    const auto stb_ms = ms_since(start);
    for (size_t i = 0; i < files.size(); ++i) {
        if (decoded[i].rgba.empty()) unexpected("stb_image cannot decode " + files[i].filename().string());
        pixels += size_t(decoded[i].width) * decoded[i].height;
    }
    start = Clock::now();
    auto worst_png = 0, worst_jpg = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto other = decode_imageio(files[i]);
        if (other.rgba.size() != decoded[i].rgba.size()) {
            std::printf("  size differs: %s\n", files[i].filename().c_str());
            continue;
        }
        auto worst = 0;
        for (size_t k = 0; k < other.rgba.size(); k += 4)
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(other.rgba[k + c]) - int(decoded[i].rgba[k + c])));
        (files[i].extension() == ".png" ? worst_png : worst_jpg) = std::max(files[i].extension() == ".png" ? worst_png : worst_jpg, worst);
    }
    const auto imageio_ms = ms_since(start); // includes the comparison, which is small next to decoding
    start = Clock::now();
    {
        auto next = std::atomic<size_t>{0};
        auto workers = std::vector<std::thread>{};
        for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
            workers.emplace_back([&] {
                for (auto i = next++; i < files.size(); i = next++) decode_stb(files[i]);
            });
        for (auto& w : workers) w.join();
    }
    const auto parallel_ms = ms_since(start);
    std::printf("  stb_image, one thread: %.0f ms (%.0f megapixels/s); every core: %.0f ms\n", stb_ms, double(pixels) / stb_ms / 1000.0,
                parallel_ms);
    std::printf("  ImageIO, one thread: %.0f ms; largest channel difference from stb: PNG %d, JPEG %d\n\n", imageio_ms, worst_png, worst_jpg);

    // Mip chains.
    start = Clock::now();
    auto mip_bytes = size_t{0}, base_bytes = size_t{0};
    for (size_t i = 0; i < files.size(); ++i) {
        mip_bytes += mip_chain(decoded[i], is_data(files[i]));
        base_bytes += decoded[i].rgba.size();
    }
    std::printf("Mip chains (sRGB-correct for color, linear for data), one thread: %.0f ms; mips add %.1f%% to %.0f MiB\n\n",
                ms_since(start), 100.0 * double(mip_bytes) / double(base_bytes), double(base_bytes) / (1 << 20));

    // ASTC on one material's three maps.
    // `floor` is the quality the chosen default must reach: PSNR at 6x6 medium, or the largest mean
    // angle error at 4x4 medium for normals.
    struct Map { const char* name; astcenc_profile profile; bool normal; int channels; double floor; };
    const Map maps[] = {{"Chessboard_base_color.jpg", ASTCENC_PRF_LDR_SRGB, false, 3, 45.0},
                        {"Chessboard_ORM.jpg", ASTCENC_PRF_LDR, false, 3, 43.0},
                        {"Chessboard_normal.jpg", ASTCENC_PRF_LDR, true, 2, 0.15}};
    std::printf("ASTC, every core (quality: PSNR in dB for color and data, mean angle error for normals)\n");
    for (const auto& map : maps) {
        const auto image = decode_stb(models / "ABeautifulGame/glTF" / map.name);
        std::printf("  %s, %dx%d\n", map.name, image.width, image.height);
        for (const auto block : {4u, 6u, 8u})
            for (const auto [quality, label] : {std::pair{ASTCENC_PRE_FAST, "fast"}, std::pair{ASTCENC_PRE_MEDIUM, "medium"}}) {
                const auto result = astc(image, block, quality, map.profile, map.normal);
                if (result.decoded.size() != image.rgba.size()) {
                    unexpected(std::string("astcenc failed on ") + map.name);
                    continue;
                }
                const auto bits = 128.0 / double(block * block);
                const auto is_default = quality == ASTCENC_PRE_MEDIUM && block == (map.normal ? 4u : 6u);
                if (map.normal) {
                    const auto degrees = normal_error_degrees(image.rgba, result.decoded, true);
                    std::printf("    %ux%u %-6s %5.2f bits/texel %8.0f ms   %.2f degrees\n", block, block, label, bits, result.ms, degrees);
                    if (is_default && !(degrees <= map.floor)) unexpected(std::string(map.name) + " is below its quality floor");
                } else {
                    const auto db = psnr(image.rgba, result.decoded, map.channels);
                    std::printf("    %ux%u %-6s %5.2f bits/texel %8.0f ms   %.2f dB\n", block, block, label, bits, result.ms, db);
                    if (is_default && !(db >= map.floor)) unexpected(std::string(map.name) + " is below its quality floor");
                }
            }
    }

    // The environment.
    auto hdr = Hdr{};
    start = Clock::now();
    int channels = 0;
    if (auto* data = stbi_loadf((samples / "hdri/aerodynamics_workshop_2k.hdr").c_str(), &hdr.width, &hdr.height, &channels, 3)) {
        hdr.rgb.assign(data, data + size_t(hdr.width) * hdr.height * 3);
        stbi_image_free(data);
    }
    std::printf("\nEnvironment: %dx%d HDR loaded in %.0f ms\n", hdr.width, hdr.height, ms_since(start));
    if (hdr.width == 0) {
        unexpected("the HDR environment cannot be loaded; run tools/fetch_render_samples.sh");
        return 1;
    }
    auto brightest = 0.0f;
    for (size_t i = 0; i < hdr.rgb.size(); i += 3)
        brightest = std::max(brightest, 0.2126f * hdr.rgb[i] + 0.7152f * hdr.rgb[i + 1] + 0.0722f * hdr.rgb[i + 2]);
    std::printf("  brightest texel: %.0f (relative luminance)\n", brightest);
    start = Clock::now();
    const auto sh = irradiance_sh(hdr);
    std::printf("  irradiance as 9 SH coefficients, one thread: %.0f ms (DC %.3f %.3f %.3f)\n", ms_since(start), sh[0][0], sh[0][1], sh[0][2]);
    for (const auto dc : sh[0])
        if (!(dc > 0.0f && std::isfinite(dc))) unexpected("the irradiance's constant term is not positive and finite");
    for (const auto [face, samples_per_texel] : {std::pair{128, 256}, std::pair{256, 256}, std::pair{256, 1024}}) {
        start = Clock::now();
        const auto texels = prefilter(hdr, face, 6, samples_per_texel);
        std::printf("  prefiltered specular cube %d per face, 6 levels, %d samples per texel, every core: %.0f ms (%zu texels, %.1f MiB as RGBA16F)\n",
                    face, samples_per_texel, ms_since(start), texels, double(texels) * 8 / (1 << 20));
    }
    return failures == 0 ? 0 : 1;
}
