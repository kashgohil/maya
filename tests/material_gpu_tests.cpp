// Physically based materials on Metal (#1033, docs/renderer.md#materials): each factor and map against
// the CPU reference in support/shading.hpp, read from the HDR scene target before exposure and tone mapping.

#include <catch2/catch_test_macros.hpp>
#include "maya/core/file_system.hpp"
#include "maya/core/tangents.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "support/render_scene.hpp"
#include "support/shading.hpp"
#include <cmath>
#include <cstring>

using namespace maya;
using namespace maya::test;

namespace {
constexpr uint32_t size = 32;
constexpr auto clear = std::array<double, 4>{0.2, 0.2, 0.2, 1.0};

/// A 2 x 2 quad facing +Z with texture u to the right and v down, tangents from MikkTSpace. Mirrored,
/// u runs to the left instead, as on the mirrored half of a symmetric model.
Geometry textured_quad(bool mirrored = false) {
    const auto u = [&](float x) { return mirrored ? 1.0f - x : x; };
    const Vertex corners[4] = {{{-1, -1, 0}, {0, 0, 1}, {1, 1, 1, 1}, {u(0), 1}}, {{1, -1, 0}, {0, 0, 1}, {1, 1, 1, 1}, {u(1), 1}},
                               {{1, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, {u(1), 0}}, {{-1, 1, 0}, {0, 0, 1}, {1, 1, 1, 1}, {u(0), 0}}};
    auto list = std::vector<Vertex>{};
    for (const auto i : {0, 1, 2, 0, 2, 3}) list.push_back(corners[i]);
    REQUIRE(generate_tangents(list));
    auto welded = weld_vertices(list);
    return {std::move(welded.vertices), std::move(welded.indices)};
}

uint8_t encode(double value) { return uint8_t(std::lround(std::clamp(value, 0.0, 1.0) * 255.0)); }
double srgb_decode(uint8_t value) {
    const auto c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
/// A normal map texel for a tangent-space normal: x in red, green, and blue, y in alpha.
std::array<uint8_t, 4> normal_texel(double x, double y) {
    const auto r = encode(x * 0.5 + 0.5);
    return {r, r, r, encode(y * 0.5 + 0.5)};
}
/// The tangent-space normal the shader rebuilds from that texel.
Direction decoded_normal(std::array<uint8_t, 4> texel, double scale = 1.0) {
    const auto x = texel[0] / 255.0 * 2.0 - 1.0, y = texel[3] / 255.0 * 2.0 - 1.0;
    const auto z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
    return normalize({x * scale, y * scale, z});
}

math::Quat rotation_to(const Direction& to) { // turns +Z onto `to`
    const auto axis = math::Vec3{-float(to[1]), float(to[0]), 0.0f}; // z x to
    const auto length = axis.length();
    if (length < 1e-6f) return to[2] > 0 ? math::Quat{} : math::Quat::from_axis_angle({0, 1, 0}, math::PI);
    return math::Quat::from_axis_angle(axis * (1.0f / length), std::acos(float(std::clamp(to[2], -1.0, 1.0))));
}

struct Light {
    Direction to_light;
    float intensity;
};
struct Placed {
    size_t material; // index of the fixture's material slot
    TransformComponent transform{};
    bool mirrored = false;
};
using Hdr = std::vector<std::array<float, 4>>;

/// A headless Metal device and renderer, and a project with the quads, eight material slots whose values
/// tests publish, and textures.
struct MaterialFixture {
    explicit MaterialFixture(std::map<std::string, TestImage> textures = {}) {
        REQUIRE(device.initialize(nullptr));
        auto source = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        REQUIRE_FALSE(source.empty());
        renderer = std::make_unique<Renderer>(device, std::move(source));
        auto materials = std::map<std::string, MaterialAsset>{};
        for (int i = 0; i < 8; ++i) materials.emplace("m" + std::to_string(i) + ".material", MaterialAsset{});
        auto names = std::vector<std::string>{};
        for (const auto& [name, image] : textures) names.push_back(name);
        project = std::make_unique<TestProject>(device, std::map<std::string, Geometry>{{"quad.mesh", textured_quad()},
                                                                                        {"mirrored.mesh", textured_quad(true)}},
                                                std::move(materials), std::move(textures));
        quad = project->add<MeshAsset>(1, "quad.mesh");
        mirrored = project->add<MeshAsset>(2, "mirrored.mesh");
        for (uint64_t i = 0; i < 8; ++i) slots.push_back(project->add<MaterialAsset>(10 + i, "m" + std::to_string(i) + ".material"));
        for (uint64_t i = 0; i < names.size(); ++i) texture_refs.emplace(names[i], project->add<TextureAsset>(100 + i, names[i]));
    }
    ~MaterialFixture() {
        target.reset();
        project.reset();
        renderer.reset();
        CHECK(device.take_gpu_errors().empty());
        device.shutdown();
    }
    AssetRef<TextureAsset> texture(const std::string& name) const { return texture_refs.at(name); }
    void set(size_t slot, const MaterialAsset& value) { REQUIRE_FALSE(project->registry->publish(slots[slot], value)); }

    /// Renders the placed quads, seen along -Z from far away (so V is +Z at the centre), and reads back
    /// the scene's HDR light.
    Hdr render(const std::vector<Placed>& placed, const std::vector<Light>& lights, math::Vec3 ambient = math::Vec3{0.0f},
               std::vector<RenderDiagnostic>* diagnostics = nullptr) {
        World world;
        build_world(world, [&](WorldCommands& commands) {
            for (const auto& light : lights) {
                auto entity = commands.create();
                commands.add(entity, TransformComponent{{}, rotation_to(light.to_light), {1.0f}});
                commands.add(entity, LightComponent{LightKind::directional, {1.0f}, light.intensity});
            }
            for (const auto& item : placed) {
                auto entity = commands.create();
                commands.add(entity, item.transform);
                commands.add(entity, MeshRendererComponent{item.mirrored ? mirrored : quad, slots[item.material], true});
            }
        });
        const auto snapshot = extract_render_snapshot(world, *project->registry, {ambient});
        if (diagnostics) *diagnostics = snapshot.diagnostics;
        else REQUIRE(snapshot.diagnostics.empty());
        auto camera = CameraComponent{};
        camera.vertical_fov = 0.05f;
        camera.far_clip = 200.0f;
        auto view = make_render_view(camera, look_pose({0, 0, 50}, {0, 0, 0}), size, size);
        REQUIRE(view);
        view->clear_color = clear;
        if (!target) target = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, true, "materials"});
        REQUIRE_FALSE(target->resize(size, size));
        REQUIRE_FALSE(device.begin_frame());
        const auto error = renderer->render(snapshot, *view, *target);
        INFO(error.message);
        REQUIRE_FALSE(error);
        REQUIRE_FALSE(device.end_frame());
        auto bytes = std::vector<std::byte>{};
        REQUIRE_FALSE(device.read_texture(target->scene_color(), bytes));
        REQUIRE(bytes.size() == size_t(size) * size * 8);
        auto image = Hdr(size_t(size) * size);
        for (size_t i = 0; i < image.size(); ++i)
            for (size_t c = 0; c < 4; ++c) {
                _Float16 half;
                std::memcpy(&half, bytes.data() + i * 8 + c * 2, 2);
                image[i][c] = float(half);
            }
        return image;
    }

    MetalDevice device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<TestProject> project;
    std::unique_ptr<RenderTarget> target;
    AssetRef<MeshAsset> quad, mirrored;
    std::vector<AssetRef<MaterialAsset>> slots;
    std::map<std::string, AssetRef<TextureAsset>> texture_refs;
};

std::array<float, 4> at(const Hdr& image, uint32_t x = size / 2, uint32_t y = size / 2) { return image[size_t(y) * size + x]; }
/// Within half precision and a little filtering: 1% and 0.002 absolute.
bool near(const std::array<float, 4>& shown, const Rgb& expected, double relative = 0.01) {
    for (int c = 0; c < 3; ++c)
        if (std::abs(shown[c] - expected[c]) > relative * std::abs(expected[c]) + 0.002) return false;
    return true;
}
Rgb times(Rgb value, double scale) {
    for (auto& v : value) v *= scale;
    return value;
}
Rgb plus(Rgb a, const Rgb& b) {
    for (int c = 0; c < 3; ++c) a[c] += b[c];
    return a;
}
#define SHOWN(p, e) INFO("shown " << p[0] << " " << p[1] << " " << p[2] << ", expected " << e[0] << " " << e[1] << " " << e[2])

TransformComponent turned(float yaw) { return {{}, math::Quat::from_axis_angle({0, 1, 0}, yaw), {1.0f}}; }
Direction toward(double yaw, double pitch) { // a unit direction: yaw about Y from +Z, then pitch up
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}
} // namespace

