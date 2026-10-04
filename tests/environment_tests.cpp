// Environments (#1035, docs/assets.md#environments): environment files, HDR decoding, the mappings,
// cooking (irradiance, the background, the prefiltered cube), the split-sum table, and loading.

#include "maya/assets/environment_cook.hpp"
#include "maya/assets/registry.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/hdr.hpp"
#include "support/shading.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <unistd.h>

using namespace maya;
using namespace maya::test;
namespace fs = std::filesystem;

namespace {
constexpr auto pi = std::numbers::pi_v<float>;
bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }
EnvironmentSettingsResult settings(const std::string& text) {
    auto input = std::istringstream(text);
    return read_environment_settings(input);
}
std::span<const std::byte> bytes(const std::string& text) { return std::as_bytes(std::span(text.data(), text.size())); }
/// One texel of a cooked RGBA16F level.
math::Vec3 half_texel(const std::vector<std::byte>& data, size_t offset) {
    float rgb[3];
    for (int c = 0; c < 3; ++c) {
        _Float16 h;
        std::memcpy(&h, data.data() + offset + size_t(c) * 2, 2);
        rgb[c] = float(h);
    }
    return {rgb[0], rgb[1], rgb[2]};
}
bool close(const math::Vec3& a, const math::Vec3& b, float relative) {
    const auto near = [&](float x, float y) { return std::abs(x - y) <= relative * std::max(std::abs(y), 0.1f); };
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}
#define SHOW(a, b) INFO((a).x << " " << (a).y << " " << (a).z << " vs " << (b).x << " " << (b).y << " " << (b).z)

struct Folder {
    Folder() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("maya-environments-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    void write(const std::string& name, const std::string& text) const { std::ofstream(path / name, std::ios::binary) << text; }
    fs::path path;
};
} // namespace

TEST_CASE("Environment files name a source and optional cooking settings, and refuse anything else", "[assets][environments]") {
    const auto read = settings("maya-environment 1\nsource \"sky.hdr\"\nspecular_size 64\nsamples 32\n");
    INFO(read.error);
    REQUIRE(read);
    CHECK(read.settings.source == "sky.hdr");
    CHECK(read.settings.specular_size == 64);
    CHECK(read.settings.samples == 32);
    auto written = std::ostringstream{};
    write_environment_settings(written, read.settings);
    CHECK(written.str() == "maya-environment 1\nsource \"sky.hdr\"\nspecular_size 64\nsamples 32\n");
    const auto defaults = settings("maya-environment 1\nsource \"a b.hdr\"\n");
    REQUIRE(defaults);
    CHECK(defaults.settings.source == "a b.hdr");
    CHECK(defaults.settings.specular_size == 128);
    CHECK(defaults.settings.samples == 256);
    CHECK(has(settings("maya-texture 1\n").error, "line 1"));
    CHECK(has(settings("maya-environment 1\n").error, "names no source"));
    CHECK(has(settings("maya-environment 1\nsource \"a.hdr\"\nsource \"b.hdr\"\n").error, "line 3: 'source' appears more than once"));
    CHECK(has(settings("maya-environment 1\nsource \"a.hdr\"\nspecular_size 100\n").error, "power of two"));
    CHECK(has(settings("maya-environment 1\nsource \"a.hdr\"\nspecular_size 1024\n").error, "16 to 512"));
    CHECK(has(settings("maya-environment 1\nsource \"a.hdr\"\nsamples 8\n").error, "16 to 4096"));
    CHECK(has(settings("maya-environment 1\nsource \"a.hdr\"\nexposure 2\n").error, "unknown setting 'exposure'"));
}

TEST_CASE("HDR images decode as linear floats, and must be equirectangular Radiance files", "[assets][environments]") {
    // RGBE shares one exponent per texel: about 8 bits of precision relative to its largest channel.
    const auto image = environment_image(16, [](const math::Vec3& d) { return math::Vec3{1.0f + 0.5f * d.x, 0.75f, 1.5f}; });
    const auto decoded = decode_hdr_image(bytes(radiance_file(image)));
    INFO(decoded.error);
    REQUIRE(decoded);
    CHECK(decoded.image.width == 16);
    CHECK(decoded.image.height == 8);
    for (size_t i = 0; i < image.rgb.size(); ++i) CHECK(std::abs(decoded.image.rgb[i] - image.rgb[i]) <= 0.01f);
    CHECK(has(decode_hdr_image(bytes(radiance_file(HdrImage{8, 8, std::vector<float>(192, 1.0f)}))).error, "twice as wide"));
    CHECK(has(decode_hdr_image(bytes("not an image")).error, "not a Radiance"));
    CHECK(has(decode_hdr_image(bytes(radiance_file(image)), 8).error, "at most 8"));
}

