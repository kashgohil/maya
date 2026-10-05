#pragma once

#include "maya/rhi/vertex.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace maya {
/// Writes every corner's tangent in a triangle list (three vertices per triangle, unindexed) with
/// MikkTSpace, from positions, normals, and texture coordinates, in glTF's convention (see
/// Vertex::tangent). Returns false, leaving the tangents unchanged, if the list is not whole triangles
/// or MikkTSpace fails (it allocates). Corners whose triangle's texture coordinates do not span it
/// (none, or all on a line) get some unit tangent perpendicular to their normal.
bool generate_tangents(std::span<Vertex> corners);

struct IndexedVertices {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // one per corner
};
/// Shares bitwise-identical corners, in the order each first appears.
IndexedVertices weld_vertices(std::span<const Vertex> corners);
} // namespace maya