TEST_CASE("MikkTSpace gives a quad's tangent along u, and flips its bitangent's sign when u is mirrored", "[rhi][materials]") {
    const auto plain = textured_quad();
    const auto mirror = textured_quad(true);
    for (const auto& v : plain.vertices) {
        CHECK(std::abs(v.tangent.x - 1.0f) < 1e-5f);
        CHECK(v.tangent.w == 1.0f); // cross(N, T) is +Y, up the texture
    }
    for (const auto& v : mirror.vertices) {
        CHECK(std::abs(v.tangent.x + 1.0f) < 1e-5f);
        CHECK(v.tangent.w == -1.0f); // cross(N, T) is -Y, so the sign turns it back up the texture
    }
    CHECK(plain.vertices.size() == 4); // corners that agree are shared
    CHECK(plain.indices.size() == 6);
}

TEST_CASE("The reference BRDF conserves energy except at grazing angles on smooth dielectrics, as glTF's model does", "[rhi][materials]") {
    // White furnace for direct light: the directional albedo over the hemisphere. Metals never exceed 1,
    // and nothing does where the surface faces the eye (NdotV 0.7 and up). glTF's model weights diffuse
    // by 1 - F(VdotH), which barely shrinks while a smooth dielectric's specular grows toward grazing, so
    // there it exceeds 1 (docs/renderer.md#materials records the table); rough surfaces lose energy instead.
    for (const auto roughness : {0.25, 0.5, 0.75, 1.0})
        for (const auto NdotV : {0.1, 0.4, 0.7, 1.0})
            for (const auto metallic : {0.0, 1.0}) {
                const auto albedo = directional_albedo({{1, 1, 1}, metallic, roughness}, NdotV, 160)[0];
                INFO("roughness " << roughness << ", NdotV " << NdotV << ", metallic " << metallic << ": albedo " << albedo);
                if (metallic > 0 || NdotV >= 0.7) CHECK(albedo <= 1.005);
                else CHECK(albedo <= 1.36);
                CHECK(albedo > (metallic > 0 ? 0.29 : 0.96)); // single scattering: rough metals lose the most
            }
    // The excess shrinks with roughness.
    CHECK(directional_albedo({{1, 1, 1}, 0, 0.25}, 0.1, 160)[0] > directional_albedo({{1, 1, 1}, 0, 0.75}, 0.1, 160)[0] + 0.2);
}

