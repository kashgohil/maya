#include "maya/core/model_loader.hpp"
#include "maya/core/file_system.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <map>

namespace maya {

struct ObjIndex {
    int v, vt, vn;
    bool operator<(const ObjIndex& other) const {
        if (v != other.v) return v < other.v;
        if (vt != other.vt) return vt < other.vt;
        return vn < other.vn;
    }
};

std::unique_ptr<Mesh> ModelLoader::load_obj(GraphicsDevice& device, const std::string& path) {
    std::string content = FileSystem::read_text(path);
    if (content.empty()) {
        std::cerr << "Failed to read OBJ file: " << path << std::endl;
        return nullptr;
    }

    std::vector<math::Vec3> positions;
    std::vector<math::Vec2> uvs;
    std::vector<math::Vec3> normals;
    
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::map<ObjIndex, uint32_t> index_map;

    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        std::stringstream ls(line);
        std::string type;
        if (!(ls >> type)) continue;

        if (type == "v") {
            float x, y, z;
            ls >> x >> y >> z;
            positions.push_back({x, y, z});
        } else if (type == "vt") {
            float u, v;
            ls >> u >> v;
            uvs.push_back({u, 1.0f - v}); // Flip V for most graphics APIs
        } else if (type == "vn") {
            float x, y, z;
            ls >> x >> y >> z;
            normals.push_back({x, y, z});
        } else if (type == "f") {
            for (int i = 0; i < 3; ++i) {
                std::string segment;
                ls >> segment;
                
                ObjIndex idx = {-1, -1, -1};
                size_t first_slash = segment.find('/');
                
                if (first_slash == std::string::npos) {
                    // f v1 v2 v3
                    idx.v = std::stoi(segment) - 1;
                } else {
                    idx.v = std::stoi(segment.substr(0, first_slash)) - 1;
                    size_t second_slash = segment.find('/', first_slash + 1);
                    
                    if (second_slash == std::string::npos) {
                        // f v1/vt1 v2/vt2 v3/vt3
                        idx.vt = std::stoi(segment.substr(first_slash + 1)) - 1;
                    } else {
                        // f v1/vt1/vn1 or f v1//vn1
                        if (second_slash > first_slash + 1) {
                            idx.vt = std::stoi(segment.substr(first_slash + 1, second_slash - first_slash - 1)) - 1;
                        }
                        idx.vn = std::stoi(segment.substr(second_slash + 1)) - 1;
                    }
                }

                if (index_map.find(idx) == index_map.end()) {
                    uint32_t new_index = static_cast<uint32_t>(vertices.size());
                    index_map[idx] = new_index;
                    
                    math::Vec3 pos = positions[idx.v];
                    math::Vec2 uv = (idx.vt >= 0) ? uvs[idx.vt] : math::Vec2{0, 0};
                    math::Vec3 norm = (idx.vn >= 0) ? normals[idx.vn] : math::Vec3{0, 0, 0};
                    
                    vertices.emplace_back(pos, norm, math::Vec4{1.0f, 1.0f, 1.0f, 1.0f}, uv);
                }
                indices.push_back(index_map[idx]);
            }
        }
    }

    return std::make_unique<Mesh>(device, vertices, indices);
}

} // namespace maya
