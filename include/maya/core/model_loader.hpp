#pragma once

#include "maya/core/mesh.hpp"
#include <string>
#include <memory>

namespace maya {
struct ModelLoadResult {
    std::unique_ptr<Mesh> mesh;
    std::string diagnostic;
};
class ModelLoader {
public:
    /// Initial OBJ subset: triangular faces, positive indices, optional UVs/normals.
    static ModelLoadResult load_obj_checked(GraphicsDevice& device, const std::string& path);
    static std::unique_ptr<Mesh> load_obj(GraphicsDevice& device, const std::string& path);
};
} // namespace maya
