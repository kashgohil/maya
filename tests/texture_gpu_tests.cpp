#include "maya/assets/registry.hpp"
#include "maya/assets/texture_cook.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "support/png.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace maya;
namespace fs = std::filesystem;

namespace {
// Draws a quad covering the target that samples texture 0 at an explicit mip level.
constexpr auto sample_shader = R"(
    #include <metal_stdlib>
    using namespace metal;
    constant float2 corners[6] = {float2(0,0), float2(1,0), float2(0,1), float2(1,0), float2(1,1), float2(0,1)};
    struct Sampled { float4 position [[position]]; float2 uv; };
    struct Lod { float level; };
    vertex Sampled vertexFull(uint id [[vertex_id]]) {
        const float2 c = corners[id % 6];
        return {float4(c * 2.0 - 1.0, 0.0, 1.0), float2(c.x, 1.0 - c.y)};
    }
    fragment float4 sampleLevel(Sampled in [[stage_in]], texture2d<float> image [[texture(0)]],
                                sampler filter [[sampler(0)]], constant Lod& lod [[buffer(0)]]) {
        return image.sample(filter, in.uv, level(lod.level));
    }
)";
using Pixel = std::array<int, 4>;

struct Sampling {
    MetalDevice device;
    PipelineHandle pipeline;
    Sampling() {
        REQUIRE(device.initialize(nullptr));
        auto desc = PipelineDesc{};
        desc.shader_source = sample_shader;
        desc.vertex_entry = "vertexFull";
        desc.fragment_entry = "sampleLevel";
        desc.color_formats = {Format::rgba8_unorm};
        desc.cull = CullMode::none;
        desc.label = "sample level";
        const auto created = device.create_pipeline(desc);
        INFO(created.diagnostic.message);
        REQUIRE(created);
        pipeline = created.handle;
    }
    SamplerHandle sampler(Filter filter, MipFilter mips) {
        const auto created = device.create_sampler({filter, filter, AddressMode::clamp_to_edge, AddressMode::clamp_to_edge, "test", mips});
        REQUIRE(created);
        return created.handle;
    }
    /// Encodes one frame that samples `texture` into a new size x size RGBA8 target and submits it.
    TextureHandle draw(TextureHandle texture, SamplerHandle sampler, float lod, uint32_t size) {
        const auto target = device.create_texture({size, size, Format::rgba8_unorm, TextureUsage::render_target | TextureUsage::readback, "target"});
        REQUIRE(target);
        REQUIRE_FALSE(device.begin_frame());
        auto pass = RenderPassDesc{};
        pass.colors.push_back({target.handle, LoadAction::clear, StoreAction::store, {0, 0, 0, 0}});
        REQUIRE_FALSE(device.begin_render_pass(pass));
        REQUIRE_FALSE(device.set_pipeline(pipeline));
        const auto uploaded = device.upload_transient(&lod, sizeof(lod));
        REQUIRE(uploaded);
        REQUIRE_FALSE(device.set_uniform_buffer(0, uploaded.slice));
        REQUIRE_FALSE(device.set_texture(0, texture));
        REQUIRE_FALSE(device.set_sampler(0, sampler));
        REQUIRE_FALSE(device.draw(6));
        REQUIRE_FALSE(device.end_render_pass());
        REQUIRE_FALSE(device.end_frame());
        return target.handle;
    }
    std::vector<std::byte> read(TextureHandle target) {
        device.wait_idle();
        auto pixels = std::vector<std::byte>{};
        REQUIRE_FALSE(device.read_texture(target, pixels));
        device.destroy(target);
        return pixels;
    }
    std::vector<std::byte> sample(TextureHandle texture, SamplerHandle sampler, float lod, uint32_t size) {
        return read(draw(texture, sampler, lod, size));
    }
};
Pixel at(const std::vector<std::byte>& pixels, uint32_t width, uint32_t x, uint32_t y) {
    const auto* texel = pixels.data() + (size_t{y} * width + x) * 4;
    return {int(texel[0]), int(texel[1]), int(texel[2]), int(texel[3])};
}
bool near(const Pixel& value, const Pixel& expected, int tolerance) {
    for (size_t i = 0; i < 4; ++i)
        if (std::abs(value[i] - expected[i]) > tolerance) return false;
    return true;
}
/// The 8-bit linear value an sRGB-encoded 8-bit value decodes to.
int linear(int encoded) {
    const auto c = encoded / 255.0;
    return int(std::lround((c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)) * 255.0));
}
SourceImage image(uint32_t width, uint32_t height, auto&& texel) {
    auto source = SourceImage{width, height, std::vector<uint8_t>(size_t{width} * height * 4)};
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto value = texel(x, y);
            std::memcpy(source.rgba.data() + (size_t{y} * width + x) * 4, value.data(), 4);
        }
    return source;
}
TextureSettings settings(TextureRole role, TextureCompression compression) {
    auto value = TextureSettings{};
    value.source = "image.png";
    value.role = role;
    value.compression = compression;
    return value;
}
TextureHandle upload(GraphicsDevice& device, const TextureImage& image) {
    const auto created = device.create_texture(image.desc("cooked"), image.data);
    INFO(created.diagnostic.message);
    REQUIRE(created);
    return created.handle;
}
const auto solid = [](uint8_t r, uint8_t g, uint8_t b) {
    return [=](uint32_t, uint32_t) { return std::array<uint8_t, 4>{r, g, b, 255}; };
};
} // namespace

