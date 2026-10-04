#pragma once

#include "maya/assets/environment_cook.hpp"
#include "maya/assets/registry.hpp"
#include "maya/world/world.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unistd.h>

namespace maya::test {

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

/// Unit cube (half extent 0.5), one normal per face, counter-clockwise seen from outside.
inline Geometry unit_cube(const math::Vec4& color = {1, 1, 1, 1}) {
    const math::Vec3 faces[6][3] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                                    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                                    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    auto geometry = Geometry{};
    for (const auto& [n, u, v] : faces) {
        const auto base = static_cast<uint32_t>(geometry.vertices.size());
        constexpr float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& [a, b] : corners)
            geometry.vertices.emplace_back(n * 0.5f + u * (0.5f * a) + v * (0.5f * b), n, color);
        for (const auto i : {0u, 1u, 2u, 0u, 2u, 3u}) geometry.indices.push_back(base + i);
    }
    return geometry;
}

/// A 2x0.5 quad through the origin whose object-space normal is (0,1,1)/sqrt(2).
inline Geometry slanted_quad() {
    const auto n = math::Vec3(0.0f, 1.0f, 1.0f).normalized();
    auto geometry = Geometry{};
    const math::Vec3 corners[] = {{-1, -0.25f, 0.25f}, {1, -0.25f, 0.25f}, {1, 0.25f, -0.25f}, {-1, 0.25f, -0.25f}};
    for (const auto& p : corners)
        geometry.vertices.emplace_back(p, n, math::Vec4{1, 1, 1, 1});
    geometry.indices = {0, 1, 2, 0, 2, 3};
    return geometry;
}

/// A texture served from memory: one level of RGBA8 texels, sampled with nearest filtering and repeat.
struct TestImage {
    uint32_t width = 1, height = 1;
    std::vector<uint8_t> rgba; // rows top to bottom
    TextureRole role = TextureRole::color; // color is sRGB; data and normal are linear
};
/// A size x size texture of one texel value.
inline TestImage solid_image(std::array<uint8_t, 4> texel, TextureRole role = TextureRole::color, uint32_t size = 4) {
    auto image = TestImage{size, size, {}, role};
    for (uint32_t i = 0; i < size * size; ++i) image.rgba.insert(image.rgba.end(), texel.begin(), texel.end());
    return image;
}

/// Serves meshes, materials, and textures from memory, keyed by catalog file name. The registry still requires
/// each file to exist, so missing or removed files behave like missing project content.
class InlineProvider final : public AssetProvider {
public:
    InlineProvider(GraphicsDevice& device, std::map<std::string, Geometry> meshes,
                   std::map<std::string, MaterialAsset> materials, std::shared_ptr<size_t> loads,
                   std::map<std::string, TestImage> textures = {}, std::map<std::string, HdrImage> environments = {})
        : m_device(device), m_meshes(std::move(meshes)), m_materials(std::move(materials)), m_textures(std::move(textures)),
          m_environments(std::move(environments)), m_loads(std::move(loads)) {}
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override {
        ++*m_loads;
        const auto found = m_meshes.find(path.filename().string());
        if (found == m_meshes.end()) return {{}, {AssetError::invalid_data, "no inline mesh " + path.string()}};
        auto mesh = std::make_unique<Mesh>(m_device, found->second.vertices, found->second.indices);
        if (!mesh->valid()) return {{}, {AssetError::device_unavailable, "mesh upload failed"}};
        return {std::make_shared<const MeshAsset>(std::move(mesh),
                    MeshGeometry::from(found->second.vertices, found->second.indices)), {}};
    }
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& path) override {
        ++*m_loads;
        const auto found = m_materials.find(path.filename().string());
        if (found == m_materials.end()) return {{}, {AssetError::invalid_data, "no inline material " + path.string()}};
        return {std::make_shared<const MaterialAsset>(found->second), {}};
    }
    AssetLoadResult<TextureAsset> load_texture(const std::filesystem::path& path) override {
        ++*m_loads;
        const auto found = m_textures.find(path.filename().string());
        if (found == m_textures.end()) return {{}, {AssetError::invalid_data, "no inline texture " + path.string()}};
        const auto& image = found->second;
        const auto format = image.role == TextureRole::color ? Format::rgba8_srgb : Format::rgba8_unorm;
        auto texture = std::make_unique<Texture>(m_device, TextureDesc{image.width, image.height, format, TextureUsage::sampled, path.stem().string()},
                                                 std::as_bytes(std::span(image.rgba)));
        auto sampler = std::make_unique<Sampler>(m_device, SamplerDesc{Filter::nearest, Filter::nearest, AddressMode::repeat,
                                                                       AddressMode::repeat, path.stem().string()});
        if (!texture->valid() || !sampler->valid()) return {{}, {AssetError::device_unavailable, "texture upload failed"}};
        return {std::make_shared<const TextureAsset>(std::move(texture), std::move(sampler), image.role), {}};
    }
    /// Cooks the image as an environment file would, with a 32-texel cube.
    AssetLoadResult<EnvironmentAsset> load_environment(const std::filesystem::path& path) override {
        ++*m_loads;
        const auto found = m_environments.find(path.filename().string());
        if (found == m_environments.end()) return {{}, {AssetError::invalid_data, "no inline environment " + path.string()}};
        const auto cooked = cook_environment(found->second, {{}, 32, 128});
        auto background = std::make_unique<Texture>(m_device, TextureDesc{cooked.background_width, cooked.background_height,
            Format::rgba16_float, TextureUsage::sampled, "background", cooked.background_levels}, cooked.background);
        auto specular = std::make_unique<Texture>(m_device, TextureDesc{cooked.specular_size, cooked.specular_size,
            Format::rgba16_float, TextureUsage::sampled, "specular", cooked.specular_levels, TextureType::cube}, cooked.specular);
        auto sampler = std::make_unique<Sampler>(m_device, SamplerDesc{Filter::linear, Filter::linear, AddressMode::repeat,
            AddressMode::clamp_to_edge, "environment", MipFilter::linear});
        if (!background->valid() || !specular->valid() || !sampler->valid()) return {{}, {AssetError::device_unavailable, "upload failed"}};
        return {std::make_shared<const EnvironmentAsset>(std::move(background), std::move(specular), std::move(sampler),
                                                         cooked.irradiance, cooked.milliseconds), {}};
    }

