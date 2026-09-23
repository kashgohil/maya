#include "maya/core/model_loader.hpp"
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <tuple>

namespace maya {
namespace {
struct ObjIndex {
    int v = -1, vt = -1, vn = -1;
    auto operator<=>(const ObjIndex&) const = default;
};
bool index_value(std::string_view text, size_t count, int& result) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() ||
        value <= 0 || static_cast<size_t>(value) > count) return false;
    result = value-1;
    return true;
}
bool parse_index(std::string_view text, size_t positions, size_t uvs, size_t normals, ObjIndex& out) {
    const auto first = text.find('/');
    if (first == std::string_view::npos) return index_value(text, positions, out.v);
    if (!index_value(text.substr(0,first), positions, out.v)) return false;
    const auto second = text.find('/',first+1);
    if (second == std::string_view::npos) return index_value(text.substr(first+1),uvs,out.vt);
    if (second > first+1 && !index_value(text.substr(first+1,second-first-1),uvs,out.vt)) return false;
    return index_value(text.substr(second+1),normals,out.vn);
}
}

ModelLoadResult ModelLoader::load_obj_checked(GraphicsDevice& device, const std::string& path) {
    auto file = std::ifstream(path);
    if (!file) return {{}, "Cannot open OBJ: " + path};
    std::vector<math::Vec3> positions, normals;
    std::vector<math::Vec2> uvs;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::map<ObjIndex,uint32_t> index_map;
    std::string line;
    size_t line_number = 0;
    const auto fail = [&](const std::string& why) -> ModelLoadResult {
        return {{}, path + ":" + std::to_string(line_number) + ": " + why};
    };
    while (std::getline(file,line)) {
        ++line_number;
        if (const auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        auto stream = std::istringstream(line);
        std::string type;
        if (!(stream >> type)) continue;
        if (type == "v" || type == "vn") {
            float x=0,y=0,z=0;
            if (!(stream >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                return fail("Expected three finite coordinates");
            (type == "v" ? positions : normals).push_back({x,y,z});
        } else if (type == "vt") {
            float u=0,v=0;
            if (!(stream >> u >> v) || !std::isfinite(u) || !std::isfinite(v))
                return fail("Expected two finite texture coordinates");
            uvs.push_back({u,1-v});
        } else if (type == "f") {
            std::string segments[3], extra;
            if (!(stream >> segments[0] >> segments[1] >> segments[2]) || stream >> extra)
                return fail("Only triangular faces are supported; triangulate the source mesh");
            for (const auto& segment : segments) {
                ObjIndex index;
                if (!parse_index(segment,positions.size(),uvs.size(),normals.size(),index))
                    return fail("Invalid face index '" + segment + "'; expected existing positive OBJ indices");
                auto found = index_map.find(index);
                if (found == index_map.end()) {
                    if (vertices.size() >= std::numeric_limits<uint32_t>::max())
                        return fail("Too many unique vertices");
                    const auto id = static_cast<uint32_t>(vertices.size());
                    found = index_map.emplace(index,id).first;
                    vertices.emplace_back(positions[index.v], index.vn < 0 ? math::Vec3{} : normals[index.vn],
                        math::Vec4{1,1,1,1}, index.vt < 0 ? math::Vec2{} : uvs[index.vt]);
                }
                indices.push_back(found->second);
            }
        }
    }
    if (file.bad()) return fail("I/O failure while reading OBJ");
    if (indices.empty()) return fail("OBJ contains no triangular faces");
    auto mesh = std::make_unique<Mesh>(device,vertices,indices);
    if (!mesh->valid()) return fail("GPU mesh allocation failed or device session is unavailable");
    return {std::move(mesh), {}};
}

} // namespace maya