TEST_CASE("Metal samples every mip level of a texture", "[rhi][textures]") {
    Sampling sampling;
    auto& device = sampling.device;
    // 8x8, 4x4, 2x2, 1x1: red, green, blue, yellow.
    const auto colors = std::array<Pixel, 4>{Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255}, Pixel{0, 0, 255, 255}, Pixel{255, 255, 0, 255}};
    const auto desc = TextureDesc{8, 8, Format::rgba8_unorm, TextureUsage::sampled, "levels", 4};
    auto data = std::vector<std::byte>{};
    for (uint32_t level = 0; level < 4; ++level)
        for (uint32_t i = 0; i < mip_extent(8, level) * mip_extent(8, level); ++i)
            for (const auto channel : colors[level]) data.push_back(std::byte(channel));
    const auto texture = device.create_texture(desc, data);
    REQUIRE(texture);
    const auto nearest = sampling.sampler(Filter::nearest, MipFilter::nearest);
    for (uint32_t level = 0; level < 4; ++level) {
        INFO("level " << level);
        CHECK(at(sampling.sample(texture.handle, nearest, float(level), 4), 4, 1, 1) == colors[level]);
    }
    // Linear mip filtering blends neighboring levels; no mip filtering reads level 0 only.
    const auto blended = at(sampling.sample(texture.handle, sampling.sampler(Filter::nearest, MipFilter::linear), 0.5f, 4), 4, 1, 1);
    CHECK(near(blended, {128, 128, 0, 255}, 2));
    CHECK(at(sampling.sample(texture.handle, sampling.sampler(Filter::nearest, MipFilter::none), 2.0f, 4), 4, 1, 1) == colors[0]);
}

TEST_CASE("Metal decodes sRGB textures to linear values and leaves linear ones as stored", "[rhi][textures]") {
    Sampling sampling;
    auto& device = sampling.device;
    const auto texel = std::vector<std::byte>{std::byte{188}, std::byte{64}, std::byte{0}, std::byte{128}};
    const auto srgb = device.create_texture({1, 1, Format::rgba8_srgb, TextureUsage::sampled, "srgb"}, texel);
    const auto unorm = device.create_texture({1, 1, Format::rgba8_unorm, TextureUsage::sampled, "unorm"}, texel);
    REQUIRE((srgb && unorm));
    const auto nearest = sampling.sampler(Filter::nearest, MipFilter::none);
    CHECK(near(at(sampling.sample(srgb.handle, nearest, 0, 2), 2, 0, 0), {linear(188), linear(64), 0, 128}, 1)); // alpha stays linear
    CHECK(at(sampling.sample(unorm.handle, nearest, 0, 2), 2, 0, 0) == Pixel{188, 64, 0, 128});
}