private:
    GraphicsDevice& m_device;
    std::map<std::string, Geometry> m_meshes;
    std::map<std::string, MaterialAsset> m_materials;
    std::map<std::string, TestImage> m_textures;
    std::map<std::string, HdrImage> m_environments;
    std::shared_ptr<size_t> m_loads;
};

/// A temporary project directory whose registry uses InlineProvider. Removed on destruction.
class TestProject {
public:
    TestProject(GraphicsDevice& device, std::map<std::string, Geometry> meshes,
                std::map<std::string, MaterialAsset> materials = {}, std::map<std::string, TestImage> textures = {},
                std::map<std::string, HdrImage> environments = {}) {
        static std::atomic<int> counter{0};
        m_root = std::filesystem::temp_directory_path() /
            ("maya-render-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(m_root);
        for (const auto& [name, geometry] : meshes) touch(name);
        for (const auto& [name, material] : materials) touch(name);
        for (const auto& [name, texture] : textures) touch(name);
        for (const auto& [name, environment] : environments) touch(name);
        registry = std::make_unique<AssetRegistry>(m_root, std::make_unique<InlineProvider>(device, std::move(meshes),
            std::move(materials), loads, std::move(textures), std::move(environments)));
    }
    ~TestProject() {
        registry.reset();
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }
    TestProject(const TestProject&) = delete;
    TestProject& operator=(const TestProject&) = delete;

    void touch(const std::string& name) { std::ofstream(m_root / name) << "inline\n"; }
    void remove(const std::string& name) { std::filesystem::remove(m_root / name); }
    template<Asset T> AssetRef<T> add(uint64_t id, const std::string& name) {
        const auto ref = AssetRef<T>{{0x7465, id}};
        REQUIRE_FALSE(registry->register_asset(ref, name));
        return ref;
    }

    std::shared_ptr<size_t> loads = std::make_shared<size_t>(0);
    std::unique_ptr<AssetRegistry> registry;

private:
    std::filesystem::path m_root;
};

/// Commits one batch and returns the created handles in creation order.
template<class Build>
std::vector<EntityHandle> build_world(World& world, Build&& build) {
    auto commands = world.commands();
    build(commands);
    auto result = world.commit(commands);
    REQUIRE(result);
    return result.created;
}

/// A rigid camera pose at `eye` looking at `target`.
inline math::Mat4 look_pose(const math::Vec3& eye, const math::Vec3& target, const math::Vec3& up = {0, 1, 0}) {
    const auto pose = inverse_affine(math::Mat4::look_at(eye, target, up));
    REQUIRE(pose);
    return *pose;
}
inline TransformComponent look_transform(const math::Vec3& eye, const math::Vec3& target,
                                         const math::Vec3& up = {0, 1, 0}) {
    const auto transform = decompose_transform(look_pose(eye, target, up));
    REQUIRE(transform);
    return *transform;
}

} // namespace maya::test
