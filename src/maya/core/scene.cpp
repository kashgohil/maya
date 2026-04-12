#include "maya/core/scene.hpp"
#include "maya/core/mesh.hpp"
#include "maya/core/texture.hpp"

namespace maya {

namespace {

struct PerObjectUniforms {
    math::Mat4 model_matrix;
    math::Mat4 view_projection_matrix;
};

} // namespace

Mesh* Scene::add_mesh(std::unique_ptr<Mesh> mesh) {
    Mesh* ptr = mesh.get();
    m_mesh_storage.push_back(std::move(mesh));
    return ptr;
}

void Scene::add_object(std::unique_ptr<Mesh> mesh, Material material) {
    Mesh* ptr = add_mesh(std::move(mesh));
    m_objects.push_back(SceneObject{ptr, material, math::Mat4::identity()});
}

void Scene::render(GraphicsDevice& device, UniformBufferHandle uniform_buffer,
    const math::Mat4& view_projection) const {
    for (const SceneObject& obj : m_objects) {
        if (!obj.mesh) {
            continue;
        }
        PerObjectUniforms uniforms;
        uniforms.model_matrix = obj.model_matrix;
        uniforms.view_projection_matrix = view_projection;
        device.update_uniform_buffer(uniform_buffer, &uniforms, sizeof(PerObjectUniforms));
        device.bind_pipeline(obj.material.pipeline);
        device.bind_uniform_buffer(uniform_buffer, 1);
        if (obj.material.texture) {
            obj.material.texture->bind(0);
        }
        obj.mesh->draw();
    }
}

} // namespace maya