TEST_CASE("Metal reflects a uniform environment without making energy: a white dielectric shows exactly the light", "[rhi][materials]") {
    MaterialFixture fixture;
    const auto ambient = 0.5f;
    for (const auto roughness : {0.0f, 0.3f, 0.7f, 1.0f})
        for (const auto yaw : {0.0f, 1.0f}) {
            INFO("roughness " << roughness << ", turned " << yaw);
            fixture.set(0, {{1, 1, 1, 1}, 0.0f, roughness});
            fixture.set(1, {{1, 1, 1, 1}, 1.0f, roughness});
            const auto dielectric = at(fixture.render({{0, turned(yaw)}}, {}, math::Vec3{ambient}));
            const auto metal = at(fixture.render({{1, turned(yaw)}}, {}, math::Vec3{ambient}));
            const auto NdotV = std::cos(double(yaw));
            const auto expected = times(environment({{1, 1, 1}, 1.0, roughness}, NdotV), ambient);
            SHOWN(metal, expected);
            CHECK(near(dielectric, {ambient, ambient, ambient}));
            CHECK(near(metal, expected));
            CHECK(metal[0] <= ambient * 1.001f);
        }
}

TEST_CASE("Metal shades every metallic and roughness under a directional light as the reference BRDF does", "[rhi][materials]") {
    MaterialFixture fixture;
    const Surface surfaces[] = {{{1, 1, 1}, 0, 0.3}, {{1, 1, 1}, 0, 1}, {{0.8, 0.1, 0.05}, 0, 0.5},
                                {{1.0, 0.78, 0.34}, 1, 0.3}, {{0.9, 0.9, 0.9}, 1, 0.7}, {{0.5, 0.5, 0.5}, 0.5, 0.45}};
    // Head-on, the Smith forms agree; seen at a grazing 69 degrees, height correlation shows.
    const auto lights = std::array{toward(0.0, 0.0), toward(0.5, 0.3), toward(-0.9, 0.2), toward(0.2, -1.1)};
    for (const auto& surface : surfaces)
        for (const auto yaw : {0.0f, 0.7f, 1.2f})
            for (const auto& L : lights) {
                fixture.set(0, {{float(surface.base[0]), float(surface.base[1]), float(surface.base[2]), 1.0f},
                                float(surface.metallic), float(surface.roughness)});
                const auto shown = at(fixture.render({{0, turned(yaw)}}, {{L, 2.0f}}));
                const auto N = Direction{std::sin(double(yaw)), 0, std::cos(double(yaw))};
                const auto expected = times(reflected(surface, N, {0, 0, 1}, normalize(L)), 2.0);
                INFO("base " << surface.base[0] << ", metallic " << surface.metallic << ", roughness " << surface.roughness
                     << ", turned " << yaw << ", light " << L[0] << " " << L[1] << " " << L[2]);
                SHOWN(shown, expected);
                CHECK(near(shown, expected));
            }
    // Pi lux on a white diffuse surface facing it is scene light 1, less what Fresnel sends to the highlight.
    fixture.set(0, {{1, 1, 1, 1}, 0.0f, 1.0f});
    const auto facing = at(fixture.render({{0}}, {{{0, 0, 1}, math::PI}}));
    CHECK(facing[0] > 0.95f);
    CHECK(facing[0] < 1.0f);
}

