#pragma once

#include "maya/core/mesh.hpp"
#include <string>
#include <memory>

namespace maya {
struct ModelLoadResult {
    std::unique_ptr<Mesh> mesh;
    std::string diagnostic;
    MeshGeometry geometry; // CPU copy of the loaded triangles
};
class ModelLoader {
public:
    /// Initial OBJ subset: triangular faces, positive indices, optional UVs/normals.
    static ModelLoadResult load_obj_checked(GraphicsDevice& device, const std::string& path);
    static std::unique_ptr<Mesh> load_obj(GraphicsDevice& device, const std::string& path);
};
} // namespace maya
