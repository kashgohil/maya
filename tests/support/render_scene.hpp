#pragma once

#include "maya/assets/registry.hpp"
#include "maya/world/world.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

/// Serves meshes and materials from memory, keyed by catalog file name. The registry still requires
/// each file to exist, so missing or removed files behave like missing project content.
class InlineProvider final : public AssetProvider {
public:
    InlineProvider(GraphicsDevice& device, std::map<std::string, Geometry> meshes,
                   std::map<std::string, MaterialAsset> materials, std::shared_ptr<size_t> loads)
        : m_device(device), m_meshes(std::move(meshes)), m_materials(std::move(materials)), m_loads(std::move(loads)) {}
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override {
        ++*m_loads;
        const auto found = m_meshes.find(path.filename().string());
        if (found == m_meshes.end()) return {{}, {AssetError::invalid_data, "no inline mesh " + path.string()}};
        auto mesh = std::make_unique<Mesh>(m_device, found->second.vertices, found->second.indices);
        if (!mesh->valid()) return {{}, {AssetError::device_unavailable, "mesh upload failed"}};
        return {std::make_shared<const MeshAsset>(std::move(mesh)), {}};
    }
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& path) override {
        ++*m_loads;
        const auto found = m_materials.find(path.filename().string());
        if (found == m_materials.end()) return {{}, {AssetError::invalid_data, "no inline material " + path.string()}};
        return {std::make_shared<const MaterialAsset>(found->second), {}};
    }

private:
    GraphicsDevice& m_device;
    std::map<std::string, Geometry> m_meshes;
    std::map<std::string, MaterialAsset> m_materials;
    std::shared_ptr<size_t> m_loads;
};

/// A temporary project directory whose registry uses InlineProvider. Removed on destruction.
class TestProject {
public:
    TestProject(GraphicsDevice& device, std::map<std::string, Geometry> meshes,
                std::map<std::string, MaterialAsset> materials = {}) {
        static std::atomic<int> counter{0};
        m_root = std::filesystem::temp_directory_path() /
            ("maya-render-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(m_root);
        for (const auto& [name, geometry] : meshes) touch(name);
        for (const auto& [name, material] : materials) touch(name);
        registry = std::make_unique<AssetRegistry>(m_root,
            std::make_unique<InlineProvider>(device, std::move(meshes), std::move(materials), loads));
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