TEST_CASE("Metal multiplies base color by its map and the map's alpha, decoding sRGB", "[rhi][materials]") {
    MaterialFixture fixture({{"base.texture", solid_image({255, 128, 64, 255})}});
    auto material = MaterialAsset{{0.5f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.6f};
    material.base_color_texture = fixture.texture("base.texture");
    fixture.set(0, material);
    const auto shown = at(fixture.render({{0}}, {{{0, 0, 1}, 2.0f}}, math::Vec3{0.3f}));
    const auto surface = Surface{{0.5 * srgb_decode(255), srgb_decode(128), srgb_decode(64)}, 0, 0.6};
    const auto expected = plus(times(reflected(surface, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}), 2.0), times(environment(surface, 1.0), 0.3));
    SHOWN(shown, expected);
    CHECK(near(shown, expected));
}

TEST_CASE("Metal takes roughness from a metallic-roughness map's green and metallic from its blue", "[rhi][materials]") {
    MaterialFixture fixture({{"mr.texture", solid_image({255, 128, 255, 255}, TextureRole::data)}});
    auto material = MaterialAsset{{1.0f, 0.8f, 0.4f, 1.0f}, 0.6f, 0.8f};
    material.metallic_roughness_texture = fixture.texture("mr.texture");
    fixture.set(0, material);
    const auto L = normalize(toward(0.3, 0.2));
    const auto shown = at(fixture.render({{0}}, {{L, 3.0f}}, math::Vec3{0.2f}));
    // Red is ignored; it holds occlusion in packed maps.
    const auto surface = Surface{{1.0, 0.8, 0.4}, 0.6 * 1.0, 0.8 * (128 / 255.0)};
    const auto expected = plus(times(reflected(surface, {0, 0, 1}, {0, 0, 1}, L), 3.0), times(environment(surface, 1.0), 0.2));
    SHOWN(shown, expected);
    CHECK(near(shown, expected));
}