TEST_CASE("The equirectangular and cube mappings agree with each other and with the documented axes", "[assets][environments]") {
    // The image's centre lies along -Z, u grows toward +X, and v runs from up to down.
    const auto forward = equirect_direction(0.5f, 0.5f);
    CHECK(std::abs(forward.z + 1.0f) < 1e-6f);
    CHECK(equirect_direction(0.75f, 0.5f).x > 0.99f);
    CHECK(equirect_direction(0.5f, 0.0f).y > 0.99f);
    for (const auto& d : {math::Vec3{0.3f, 0.4f, -0.866f}, math::Vec3{-0.8f, 0.1f, 0.59f}, math::Vec3{0.0f, -0.9f, 0.43f}}) {
        const auto uv = equirect_uv(d.normalized());
        const auto back = equirect_direction(uv.x, uv.y);
        CHECK((back - d.normalized()).length() < 1e-5f);
    }
    // Each cube face's centre is its axis; +Y's top edge leans toward -Z, as Metal's faces are laid out.
    const math::Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (uint32_t face = 0; face < 6; ++face) CHECK((cube_direction(face, 0, 0) - axes[face]).length() < 1e-6f);
    CHECK(cube_direction(2, 0, -1).z < -0.5f);
    CHECK(cube_direction(4, 1, 0).x > 0.5f); // +Z's right edge leans toward +X
    CHECK(cube_direction(4, 0, -1).y > 0.5f); // and its top edge up
}

TEST_CASE("Irradiance from the spherical harmonics is exact for light that varies at most linearly with direction", "[assets][environments]") {
    // Constant light L gives irradiance pi L everywhere; L (1 + d.y) gives pi L (1 + 2/3 n.y).
    const auto cooked = cook_environment(environment_image(256, [](const math::Vec3& d) { return math::Vec3{0.5f} * (1.0f + d.y); }),
                                         {{}, 16, 16});
    for (const auto& n : {math::Vec3{0, 1, 0}, math::Vec3{0, -1, 0}, math::Vec3{1, 0, 0}, math::Vec3{0.6f, 0.8f, 0.0f}}) {
        const auto e = evaluate_irradiance(cooked.irradiance, n);
        const auto expected = math::Vec3{pi * 0.5f * (1.0f + 2.0f / 3.0f * n.y)};
        SHOW(e, expected);
        CHECK(close(e, expected, 0.005f));
    }
    // A sun-like spot rings; irradiance is never negative.
    const auto spot = cook_environment(environment_image(256, [](const math::Vec3& d) { return math::Vec3{d.y > 0.95f ? 1000.0f : 0.0f}; }), {{}, 16, 16});
    CHECK(evaluate_irradiance(spot.irradiance, {0, -1, 0}).x >= 0.0f);
    CHECK(evaluate_irradiance(spot.irradiance, {0, 1, 0}).x > 10.0f);
}

TEST_CASE("Cooking keeps the background with its mips and prefilters a cube whose levels widen with roughness", "[assets][environments]") {
    // Constant light: every texel of every level is that light.
    const auto uniform = cook_environment(uniform_environment(2.0f, 128), {{}, 32, 64});
    CHECK(uniform.background_width == 128);
    CHECK(uniform.background_height == 64);
    CHECK(uniform.background_levels == 8); // 128 x 64 down to 1 x 1
    CHECK(uniform.specular_levels == 5); // 32 down to 2
    CHECK(uniform.specular.size() == size_t(32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2) * 6 * 8);
    for (size_t offset = 0; offset < uniform.specular.size(); offset += 8 * 37) {
        const auto texel = half_texel(uniform.specular, offset);
        SHOW(texel, math::Vec3{2.0f});
        CHECK(close(texel, math::Vec3{2.0f}, 0.002f));
    }
    // Light by direction: the mirror level holds the light along each texel's direction.
    const auto radiance = [](const math::Vec3& d) { return math::Vec3{1.0f + d.x, 1.0f + d.y, 1.0f + d.z}; };
    const auto directional = cook_environment(environment_image(512, radiance), {{}, 32, 64});
    for (const auto& [face, s, t] : {std::tuple{0u, 0.5f, -0.25f}, std::tuple{2u, -0.75f, 0.5f}, std::tuple{5u, 0.0f, 0.0f}}) {
        const auto x = uint32_t((s + 1.0f) * 16.0f), y = uint32_t((t + 1.0f) * 16.0f);
        const auto texel = half_texel(directional.specular, (size_t(face) * 32 * 32 + size_t(y) * 32 + x) * 8);
        const auto expected = radiance(cube_direction(face, (x + 0.5f) / 16.0f - 1.0f, (y + 0.5f) / 16.0f - 1.0f));
        SHOW(texel, expected);
        CHECK(close(texel, expected, 0.02f));
    }
    // The roughest level averages much more of the sphere: toward the mean, 1.
    const auto last = size_t(32 * 32 + 16 * 16 + 8 * 8 + 4 * 4) * 6 * 8;
    const auto rough = half_texel(directional.specular, last); // +X face's first texel
    CHECK(rough.x < 1.9f);
    CHECK(rough.x > 1.0f);
}

