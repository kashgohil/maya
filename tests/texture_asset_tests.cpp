#include "maya/assets/registry.hpp"
#include "maya/assets/texture_cook.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/khronos_ktx2.hpp"
#include "support/png.hpp"
#include "support/solid_jpeg.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace maya;
namespace fs = std::filesystem;

namespace {
constexpr auto texture_ref = AssetRef<TextureAsset>{{0x6d617961, 0x50}};

/// A scratch project folder, removed afterwards.
struct Folder {
    fs::path root;
    Folder() {
        static auto counter = std::atomic<int>{0};
        root = fs::temp_directory_path() / ("maya-texture-tests-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Folder() { fs::remove_all(root); }
    void write(const fs::path& relative, std::string_view bytes) const {
        fs::create_directories((root / relative).parent_path());
        auto file = std::ofstream(root / relative, std::ios::binary);
        file.write(bytes.data(), std::streamsize(bytes.size()));
    }
    void write(const fs::path& relative, std::span<const std::byte> bytes) const {
        write(relative, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
};

std::vector<uint8_t> pixels(uint32_t width, uint32_t height, auto&& texel) {
    auto rgba = std::vector<uint8_t>(size_t{width} * height * 4);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto value = texel(x, y);
            std::memcpy(rgba.data() + (size_t{y} * width + x) * 4, value.data(), 4);
        }
    return rgba;
}
std::string png(uint32_t width, uint32_t height, auto&& texel) { return test::encode_png_rgba(width, height, pixels(width, height, texel)); }
std::span<const std::byte> bytes_of(const std::string& text) { return {reinterpret_cast<const std::byte*>(text.data()), text.size()}; }

const auto canonical = std::string("maya-texture 1\nsource \"wood.png\"\nusage color\ncompression astc\nmips on\n"
                                   "filter linear linear\nmip_filter linear\nanisotropy 8\naddress repeat clamp\n");
std::string with(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
TextureSettingsResult parse(const std::string& text) {
    auto input = std::istringstream(text);
    return read_texture_settings(input);
}
TextureSettings settings(TextureRole role, TextureCompression compression, bool mips = true) {
    auto value = TextureSettings{};
    value.source = "source.png";
    value.role = role;
    value.compression = compression;
    value.mips = mips;
    if (!mips) value.sampler.mip_filter = MipFilter::none;
    return value;
}
SourceImage source(uint32_t width, uint32_t height, auto&& texel) { return {width, height, pixels(width, height, texel)}; }
std::array<uint8_t, 4> texel_at(const TextureImage& image, uint32_t level, uint32_t x, uint32_t y) {
    auto offset = size_t{0};
    for (uint32_t l = 0; l < level; ++l) offset += mip_level_bytes(image.format, image.width, image.height, l);
    offset += (size_t{y} * mip_extent(image.width, level) + x) * 4;
    auto out = std::array<uint8_t, 4>{};
    std::memcpy(out.data(), image.data.data() + offset, 4);
    return out;
}
bool near(int value, int expected, int tolerance = 2) { return std::abs(value - expected) <= tolerance; }
uint32_t word(const std::vector<std::byte>& file, size_t offset) {
    auto value = uint32_t{};
    std::memcpy(&value, file.data() + offset, 4);
    return value;
}
uint64_t word64(const std::vector<std::byte>& file, size_t offset) {
    auto value = uint64_t{};
    std::memcpy(&value, file.data() + offset, 8);
    return value;
}
void put(std::vector<std::byte>& file, size_t offset, uint32_t value) { std::memcpy(file.data() + offset, &value, 4); }
void put64(std::vector<std::byte>& file, size_t offset, uint64_t value) { std::memcpy(file.data() + offset, &value, 8); }
/// An 8x4 sRGB RGBA8 image with all four levels, each texel numbered.
TextureImage numbered_chain() {
    auto image = TextureImage{Format::rgba8_srgb, 8, 4, 4, {}};
    image.data.resize(texture_bytes(image.desc()));
    for (size_t i = 0; i < image.data.size(); ++i) image.data[i] = std::byte(i * 7 + 3);
    return image;
}
const auto brick = [](uint32_t x, uint32_t y) {
    return std::array<uint8_t, 4>{uint8_t(x * 9), uint8_t(y * 5), uint8_t((x ^ y) * 3), 255};
};
} // namespace

TEST_CASE("Texture files state every setting, and their errors name the line", "[assets][textures]") {
    const auto read = parse(canonical);
    REQUIRE(read);
    const auto& value = read.settings;
    CHECK(value.source == "wood.png");
    CHECK(value.role == TextureRole::color);
    CHECK(value.compression == TextureCompression::astc);
    CHECK(value.mips);
    CHECK(value.sampler.min_filter == Filter::linear);
    CHECK(value.sampler.mip_filter == MipFilter::linear);
    CHECK(value.sampler.max_anisotropy == 8);
    CHECK(value.sampler.address_u == AddressMode::repeat);
    CHECK(value.sampler.address_v == AddressMode::clamp_to_edge);
    auto written = std::ostringstream{};
    write_texture_settings(written, value);
    CHECK(written.str() == canonical);

    // Any order, blank lines, and every value.
    const auto shuffled = parse("\n\nmaya-texture 1\naddress mirror repeat\nanisotropy 1\n\nmip_filter none\nfilter nearest linear\n"
                                "mips off\ncompression rgba8\nusage normal\nsource \"maps/stone normal.png\"\n");
    REQUIRE(shuffled);
    CHECK(shuffled.settings.source == "maps/stone normal.png");
    CHECK(shuffled.settings.role == TextureRole::normal);
    CHECK(shuffled.settings.compression == TextureCompression::rgba8);
    CHECK_FALSE(shuffled.settings.mips);
    CHECK(shuffled.settings.sampler.min_filter == Filter::nearest);
    CHECK(shuffled.settings.sampler.address_u == AddressMode::mirror_repeat);
    auto rewritten = std::ostringstream{};
    write_texture_settings(rewritten, shuffled.settings);
    const auto again = parse(rewritten.str());
    REQUIRE(again);
    CHECK(again.settings.source == shuffled.settings.source);
    CHECK(again.settings.sampler.max_anisotropy == 1);

    const auto refused = [](const std::string& text, const std::string& expected) {
        const auto result = parse(text);
        INFO(text);
        CHECK_FALSE(result);
        CHECK(result.error == expected);
    };
    refused("", "the file is empty; expected the header 'maya-texture 1'");
    refused("maya-material 1\n", "line 1: expected the header 'maya-texture 1'");
    refused(with(canonical, "maya-texture 1", "maya-texture 2"), "line 1: unsupported texture file version 2; this build reads version 1");
    refused(with(canonical, "mips on", "mipmaps on"), "line 5: unknown setting 'mipmaps'");
    refused(canonical + "usage data\n", "line 10: 'usage' is set again (first on line 3)");
    refused(with(canonical, "usage color", "usage albedo"), "line 3: usage must be color, data, or normal");
    refused(with(canonical, "compression astc", "compression bc7"), "line 4: compression must be astc or rgba8");
    refused(with(canonical, "mips on", "mips yes"), "line 5: mips must be on or off");
    refused(with(canonical, "filter linear linear", "filter linear"),
            "line 6: filter needs two of nearest or linear: minification, then magnification");
    refused(with(canonical, "mip_filter linear", "mip_filter cubic"), "line 7: mip_filter must be none, nearest, or linear");
    refused(with(canonical, "anisotropy 8", "anisotropy 0"), "line 8: anisotropy must be a whole number from 1 (off) to 16");
    refused(with(canonical, "anisotropy 8", "anisotropy 17"), "line 8: anisotropy must be a whole number from 1 (off) to 16");
    refused(with(canonical, "anisotropy 8", "anisotropy 8x"), "line 8: anisotropy must be a whole number from 1 (off) to 16");
    refused(with(canonical, "address repeat clamp", "address wrap clamp"), "line 9: address needs two of repeat, clamp, or mirror: u, then v");
    refused(with(canonical, "usage color", "usage color data"), "line 3: unexpected 'data' after usage");
    refused(with(canonical, "\"wood.png\"", "\"\""), "line 2: source needs a quoted, nonempty path");
    refused(with(canonical, "\"wood.png\"", "\"/textures/wood.png\""), "line 2: source must be relative to the texture file's folder");
    refused(with(canonical, "\"wood.png\"", "\"../shared/wood.png\""), "line 2: source must stay in the texture file's folder or below it");
    refused(with(canonical, "anisotropy 8\n", ""), "'anisotropy' is missing; a texture file states every setting");
    refused(with(canonical, "mips on", "mips off"), "line 7: mip_filter must be none when mips is off");
}

TEST_CASE("Usage and compression choose the GPU format", "[assets][textures]") {
    CHECK(texture_format(TextureRole::color, TextureCompression::astc) == Format::astc_6x6_srgb);
    CHECK(texture_format(TextureRole::data, TextureCompression::astc) == Format::astc_6x6_unorm);
    CHECK(texture_format(TextureRole::normal, TextureCompression::astc) == Format::astc_4x4_unorm);
    CHECK(texture_format(TextureRole::color, TextureCompression::rgba8) == Format::rgba8_srgb);
    CHECK(texture_format(TextureRole::data, TextureCompression::rgba8) == Format::rgba8_unorm);
    CHECK(texture_format(TextureRole::normal, TextureCompression::rgba8) == Format::rgba8_unorm);
}

TEST_CASE("Source images decode from PNG and JPEG, and broken or oversized files are refused", "[assets][textures]") {
    const auto file = png(5, 3, brick);
    const auto decoded = decode_image(bytes_of(file));
    REQUIRE(decoded);
    CHECK(decoded.image.width == 5);
    CHECK(decoded.image.height == 3);
    CHECK(decoded.image.rgba == pixels(5, 3, brick)); // stored values, exactly
    const auto translucent = png(2, 1, [](uint32_t x, uint32_t) { return std::array<uint8_t, 4>{255, 0, 0, uint8_t(x ? 0 : 128)}; });
    CHECK(decode_image(bytes_of(translucent)).image.rgba == std::vector<uint8_t>{255, 0, 0, 128, 255, 0, 0, 0}); // straight alpha

    const auto jpeg = decode_image(std::as_bytes(std::span(test::solid_jpeg)));
    REQUIRE(jpeg);
    CHECK(jpeg.image.width == 8);
    CHECK((near(jpeg.image.rgba[0], 200, 3) && near(jpeg.image.rgba[1], 100, 3) && near(jpeg.image.rgba[2], 50, 3)));
    CHECK(jpeg.image.rgba[3] == 255);

    const auto garbage = decode_image(bytes_of("this is not an image"));
    CHECK_FALSE(garbage);
    CHECK(garbage.error.starts_with("cannot decode the image: "));
    CHECK_FALSE(decode_image(bytes_of(file.substr(0, file.size() / 2))));
    CHECK_FALSE(decode_image({}));
    const auto large = decode_image(bytes_of(png(64, 8, brick)), 32);
    CHECK(large.error == "the image is 64x8; textures can be at most 32 on each side");
}

TEST_CASE("Cooking builds every mip level in the declared format", "[assets][textures]") {
    // A black and white checkerboard averages to middle gray: sRGB-correct for color, linear for data.
    const auto checker = source(2, 2, [](uint32_t x, uint32_t y) {
        const auto white = uint8_t((x + y) % 2 ? 255 : 0);
        return std::array<uint8_t, 4>{white, white, white, 255};
    });
    const auto color = cook_texture(checker, settings(TextureRole::color, TextureCompression::rgba8));
    REQUIRE(color);
    CHECK(color.image.format == Format::rgba8_srgb);
    CHECK(color.image.mip_levels == 2);
    CHECK(near(texel_at(color.image, 1, 0, 0)[0], 188)); // linear 0.5, encoded
    CHECK(texel_at(color.image, 1, 0, 0)[3] == 255);
    const auto data = cook_texture(checker, settings(TextureRole::data, TextureCompression::rgba8));
    REQUIRE(data);
    CHECK(data.image.format == Format::rgba8_unorm);
    CHECK(near(texel_at(data.image, 1, 0, 0)[0], 128));
    CHECK(texel_at(data.image, 0, 1, 0)[0] == 255); // level 0 is the source, unchanged

    // Normal maps are renormalized and stored as x in red, green, and blue and y in alpha.
    const auto tilted = source(4, 4, [](uint32_t x, uint32_t) {
        if (x == 3) return std::array<uint8_t, 4>{128, 128, 255, 255}; // straight up
        return std::array<uint8_t, 4>{uint8_t(0.3 * 127.5 + 127.5), 128, uint8_t(0.4 * 127.5 + 127.5), 255}; // (0.3, 0, 0.4) -> (0.6, 0, 0.8)
    });
    const auto normal = cook_texture(tilted, settings(TextureRole::normal, TextureCompression::rgba8));
    REQUIRE(normal);
    const auto stored = texel_at(normal.image, 0, 0, 0);
    CHECK(near(stored[0], 204));
    CHECK((stored[1] == stored[0] && stored[2] == stored[0]));
    CHECK(near(stored[3], 128));
    const auto flat = texel_at(normal.image, 0, 3, 0);
    CHECK((near(flat[0], 128) && near(flat[3], 128)));
    CHECK(normal.image.mip_levels == 3);

    // Odd sizes: every level, max(1, size >> level), tightly packed.
    const auto odd = cook_texture(source(300, 17, brick), settings(TextureRole::data, TextureCompression::rgba8));
    REQUIRE(odd);
    CHECK(odd.image.mip_levels == 9);
    CHECK(odd.image.data.size() == texture_bytes(odd.image.desc()));
    const auto single = cook_texture(source(300, 17, brick), settings(TextureRole::data, TextureCompression::rgba8, false));
    CHECK(single.image.mip_levels == 1);
    CHECK(single.image.data.size() == 300u * 17u * 4u);

    // ASTC: 6x6 for color and data, 4x4 for normals; RGBA8 when the device cannot sample ASTC.
    const auto image = source(100, 60, brick);
    const auto astc_color = cook_texture(image, settings(TextureRole::color, TextureCompression::astc));
    REQUIRE(astc_color);
    CHECK(astc_color.image.format == Format::astc_6x6_srgb);
    CHECK(astc_color.image.mip_levels == 7);
    CHECK(astc_color.image.data.size() == texture_bytes(astc_color.image.desc()));
    CHECK(cook_texture(image, settings(TextureRole::normal, TextureCompression::astc)).image.format == Format::astc_4x4_unorm);
    CHECK(cook_texture(image, settings(TextureRole::color, TextureCompression::astc), {false, 0}).image.format == Format::rgba8_srgb);
    // The same input gives the same bytes, however many threads compress it.
    CHECK(cook_texture(image, settings(TextureRole::color, TextureCompression::astc), {true, 1}).image.data == astc_color.image.data);
    CHECK(cook_texture(image, settings(TextureRole::color, TextureCompression::astc), {true, 3}).image.data == astc_color.image.data);

    CHECK(cook_texture({}, settings(TextureRole::color, TextureCompression::rgba8)).error == "the source image is empty or incomplete");
}

TEST_CASE("KTX2 files round-trip, laid out as the specification requires", "[assets][textures]") {
    const auto image = numbered_chain();
    const auto file = write_ktx2(image, "Maya test");
    const auto read = read_ktx2(file);
    REQUIRE(read);
    CHECK(read.image.format == image.format);
    CHECK((read.image.width == 8 && read.image.height == 4 && read.image.mip_levels == 4));
    CHECK(read.image.data == image.data);

    // Header and index.
    CHECK(std::memcmp(file.data(), "\xABKTX 20\xBB\r\n\x1A\n", 12) == 0);
    CHECK(word(file, 12) == 43); // VK_FORMAT_R8G8B8A8_SRGB
    CHECK(word(file, 16) == 1); // typeSize
    CHECK((word(file, 28) == 0 && word(file, 32) == 0 && word(file, 36) == 1)); // 2D, no layers, one face
    CHECK(word(file, 40) == 4);
    CHECK(word(file, 44) == 0); // no supercompression
    // Levels are stored smallest first, each aligned to 4 bytes.
    auto previous = uint64_t{0};
    for (uint32_t level = 4; level-- > 0;) {
        const auto offset = word64(file, 80 + 24 * level), length = word64(file, 88 + 24 * level);
        CHECK(offset % 4 == 0);
        CHECK(offset >= previous);
        CHECK(length == mip_level_bytes(image.format, 8, 4, level));
        CHECK(word64(file, 96 + 24 * level) == length);
        previous = offset + length;
    }
    CHECK(previous == file.size());
    // The data format descriptor: RGBSDA, BT.709 primaries, sRGB transfer, four 8-bit samples, alpha linear.
    const auto dfd = word(file, 48);
    CHECK(word(file, 52) == 4 + 24 + 16 * 4);
    CHECK(word(file, dfd) == word(file, 52));
    CHECK(word(file, dfd + 12) == (1u | (1u << 8) | (2u << 16)));
    CHECK(word(file, dfd + 20) == 4); // bytesPlane0
    CHECK(word(file, dfd + 28 + 48) == (24u | (7u << 16) | (0x1Fu << 24)));
    // Key/value data sorted by key, each NUL-terminated.
    const auto kvd = std::string(reinterpret_cast<const char*>(file.data() + word(file, 56)), word(file, 60));
    CHECK(kvd.find(std::string("KTXorientation\0rd\0", 18)) != std::string::npos);
    CHECK(kvd.find(std::string("KTXwriter\0Maya test\0", 20)) != std::string::npos);
    CHECK(kvd.find("KTXorientation") < kvd.find("KTXwriter"));

    // ASTC: 16-byte blocks, aligned to 16, with the block size in the descriptor.
    const auto cooked = cook_texture(source(100, 60, brick), settings(TextureRole::color, TextureCompression::astc));
    REQUIRE(cooked);
    const auto astc = write_ktx2(cooked.image);
    CHECK(word(astc, 12) == 166); // VK_FORMAT_ASTC_6x6_SRGB_BLOCK
    for (uint32_t level = 0; level < cooked.image.mip_levels; ++level) CHECK(word64(astc, 80 + 24 * level) % 16 == 0);
    const auto astc_dfd = word(astc, 48);
    CHECK(word(astc, astc_dfd + 12) == (162u | (1u << 8) | (2u << 16)));
    CHECK(word(astc, astc_dfd + 16) == (5u | (5u << 8)));
    const auto astc_read = read_ktx2(astc);
    REQUIRE(astc_read);
    CHECK(astc_read.image.data == cooked.image.data);
    const auto linear = cook_texture(source(16, 16, brick), settings(TextureRole::normal, TextureCompression::astc));
    CHECK(word(write_ktx2(linear.image), 12) == 157); // VK_FORMAT_ASTC_4x4_UNORM_BLOCK

    auto incomplete = image;
    incomplete.data.pop_back();
    CHECK_THROWS_AS(write_ktx2(incomplete), std::invalid_argument);
    auto unsupported = image;
    unsupported.format = Format::rgba16_float;
    CHECK_THROWS_AS(write_ktx2(unsupported), std::invalid_argument);
}

TEST_CASE("KTX2 files written by Khronos's own tool are read", "[assets][textures]") {
    const auto rgba8 = read_ktx2(std::as_bytes(std::span(test::khronos_rgba8_ktx2)));
    INFO(rgba8.error);
    REQUIRE(rgba8);
    CHECK(rgba8.image.format == Format::rgba8_srgb);
    CHECK((rgba8.image.width == 12 && rgba8.image.height == 8 && rgba8.image.mip_levels == 4));
    CHECK(rgba8.image.data.size() == texture_bytes(rgba8.image.desc()));
    for (const auto [x, y] : {std::pair{0u, 0u}, std::pair{11u, 0u}, std::pair{5u, 7u}, std::pair{11u, 7u}}) {
        const auto texel = texel_at(rgba8.image, 0, x, y);
        CHECK(texel == std::array<uint8_t, 4>{uint8_t(20 * x), uint8_t(30 * y), uint8_t(5 * x * y), uint8_t(255 - 10 * x)});
    }
    const auto astc = read_ktx2(std::as_bytes(std::span(test::khronos_astc_ktx2)));
    INFO(astc.error);
    REQUIRE(astc);
    CHECK(astc.image.format == Format::astc_6x6_srgb);
    CHECK(astc.image.mip_levels == 4);
    CHECK(astc.image.data.size() == (2u * 2u + 1u + 1u + 1u) * 16u); // 12x8, 6x4, 3x2, 1x1
}

TEST_CASE("Malformed KTX2 files are refused with the reason", "[assets][textures]") {
    const auto valid = write_ktx2(numbered_chain());
    const auto refused = [&](auto&& change, const std::string& expected) {
        auto file = valid;
        change(file);
        const auto read = read_ktx2(file);
        INFO(expected);
        CHECK_FALSE(read);
        CHECK(read.error.find(expected) != std::string::npos);
        CHECK(read.image.data.empty());
    };
    refused([](auto& file) { file.resize(40); }, "too short to be KTX2");
    refused([](auto& file) { file[5] = std::byte{'1'}; }, "does not start with the KTX2 identifier");
    refused([](auto& file) { put(file, 12, 999); }, "vkFormat 999 is not one Maya reads");
    refused([](auto& file) { put(file, 44, 1); }, "supercompression scheme 1");
    refused([](auto& file) { put(file, 16, 4); }, "typeSize is 4");
    refused([](auto& file) { put(file, 28, 2); }, "only 2D images");
    refused([](auto& file) { put(file, 20, 0x7FFFFFFF); }, "the image is 2147483647x4; Maya reads at most 16384 on each side");
    refused([](auto& file) { put(file, 20, 16384); }, "level 0 holds 128 bytes; rgba8_srgb needs 262144"); // before allocating
    refused([](auto& file) { put(file, 36, 6); }, "cube faces are not supported");
    refused([](auto& file) { put(file, 32, 3); }, "array layers");
    refused([](auto& file) { put(file, 40, 0); }, "levelCount is 0");
    refused([](auto& file) { put(file, 40, 5); }, "5 levels is more than a 8x4 image has");
    refused([](auto& file) { put64(file, 88, 99); }, "level 0 holds 99 bytes; rgba8_srgb needs 128");
    refused([](auto& file) { put64(file, 80, file.size() - 8); }, "level 0 runs past the end");
    refused([](auto& file) { put64(file, 80 + 24 * 3, word64(file, 80 + 24 * 3) + 2); }, "level 3 is not aligned"); // stored first
    refused([](auto& file) { put(file, 48, uint32_t(file.size())); }, "data format descriptor is missing");
    refused([&](auto& file) { put(file, word(file, 48) + 12, 1u | (1u << 8) | (1u << 16)); }, "transfer function does not match vkFormat 43");
    refused([&](auto& file) { put(file, word(file, 48) + 12, 162u | (1u << 8) | (2u << 16)); }, "color model (162) does not match");
    refused([](auto& file) { file.resize(file.size() - 1); }, "runs past the end");
}

namespace {
/// A project folder with a texture descriptor and its registry on a null device.
struct TextureProject {
    Folder folder;
    NullGraphicsDevice device;
    std::unique_ptr<AssetRegistry> registry;
    explicit TextureProject(NullDeviceOptions options = {}) : device(options) {
        REQUIRE(device.initialize(nullptr, {3, 0}));
        registry = std::make_unique<AssetRegistry>(folder.root, std::make_unique<FileAssetProvider>(device));
        REQUIRE_FALSE(registry->register_asset(texture_ref, "textures/wood.texture"));
    }
    ~TextureProject() {
        registry.reset();
        device.shutdown();
    }
    void descriptor(const std::string& text) const { folder.write("textures/wood.texture", text); }
    void image(const std::string& name, const std::string& bytes) const { folder.write("textures/" + name, bytes); }
};
} // namespace

TEST_CASE("Texture assets load from source or cooked images with the settings they state", "[assets][textures]") {
    TextureProject project;
    const auto before = project.device.stats();
    project.image("wood.png", png(64, 32, brick));
    project.descriptor(canonical);
    const auto loaded = project.registry->acquire(texture_ref);
    REQUIRE(loaded);
    const auto& texture = loaded.lease.value();
    const auto& desc = texture.texture().desc();
    CHECK(desc.format == Format::astc_6x6_srgb);
    CHECK((desc.width == 64 && desc.height == 32 && desc.mip_levels == 7));
    CHECK(desc.usage == TextureUsage::sampled);
    CHECK(desc.label == "wood");
    CHECK(texture.role() == TextureRole::color);
    const auto& sampler = texture.sampler().desc();
    CHECK((sampler.mip_filter == MipFilter::linear && sampler.max_anisotropy == 8));
    CHECK((sampler.address_u == AddressMode::repeat && sampler.address_v == AddressMode::clamp_to_edge));
    CHECK(project.device.stats().textures == before.textures + 1);
    CHECK(project.device.stats().samplers == before.samplers + 1);
    CHECK(project.device.stats().texture_bytes - before.texture_bytes == texture.gpu_bytes());
    CHECK(texture.gpu_bytes() == texture_bytes(desc));
    const auto residency = project.registry->residency();
    CHECK(residency.textures == 1);
    CHECK(residency.texture_gpu_bytes == texture.gpu_bytes());

    // A cooked KTX2 source is read as it is.
    auto cooked_settings = settings(TextureRole::data, TextureCompression::rgba8);
    const auto cooked = cook_texture(source(16, 8, brick), cooked_settings);
    REQUIRE(cooked);
    project.folder.write("textures/wood.ktx2", write_ktx2(cooked.image));
    project.descriptor(with(with(with(canonical, "wood.png", "wood.ktx2"), "usage color", "usage data"), "compression astc", "compression rgba8"));
    const auto ktx2 = project.registry->reload(texture_ref);
    REQUIRE(ktx2);
    CHECK(ktx2.lease.value().texture().desc().format == Format::rgba8_unorm);
    CHECK(ktx2.lease.value().texture().desc().mip_levels == 5);
    CHECK(ktx2.lease.handle().generation == loaded.lease.handle().generation + 1);

    // Mips off: one level, sampled without mip filtering. Uppercase extensions are images too.
    project.image("WOOD.PNG", png(64, 32, brick));
    project.descriptor(with(with(with(canonical, "wood.png", "WOOD.PNG"), "mips on", "mips off"), "mip_filter linear", "mip_filter none"));
    const auto single = project.registry->reload(texture_ref);
    REQUIRE(single);
    CHECK(single.lease.value().texture().desc().mip_levels == 1);
    CHECK(single.lease.value().sampler().desc().mip_filter == MipFilter::none);

    // A device that cannot sample ASTC gets RGBA8.
    TextureProject plain({.astc = false});
    plain.image("wood.png", png(8, 8, brick));
    plain.descriptor(canonical);
    const auto fallback = plain.registry->acquire(texture_ref);
    REQUIRE(fallback);
    CHECK(fallback.lease.value().texture().desc().format == Format::rgba8_srgb);
}

TEST_CASE("Texture problems are reported with their file and leave nothing allocated", "[assets][textures]") {
    const auto refused = [](const std::function<void(TextureProject&)>& setup, AssetError code, const std::string& expected,
                            NullDeviceOptions options = {}) {
        TextureProject project(options);
        setup(project);
        const auto before = project.device.stats();
        const auto result = project.registry->acquire(texture_ref);
        INFO(expected);
        CHECK_FALSE(result);
        CHECK(result.diagnostic.code == code);
        CHECK(result.diagnostic.message.find(expected) != std::string::npos);
        CHECK(project.registry->info(texture_ref.id)->state == AssetState::failed);
        CHECK(project.device.stats().textures == before.textures);
        CHECK(project.device.stats().samplers == before.samplers);
        CHECK(project.registry->residency().texture_gpu_bytes == 0);
    };
    refused([](TextureProject&) {}, AssetError::missing_file, "Missing/unreadable asset 'textures/wood.texture'");
    refused([](TextureProject& p) { p.descriptor(with(canonical, "usage color", "usage albedo")); }, AssetError::invalid_data,
            "wood.texture: line 3: usage must be color, data, or normal");
    refused([](TextureProject& p) { p.descriptor(canonical); }, AssetError::missing_file, "wood.texture: source 'wood.png' is missing");
    refused([](TextureProject& p) {
        p.descriptor(with(canonical, "wood.png", "wood.bmp"));
        p.image("wood.bmp", "BM");
    }, AssetError::invalid_data, "source 'wood.bmp' must be a .png, .jpg, .jpeg, or .ktx2 file");
    refused([](TextureProject& p) {
        p.descriptor(canonical);
        p.image("wood.png", "\x89PNG broken");
    }, AssetError::invalid_data, "source 'wood.png': cannot decode the image");
    refused([](TextureProject& p) {
        // A symlink inside the folder that leads out of it.
        p.descriptor(with(canonical, "wood.png", "link/wood.png"));
        p.folder.write("outside/wood.png", png(4, 4, brick));
        fs::create_directory_symlink(p.folder.root / "outside", p.folder.root / "textures/link");
    }, AssetError::invalid_path, "source 'link/wood.png' leaves the texture file's folder");
    refused([](TextureProject& p) { // cooked as data, declared as color
        p.descriptor(with(with(canonical, "wood.png", "wood.ktx2"), "compression astc", "compression rgba8"));
        p.folder.write("textures/wood.ktx2", write_ktx2(cook_texture(source(8, 8, brick), settings(TextureRole::data, TextureCompression::rgba8)).image));
    }, AssetError::invalid_data, "is rgba8_unorm, but usage color with compression rgba8 needs rgba8_srgb");
    refused([](TextureProject& p) { // one level, declared with mips
        p.descriptor(with(with(canonical, "wood.png", "wood.ktx2"), "compression astc", "compression rgba8"));
        p.folder.write("textures/wood.ktx2",
                       write_ktx2(cook_texture(source(8, 8, brick), settings(TextureRole::color, TextureCompression::rgba8, false)).image));
    }, AssetError::invalid_data, "has 1 mip level(s); mips on needs 4");
    refused([](TextureProject& p) {
        p.descriptor(with(canonical, "wood.png", "wood.ktx2"));
        auto file = write_ktx2(cook_texture(source(8, 8, brick), settings(TextureRole::color, TextureCompression::astc)).image);
        file.resize(100);
        p.folder.write("textures/wood.ktx2", file);
    }, AssetError::invalid_data, "source 'wood.ktx2': the level index runs past the end of the file");
    refused([](TextureProject& p) {
        p.descriptor(with(canonical, "wood.png", "wood.ktx2"));
        p.folder.write("textures/wood.ktx2", write_ktx2(cook_texture(source(8, 8, brick), settings(TextureRole::color, TextureCompression::astc)).image));
    }, AssetError::load_failed, "is ASTC, which this device cannot sample", {.astc = false});

    // A provider that loads no textures says so.
    struct Meshes final : AssetProvider {
        AssetLoadResult<MeshAsset> load_mesh(const fs::path&) override { return {}; }
        AssetLoadResult<MaterialAsset> load_material(const fs::path&) override { return {}; }
    };
    Folder folder;
    folder.write("wood.texture", canonical);
    AssetRegistry registry(folder.root, std::make_unique<Meshes>());
    REQUIRE_FALSE(registry.register_asset(texture_ref, "wood.texture"));
    CHECK(registry.acquire(texture_ref).diagnostic.message.find("does not load textures") != std::string::npos);
}

TEST_CASE("Texture versions reload, and old versions retire only after the GPU finishes with them", "[assets][textures]") {
    TextureProject project({.manual_completion = true});
    auto& device = project.device;
    project.image("wood.png", png(64, 64, brick));
    project.descriptor(canonical);
    const auto baseline = device.stats();
    auto first = project.registry->acquire(texture_ref);
    REQUIRE(first);
    const auto first_bytes = first.lease.value().gpu_bytes();

    { // A frame that used the first version is still executing when the texture is reloaded.
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.end_frame());
    project.image("wood.png", png(32, 32, brick));
    const auto second = project.registry->reload(texture_ref);
    REQUIRE(second);
    CHECK(second.lease.value().texture().desc().width == 32);
    CHECK_FALSE(project.registry->resolve(first.lease.handle())); // the old version is no longer published
    CHECK(first.lease.value().valid()); // but its lease still owns it
    first = {};
    CHECK(device.stats().pending_retirement_bytes == first_bytes); // released only after that frame completes
    CHECK(device.stats().textures == baseline.textures + 1);
    device.complete_through(device.stats().submitted_frames);
    REQUIRE_FALSE(device.begin_frame()); // completed work is collected here
    CHECK(device.stats().pending_retirement_bytes == 0);
    REQUIRE_FALSE(device.end_frame());
    device.complete_through(device.stats().submitted_frames);

    // A failed reload keeps the published version and records why.
    project.image("wood.png", "not a png");
    const auto failed = project.registry->reload(texture_ref);
    CHECK_FALSE(failed);
    const auto info = project.registry->info(texture_ref.id);
    CHECK(info->state == AssetState::ready);
    CHECK(info->diagnostic.message.find("cannot decode the image") != std::string::npos);
    CHECK(project.registry->acquire(texture_ref).lease.value().texture().desc().width == 32);
    }

    // Load and evict cycles return texture memory to the baseline every time.
    project.image("wood.png", png(64, 64, brick));
    for (int cycle = 0; cycle < 50; ++cycle) {
        REQUIRE(project.registry->reload(texture_ref));
        CHECK(project.registry->evict_unused() == 1);
        device.wait_idle();
        const auto stats = device.stats();
        CHECK(stats.textures == baseline.textures);
        CHECK(stats.samplers == baseline.samplers);
        CHECK(stats.texture_bytes == baseline.texture_bytes);
        CHECK(stats.pending_retirements == 0);
    }
    CHECK(device.native_resources() == baseline.textures + baseline.samplers + baseline.buffers + baseline.pipelines);
}

TEST_CASE("Texture entries round-trip through the catalog", "[assets][textures]") {
    auto output = std::ostringstream{};
    write_asset_catalog(output, {{texture_ref.id, AssetKind::texture, "textures/wood.texture"}});
    CHECK(output.str() == "maya-assets 1\ntexture 6d617961 50 \"textures/wood.texture\"\n");
    auto input = std::istringstream(output.str());
    const auto read = read_asset_catalog(input);
    REQUIRE(read);
    CHECK(read.records.front().kind == AssetKind::texture);
    CHECK(std::string(asset_kind_name(AssetKind::texture)) == "texture");
    CHECK(asset_kind<TextureAsset> == AssetKind::texture);
}

TEST_CASE("The placeholder is a declared magenta and black checkerboard", "[assets][textures]") {
    const auto texels = placeholder_texture_pixels();
    REQUIRE(texels.size() == placeholder_texture_size * placeholder_texture_size * 4);
    const auto at = [&](uint32_t x, uint32_t y) {
        const auto* texel = texels.data() + (size_t{y} * placeholder_texture_size + x) * 4;
        return std::array{int(texel[0]), int(texel[1]), int(texel[2]), int(texel[3])};
    };
    CHECK(at(0, 0) == std::array{255, 0, 255, 255});
    CHECK(at(1, 1) == std::array{255, 0, 255, 255});
    CHECK(at(2, 0) == std::array{0, 0, 0, 255});
    CHECK(at(0, 2) == std::array{0, 0, 0, 255});
    CHECK(at(2, 2) == std::array{255, 0, 255, 255});
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto placeholder = make_placeholder_texture(device);
    REQUIRE(placeholder);
    CHECK(placeholder->texture().desc().format == Format::rgba8_srgb);
    CHECK(placeholder->sampler().desc().min_filter == Filter::nearest);
    CHECK(placeholder->role() == TextureRole::color);
    device.shutdown();
    CHECK_FALSE(make_placeholder_texture(device));
}
