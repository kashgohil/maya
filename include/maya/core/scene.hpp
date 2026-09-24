#pragma once

#include "maya/core/material.hpp"
#include "maya/assets/asset.hpp"
#include "maya/core/scene_draw_uniforms.hpp"
#include "maya/math/matrix.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/resource.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace maya {

class Mesh;

/// Per-object CPU state updated each frame before `render`.
struct SceneObject {
    const Mesh* mesh = nullptr;
    Material material{};
    math::Mat4 model_matrix = math::Mat4::identity();
};

/// Owns meshes and a flat list of drawable objects (no hierarchy yet).
class Scene {
public:
    /// Takes mesh ownership; returns raw pointer stored in this scene.
    Mesh* add_mesh(std::unique_ptr<Mesh> mesh);

    void add_object(std::unique_ptr<Mesh> mesh, Material material);
    void add_object(AssetLease<MeshAsset> mesh, Material material);

    std::vector<SceneObject>& objects() { return m_objects; }
    const std::vector<SceneObject>& objects() const { return m_objects; }

    uint32_t drawable_count() const { return static_cast<uint32_t>(m_objects.size()); }

    /// Inside an open render pass: binds pipeline, uniforms, optional texture, and draws every object.
    /// Every draw rewrites offset 0 of `uniform_buffer`; per-draw allocation is #997.
    /// Returns the first device error, leaving later objects undrawn.
    RhiDiagnostic render(GraphicsDevice& device, BufferHandle uniform_buffer,
        const math::Mat4& view_projection, const DirectionalLighting& lighting,
        const math::Vec3& camera_position_world) const;

private:
    std::vector<std::unique_ptr<Mesh>> m_mesh_storage;
    std::vector<AssetLease<MeshAsset>> m_asset_storage;
    std::vector<SceneObject> m_objects;
};

} // namespace maya
