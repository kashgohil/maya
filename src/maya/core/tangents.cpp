#include "maya/core/tangents.hpp"
#include <cstring>
#include <limits>
#include <mikktspace.h>
#include <string_view>
#include <unordered_map>

namespace maya {
namespace {
std::vector<Vertex>& corners_of(const SMikkTSpaceContext* context) {
    return *static_cast<std::vector<Vertex>*>(context->m_pUserData);
}
Vertex& corner(const SMikkTSpaceContext* context, int face, int vertex) {
    return corners_of(context)[size_t(face) * 3 + size_t(vertex)];
}
} // namespace

bool generate_tangents(std::span<Vertex> corners) {
    if (corners.size() % 3 != 0 || corners.size() / 3 > size_t(std::numeric_limits<int>::max())) return false;
    if (corners.empty()) return true;
    // MikkTSpace writes as it goes; work on a copy so a failure changes nothing.
    auto working = std::vector<Vertex>(corners.begin(), corners.end());
    auto interface = SMikkTSpaceInterface{};
    interface.m_getNumFaces = [](const SMikkTSpaceContext* context) { return int(corners_of(context).size() / 3); };
    interface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
    interface.m_getPosition = [](const SMikkTSpaceContext* context, float out[], int face, int vertex) {
        const auto& p = corner(context, face, vertex).position;
        out[0] = p.x, out[1] = p.y, out[2] = p.z;
    };
    interface.m_getNormal = [](const SMikkTSpaceContext* context, float out[], int face, int vertex) {
        const auto& n = corner(context, face, vertex).normal;
        out[0] = n.x, out[1] = n.y, out[2] = n.z;
    };
    // MikkTSpace's bitangent follows increasing t. glTF's points up the texture while v grows down,
    // so it is given t = 1 - v: the sign it returns is then glTF's (three.js negates it instead).
    interface.m_getTexCoord = [](const SMikkTSpaceContext* context, float out[], int face, int vertex) {
        const auto& uv = corner(context, face, vertex).uv;
        out[0] = uv.x, out[1] = 1.0f - uv.y;
    };
    interface.m_setTSpaceBasic = [](const SMikkTSpaceContext* context, const float tangent[], float sign, int face, int vertex) {
        corner(context, face, vertex).tangent = {tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f};
    };
    auto context = SMikkTSpaceContext{&interface, &working};
    if (!genTangSpaceDefault(&context)) return false;
    std::ranges::copy(working, corners.begin());
    return true;
}

IndexedVertices weld_vertices(std::span<const Vertex> corners) {
    auto result = IndexedVertices{};
    result.indices.reserve(corners.size());
    auto seen = std::unordered_map<std::string_view, uint32_t>{};
    seen.reserve(corners.size());
    result.vertices.reserve(corners.size());
    for (const auto& vertex : corners) {
        // Keys view the input's bytes, which outlive the map; padding is always zero.
        const auto key = std::string_view(reinterpret_cast<const char*>(&vertex), sizeof(Vertex));
        const auto [found, added] = seen.try_emplace(key, uint32_t(result.vertices.size()));
        if (added) result.vertices.push_back(vertex);
        result.indices.push_back(found->second);
    }
    return result;
}
} // namespace maya
