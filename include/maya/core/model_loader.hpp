#pragma once

#include "maya/core/mesh.hpp"
#include <string>
#include <memory>
#include <vector>

namespace maya {
struct ModelLoadResult {
    std::unique_ptr<Mesh> mesh;
    std::string diagnostic;
    MeshGeometry geometry; // CPU copy of the loaded triangles
};
/// An OBJ's welded triangles, with MikkTSpace tangents, before they reach a device (#1039: packages cook them).
struct ObjParseResult {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::string diagnostic; // why it could not be read; empty on success
    explicit operator bool() const noexcept { return diagnostic.empty(); }
};
class ModelLoader {
public:
    /// Parses without a device: what load_obj_checked uploads.
    static ObjParseResult parse_obj(const std::string& path);
    /// Initial OBJ subset: triangular faces, positive indices, optional UVs/normals.
    static ModelLoadResult load_obj_checked(GraphicsDevice& device, const std::string& path);
    static std::unique_ptr<Mesh> load_obj(GraphicsDevice& device, const std::string& path);
};
} // namespace maya