TEST_CASE("Metal samples ASTC textures cooked from images, at every level", "[rhi][textures]") {
    Sampling sampling;
    auto& device = sampling.device;
    REQUIRE(device.limits().astc);
    const auto nearest = sampling.sampler(Filter::nearest, MipFilter::nearest);
    const auto check_levels = [&](const TextureImage& cooked, const Pixel& expected, int tolerance) {
        const auto texture = upload(device, cooked);
        for (const auto level : {0u, cooked.mip_levels / 2, cooked.mip_levels - 1}) {
            INFO(format_name(cooked.format) << " level " << level);
            CHECK(near(at(sampling.sample(texture, nearest, float(level), 4), 4, 1, 1), expected, tolerance));
        }
        device.destroy(texture);
    };
    const auto flat = image(64, 64, solid(200, 100, 50));
    const auto color = cook_texture(flat, settings(TextureRole::color, TextureCompression::astc));
    REQUIRE(color);
    CHECK(color.image.format == Format::astc_6x6_srgb);
    check_levels(color.image, {linear(200), linear(100), linear(50), 255}, 2);
    const auto data = cook_texture(flat, settings(TextureRole::data, TextureCompression::astc));
    REQUIRE(data);
    check_levels(data.image, {200, 100, 50, 255}, 2);
    // A tilted normal (0.6, 0, 0.8) is stored as x in red, green, and blue and y in alpha.
    const auto normal = cook_texture(image(32, 32, solid(204, 128, 229)), settings(TextureRole::normal, TextureCompression::astc));
    REQUIRE(normal);
    CHECK(normal.image.format == Format::astc_4x4_unorm);
    check_levels(normal.image, {204, 204, 204, 128}, 3);

    // A detailed image survives compression texel by texel, in its block layout.
    const auto detail = image(60, 36, [](uint32_t x, uint32_t y) {
        return std::array<uint8_t, 4>{uint8_t(x * 4), uint8_t(y * 7), uint8_t(128 + 100 * std::sin(x * 0.3) * std::cos(y * 0.2)), 255};
    });
    auto rgba8 = settings(TextureRole::data, TextureCompression::rgba8);
    rgba8.mips = false;
    auto astc = settings(TextureRole::data, TextureCompression::astc);
    astc.mips = false;
    const auto exact = cook_texture(detail, rgba8);
    const auto compressed = cook_texture(detail, astc);
    REQUIRE((exact && compressed));
    // Both sampled the same texels (nearest, level 0), so they differ only by compression.
    const auto sampled_exact = sampling.sample(upload(device, exact.image), nearest, 0, 60);
    const auto sampled = sampling.sample(upload(device, compressed.image), nearest, 0, 60);
    auto error = 0.0, compared = 0.0;
    for (uint32_t y = 0; y < 60; ++y)
        for (uint32_t x = 0; x < 60; ++x) {
            const auto a = at(sampled_exact, 60, x, y), b = at(sampled, 60, x, y);
            for (size_t c = 0; c < 3; ++c) error += std::abs(a[c] - b[c]);
            compared += 3;
        }
    CHECK(error / compared < 3.0); // mean absolute error per channel
    CHECK(at(sampled_exact, 60, 59, 0) == Pixel{236, 0, int(detail.rgba[59 * 4 + 2]), 255}); // the RGBA8 reference is exact
}