TEST_CASE("Metal darkens only ambient light by an occlusion map's red, by its strength", "[rhi][materials]") {
    MaterialFixture fixture({{"ao.texture", solid_image({64, 255, 255, 255}, TextureRole::data)}});
    const auto L = Direction{0, 0, 1};
    const auto surface = Surface{{0.7, 0.7, 0.7}, 0, 0.5};
    const auto direct = times(reflected(surface, {0, 0, 1}, {0, 0, 1}, L), 2.0);
    const auto ambient = times(environment(surface, 1.0), 0.4);
    for (const auto strength : {1.0f, 0.5f, 0.0f}) {
        auto material = MaterialAsset{{0.7f, 0.7f, 0.7f, 1.0f}, 0.0f, 0.5f};
        material.occlusion_texture = fixture.texture("ao.texture");
        material.occlusion_strength = strength;
        fixture.set(0, material);
        const auto shown = at(fixture.render({{0}}, {{L, 2.0f}}, math::Vec3{0.4f}));
        const auto occlusion = 1.0 + strength * (64 / 255.0 - 1.0);
        const auto expected = plus(direct, times(ambient, occlusion));
        INFO("strength " << strength);
        SHOWN(shown, expected);
        CHECK(near(shown, expected));
    }
}

TEST_CASE("Metal adds emissive light: its color times the map and the strength, unaffected by lights", "[rhi][materials]") {
    MaterialFixture fixture({{"glow.texture", solid_image({255, 128, 0, 255})}});
    auto material = MaterialAsset{{0, 0, 0, 1}, 0.0f, 1.0f};
    material.emissive = {0.5f, 1.0f, 1.0f};
    material.emissive_strength = 4.0f;
    material.emissive_texture = fixture.texture("glow.texture");
    fixture.set(0, material);
    const auto expected = Rgb{0.5 * 4.0, srgb_decode(128) * 4.0, 0.0};
    const auto dark = at(fixture.render({{0}}, {}));
    SHOWN(dark, expected);
    CHECK(near(dark, expected));
    // Without the map, the factor and strength alone.
    material.emissive_texture = {};
    fixture.set(0, material);
    CHECK(near(at(fixture.render({{0}}, {})), {2.0, 4.0, 4.0}));
}