TEST_CASE("Cooking is deterministic whatever the thread count", "[assets][environments]") {
    const auto image = environment_image(128, [](const math::Vec3& d) { return math::Vec3{1.0f + d.x * d.x, std::max(d.y, 0.0f) * 5.0f, 0.2f}; });
    const auto one = cook_environment(image, {{}, 32, 64}, 1);
    const auto many = cook_environment(image, {{}, 32, 64}, 7);
    CHECK(one.specular == many.specular);
    CHECK(one.background == many.background);
    for (size_t i = 0; i < 9; ++i) CHECK((one.irradiance[i] - many.irradiance[i]).length() == 0.0f);
}

TEST_CASE("The split-sum table matches the BRDF's specular albedo, and a smooth metal reflects everything", "[assets][environments]") {
    // F0 1 (a white metal): scale + bias is the specular's directional albedo, which shading.hpp integrates
    // over the hemisphere independently.
    for (const auto roughness : {0.25, 0.5, 0.75, 1.0})
        for (const auto NdotV : {0.2, 0.5, 0.9}) {
            const auto [scale, bias] = brdf_scale_bias(NdotV, roughness, 2048);
            const auto albedo = directional_albedo({{1, 1, 1}, 1, roughness}, NdotV, 160)[0];
            INFO("roughness " << roughness << ", NdotV " << NdotV << ": table " << scale + bias << ", integrated " << albedo);
            CHECK(std::abs(scale + bias - albedo) < 0.01);
            CHECK(scale >= 0.0);
            CHECK(bias >= 0.0);
        }
    const auto [scale, bias] = brdf_scale_bias(0.7, 0.0);
    CHECK(std::abs(scale + bias - 1.0) < 1e-6);
    const auto table = brdf_table(8, 64);
    CHECK(table.size() == 8 * 8 * 8);
}

TEST_CASE("Environments load from their files once per version, and refuse sources they cannot use", "[assets][environments]") {
    const Folder folder;
    folder.write("sky.hdr", radiance_file(uniform_environment(1.5f, 32)));
    folder.write("sky.environment", "maya-environment 1\nsource \"sky.hdr\"\nspecular_size 16\nsamples 16\n");
    folder.write("photo.environment", "maya-environment 1\nsource \"photo.png\"\n");
    folder.write("photo.png", "not really");
    folder.write("escape.environment", "maya-environment 1\nsource \"../outside.hdr\"\n");
    folder.write("gone.environment", "maya-environment 1\nsource \"gone.hdr\"\n");
    folder.write("square.hdr", radiance_file(HdrImage{8, 8, std::vector<float>(192, 1.0f)}));
    folder.write("square.environment", "maya-environment 1\nsource \"square.hdr\"\n");
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        AssetRegistry registry(folder.path, std::make_unique<FileAssetProvider>(device));
        const auto ref = [&](uint64_t id, const char* path) {
            const auto asset = AssetRef<EnvironmentAsset>{{0x656e, id}};
            REQUIRE_FALSE(registry.register_asset(asset, path));
            return asset;
        };
        const auto sky = ref(1, "sky.environment");
        const auto loaded = registry.acquire(sky);
        INFO(loaded.diagnostic.message);
        REQUIRE(loaded);
        const auto& environment = loaded.lease.value();
        CHECK(environment.background().desc().width == 32);
        CHECK(environment.background().desc().format == Format::rgba16_float);
        CHECK(environment.specular().desc().type == TextureType::cube);
        CHECK(environment.specular_levels() == 4);
        CHECK(std::abs(evaluate_irradiance(environment.irradiance(), {0, 1, 0}).x - pi * 1.5f) < 0.05f);
        CHECK(environment.cook_milliseconds() > 0.0);
        const auto residency = registry.residency();
        CHECK(residency.environments == 1);
        CHECK(residency.environment_gpu_bytes == environment.gpu_bytes());
        CHECK(environment.gpu_bytes() == texture_bytes(environment.background().desc()) + texture_bytes(environment.specular().desc()));
        // Acquiring again is the same version: cooked once.
        CHECK(&registry.acquire(sky).lease.value() == &environment);
        CHECK(registry.reload(sky).lease.handle().generation == loaded.lease.handle().generation + 1);

        const auto problem = [&](uint64_t id, const char* path) { return registry.acquire(ref(id, path)).diagnostic.message; };
        CHECK(has(problem(2, "photo.environment"), "must be a Radiance .hdr file"));
        CHECK(has(problem(3, "escape.environment"), "leaves the environment file's folder"));
        CHECK(has(problem(4, "gone.environment"), "is missing"));
        CHECK(has(problem(5, "square.environment"), "twice as wide"));
        auto catalog = std::ostringstream{};
        write_asset_catalog(catalog, registry.records());
        CHECK(has(catalog.str(), "environment 656e 1 \"sky.environment\""));
        auto input = std::istringstream(catalog.str());
        const auto read = read_asset_catalog(input);
        REQUIRE(read);
        CHECK(read.records[0].kind == AssetKind::environment);
    }
    device.shutdown();
}