namespace {
struct Project {
    fs::path root;
    Project() {
        static auto counter = std::atomic<int>{0};
        root = fs::temp_directory_path() / ("maya-texture-gpu-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Project() { fs::remove_all(root); }
    void write(const fs::path& relative, const std::string& bytes) const {
        auto file = std::ofstream(root / relative, std::ios::binary);
        file << bytes;
    }
    /// A 16x16 PNG in four 8x8 quadrants of one color each.
    void quadrants(const fs::path& relative, std::array<std::array<uint8_t, 3>, 4> colors) const {
        auto rgba = std::vector<uint8_t>(16 * 16 * 4);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) {
                const auto& color = colors[(y / 8) * 2 + x / 8];
                std::memcpy(rgba.data() + (y * 16 + x) * 4, color.data(), 3);
                rgba[(y * 16 + x) * 4 + 3] = 255;
            }
        write(relative, test::encode_png_rgba(16, 16, rgba));
    }
};
constexpr auto texture_ref = AssetRef<TextureAsset>{{0x6d617961, 0x50}};
const auto descriptor = std::string("maya-texture 1\nsource \"quadrants.png\"\nusage color\ncompression astc\nmips on\n"
                                    "filter nearest nearest\nmip_filter nearest\nanisotropy 1\naddress clamp clamp\n");
} // namespace

TEST_CASE("Metal draws texture assets, and the placeholder stands in for missing ones", "[rhi][textures][assets]") {
    Sampling sampling;
    auto& device = sampling.device;
    Project project;
    project.quadrants("quadrants.png", {{{250, 30, 30}, {30, 250, 30}, {30, 30, 250}, {240, 240, 240}}});
    project.write("quadrants.texture", descriptor);
    AssetRegistry registry(project.root, std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(texture_ref, "quadrants.texture"));
    const auto loaded = registry.acquire(texture_ref);
    INFO(loaded.diagnostic.message);
    REQUIRE(loaded);
    const auto& texture = loaded.lease.value();
    CHECK(texture.texture().desc().format == Format::astc_6x6_srgb);
    // Each quadrant's center, through the asset's own sampler, decoded to linear.
    const auto pixels = sampling.sample(texture.texture().handle(), texture.sampler().handle(), 0, 16);
    CHECK(near(at(pixels, 16, 4, 4), {linear(250), linear(30), linear(30), 255}, 4));
    CHECK(near(at(pixels, 16, 12, 4), {linear(30), linear(250), linear(30), 255}, 4));
    CHECK(near(at(pixels, 16, 4, 12), {linear(30), linear(30), linear(250), 255}, 4));
    CHECK(near(at(pixels, 16, 12, 12), {linear(240), linear(240), linear(240), 255}, 4));

    const auto missing = AssetRef<TextureAsset>{{0x6d617961, 0x51}};
    REQUIRE_FALSE(registry.register_asset(missing, "missing.texture"));
    CHECK_FALSE(registry.acquire(missing));
    const auto placeholder = make_placeholder_texture(device);
    REQUIRE(placeholder);
    const auto checker = sampling.sample(placeholder->texture().handle(), placeholder->sampler().handle(), 0, 8);
    CHECK(at(checker, 8, 0, 0) == Pixel{255, 0, 255, 255});
    CHECK(at(checker, 8, 2, 0) == Pixel{0, 0, 0, 255});
    CHECK(at(checker, 8, 3, 3) == Pixel{255, 0, 255, 255});
}

TEST_CASE("Metal keeps a reloaded texture's old version until frames that sample it finish", "[rhi][textures][assets]") {
    Sampling sampling;
    auto& device = sampling.device;
    Project project;
    const auto red = std::array<uint8_t, 3>{255, 0, 0}, blue = std::array<uint8_t, 3>{0, 0, 255};
    project.quadrants("quadrants.png", {red, red, red, red});
    project.write("quadrants.texture", descriptor);
    const auto baseline = device.native_texture_count();
    {
        AssetRegistry registry(project.root, std::make_unique<FileAssetProvider>(device));
        REQUIRE_FALSE(registry.register_asset(texture_ref, "quadrants.texture"));
        auto first = registry.acquire(texture_ref);
        REQUIRE(first);
        const auto before = sampling.draw(first.lease.value().texture().handle(), first.lease.value().sampler().handle(), 0, 4);
        // The frame may still be executing: reload, and release the only lease on the old version.
        project.quadrants("quadrants.png", {blue, blue, blue, blue});
        const auto second = registry.reload(texture_ref);
        REQUIRE(second);
        first = {};
        const auto after = sampling.draw(second.lease.value().texture().handle(), second.lease.value().sampler().handle(), 0, 4);
        CHECK(at(sampling.read(before), 4, 1, 1) == Pixel{255, 0, 0, 255});
        CHECK(at(sampling.read(after), 4, 1, 1) == Pixel{0, 0, 255, 255});
    }
    device.wait_idle();
    CHECK(device.native_texture_count() == baseline);
}