TEST_CASE("Metal tilts normals toward +u and up the texture as glTF's normal maps say, on mirrored UVs too", "[rhi][materials]") {
    const auto right = normal_texel(0.6, 0.0), up = normal_texel(0.0, 0.6);
    MaterialFixture fixture({{"right.texture", solid_image(right, TextureRole::normal)},
                             {"up.texture", solid_image(up, TextureRole::normal)}});
    const auto surface = Surface{{0.8, 0.8, 0.8}, 0, 0.7};
    const auto check = [&](const std::string& map, bool mirrored, float scale, const Direction& expected_normal) {
        auto material = MaterialAsset{{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.7f};
        material.normal_texture = fixture.texture(map);
        material.normal_scale = scale;
        fixture.set(0, material);
        for (const auto& L : {toward(0.8, 0.0), toward(-0.8, 0.0), toward(0.0, 0.8), toward(0.0, -0.8)}) {
            const auto shown = at(fixture.render({{0, {}, mirrored}}, {{L, 2.0f}}));
            const auto expected = times(reflected(surface, expected_normal, {0, 0, 1}, normalize(L)), 2.0);
            INFO(map << (mirrored ? ", mirrored" : "") << ", scale " << scale << ", light " << L[0] << " " << L[1] << " " << L[2]);
            SHOWN(shown, expected);
            CHECK(near(shown, expected));
        }
    };
    const auto tilt = decoded_normal(right), lift = decoded_normal(up);
    check("right.texture", false, 1.0f, tilt); // toward +X, where u grows
    check("up.texture", false, 1.0f, lift); // toward +Y, up the texture
    check("right.texture", true, 1.0f, {-tilt[0], tilt[1], tilt[2]}); // u grows toward -X on the mirrored quad
    check("up.texture", true, 1.0f, lift); // up is still up
    check("right.texture", false, 0.5f, decoded_normal(right, 0.5)); // scale shrinks the tilt
    check("right.texture", false, 0.0f, {0, 0, 1}); // and zero ignores the map
    // From the light's side the tilt is plainly brighter.
    auto material = MaterialAsset{{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.7f};
    material.normal_texture = fixture.texture("right.texture");
    fixture.set(0, material);
    CHECK(at(fixture.render({{0}}, {{toward(0.8, 0.0), 2.0f}}))[0] > 1.5f * at(fixture.render({{0}}, {{toward(-0.8, 0.0), 2.0f}}))[0]);
}

TEST_CASE("Metal cuts masked surfaces out below the alpha cutoff, and keeps the rest opaque", "[rhi][materials]") {
    // Left half transparent (alpha 64), right half nearly opaque (alpha 192).
    auto split = TestImage{4, 4, {}, TextureRole::color};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) split.rgba.insert(split.rgba.end(), {255, 255, 255, uint8_t(x < 2 ? 64 : 192)});
    MaterialFixture fixture({{"split.texture", split}});
    const auto ambient = 0.5f;
    for (const auto cutoff : {0.2f, 0.5f, 0.9f}) {
        auto material = MaterialAsset{{1, 1, 1, 1}, 0.0f, 1.0f};
        material.base_color_texture = fixture.texture("split.texture");
        material.alpha_mode = AlphaMode::mask;
        material.alpha_cutoff = cutoff;
        fixture.set(0, material);
        const auto image = fixture.render({{0}}, {}, math::Vec3{ambient});
        const auto left = at(image, 10), right = at(image, 22);
        INFO("cutoff " << cutoff << ": left " << left[0] << ", right " << right[0]);
        CHECK(near(left, cutoff <= 64 / 255.0f ? Rgb{ambient, ambient, ambient} : Rgb{clear[0], clear[1], clear[2]}));
        CHECK(near(right, cutoff <= 192 / 255.0f ? Rgb{ambient, ambient, ambient} : Rgb{clear[0], clear[1], clear[2]}));
    }
    // An opaque material ignores alpha.
    auto opaque = MaterialAsset{{1, 1, 1, 0.1f}, 0.0f, 1.0f};
    opaque.base_color_texture = fixture.texture("split.texture");
    fixture.set(0, opaque);
    CHECK(near(at(fixture.render({{0}}, {}, math::Vec3{ambient}), 10), {ambient, ambient, ambient}));
}

TEST_CASE("Metal blends see-through surfaces over what is behind them, back to front", "[rhi][materials]") {
    MaterialFixture fixture;
    const auto ambient = 0.5f;
    auto red = MaterialAsset{{1, 0, 0, 0.25f}, 0.0f, 1.0f};
    red.alpha_mode = AlphaMode::blend;
    auto blue = MaterialAsset{{0, 0, 1, 0.5f}, 0.0f, 1.0f};
    blue.alpha_mode = AlphaMode::blend;
    fixture.set(0, red);
    fixture.set(1, blue);
    const auto shaded = [&](const Surface& s) { return times(environment(s, 1.0), ambient); };
    const auto over = [](const Rgb& front, double alpha, const Rgb& back) {
        return Rgb{front[0] * alpha + back[0] * (1 - alpha), front[1] * alpha + back[1] * (1 - alpha), front[2] * alpha + back[2] * (1 - alpha)};
    };
    const auto background = Rgb{clear[0], clear[1], clear[2]};
    const auto red_light = shaded({{1, 0, 0}, 0, 1}), blue_light = shaded({{0, 0, 1}, 0, 1});
    // One over the background.
    CHECK(near(at(fixture.render({{0}}, {}, math::Vec3{ambient})), over(red_light, 0.25, background)));
    // Red in front of blue, listed first: drawn after it all the same.
    const auto front = TransformComponent{{0, 0, 1}, {}, {1.0f}}, back = TransformComponent{{0, 0, -1}, {}, {1.0f}};
    const auto expected = over(red_light, 0.25, over(blue_light, 0.5, background));
    const auto shown = at(fixture.render({{0, front}, {1, back}}, {}, math::Vec3{ambient}));
    SHOWN(shown, expected);
    CHECK(near(shown, expected));
    // An opaque surface behind blended ones still hides the background, and does not hide them.
    fixture.set(2, {{1, 1, 1, 1}, 0.0f, 1.0f});
    const auto wall = at(fixture.render({{0, front}, {2, back}}, {}, math::Vec3{ambient}));
    CHECK(near(wall, over(red_light, 0.25, shaded({{1, 1, 1}, 0, 1}))));
}

TEST_CASE("Metal draws double-sided surfaces from behind with their normals turned, and culls single-sided ones", "[rhi][materials]") {
    MaterialFixture fixture;
    auto material = MaterialAsset{{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.6f};
    fixture.set(0, material);
    material.double_sided = true;
    fixture.set(1, material);
    const auto behind = turned(math::PI); // its front faces away from the camera
    const auto L = normalize(toward(0.4, 0.3));
    CHECK(near(at(fixture.render({{0, behind}}, {{L, 2.0f}})), {clear[0], clear[1], clear[2]}));
    const auto shown = at(fixture.render({{1, behind}}, {{L, 2.0f}}));
    // Seen from behind, the back is lit from the camera's side as a front would be.
    const auto expected = times(reflected({{0.8, 0.8, 0.8}, 0, 0.6}, {0, 0, 1}, {0, 0, 1}, L), 2.0);
    SHOWN(shown, expected);
    CHECK(near(shown, expected));
}

TEST_CASE("Metal draws the placeholder for missing maps and maps of the wrong role, and reports them", "[rhi][materials]") {
    MaterialFixture fixture({{"color.texture", solid_image({255, 255, 255, 255})}});
    fixture.project->add<TextureAsset>(200, "gone.texture"); // cataloged, with no file
    auto material = MaterialAsset{{1, 1, 1, 1}, 0.0f, 1.0f};
    material.normal_texture = fixture.texture("color.texture");
    material.base_color_texture = AssetRef<TextureAsset>{{0x7465, 200}};
    fixture.set(0, material);
    auto diagnostics = std::vector<RenderDiagnostic>{};
    const auto image = fixture.render({{0}}, {{{0, 0, 1}, 2.0f}}, math::Vec3{0.3f}, &diagnostics);
    CHECK(std::ranges::count(diagnostics, RenderIssue::texture_role, &RenderDiagnostic::code) == 1);
    CHECK(std::ranges::count(diagnostics, RenderIssue::missing_texture, &RenderDiagnostic::code) == 1);
    // The placeholder's magenta and black squares, two texels each, cover the quad.
    auto magenta = 0, black = 0;
    for (uint32_t x = 6; x < 26; ++x) {
        const auto p = at(image, x);
        if (p[0] > 0.2f && p[2] > 0.2f && p[1] < 0.15f) ++magenta;
        if (p[0] < 0.15f) ++black;
    }
    INFO("magenta " << magenta << ", black " << black);
    CHECK(magenta > 3);
    CHECK(black > 3);
}
