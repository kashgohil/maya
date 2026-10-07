#pragma once

#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/vertex.hpp"
#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace maya {
/// CPU copy of a mesh's triangles in local space, kept for picking and bounds. It costs 12 bytes
/// per vertex and 4 per index, and empty geometry simply cannot be picked.
struct MeshGeometry {
    std::vector<math::Vec3> positions;
    std::vector<uint32_t> indices; // triangles, three per face
    math::Vec3 min{0.0f}, max{0.0f}; // local bounds
    bool empty() const noexcept { return indices.empty(); }
    static MeshGeometry from(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
        auto geometry = MeshGeometry{};
        geometry.positions.reserve(vertices.size());
        for (const auto& vertex : vertices) geometry.positions.push_back(vertex.position);
        geometry.indices = indices;
        if (!geometry.positions.empty()) geometry.min = geometry.max = geometry.positions.front();
        for (const auto& p : geometry.positions) {
            geometry.min = {std::min(geometry.min.x, p.x), std::min(geometry.min.y, p.y), std::min(geometry.min.z, p.z)};
            geometry.max = {std::max(geometry.max.x, p.x), std::max(geometry.max.y, p.y), std::max(geometry.max.z, p.z)};
        }
        return geometry;
    }
};

/// Owns buffer allocations. Share a mesh through an asset lease, never copy its handles.
class Mesh {
public:
    /// A skinned mesh (#1038) also has one SkinVertex per vertex: joints and weights, a second stream.
    Mesh(GraphicsDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
         std::span<const SkinVertex> skin = {})
        : m_device(device), m_lifetime(device.resource_lifetime()) {
        if (indices.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Mesh index count exceeds uint32_t");
        if (!skin.empty() && skin.size() != vertices.size()) throw std::invalid_argument("A skinned mesh needs one SkinVertex per vertex");
        m_index_count = static_cast<uint32_t>(indices.size());
        if (vertices.empty() || indices.empty() || m_lifetime.expired()) return;
        auto vertex_buffer = m_device.create_buffer(
            {vertices.size() * sizeof(Vertex), BufferUsage::vertex, "mesh vertices"}, vertices.data());
        if (!vertex_buffer) return;
        try {
            auto index_buffer = m_device.create_buffer(
                {indices.size() * sizeof(uint32_t), BufferUsage::index, "mesh indices"}, indices.data());
            if (!index_buffer) {
                m_device.destroy(vertex_buffer.handle);
                return;
            }
            if (!skin.empty()) {
                auto skin_buffer = m_device.create_buffer({skin.size() * sizeof(SkinVertex), BufferUsage::vertex, "mesh skin"}, skin.data());
                if (!skin_buffer) {
                    m_device.destroy(index_buffer.handle);
                    m_device.destroy(vertex_buffer.handle);
                    return;
                }
                m_sb = skin_buffer.handle;
            }
            m_vb = vertex_buffer.handle;
            m_ib = index_buffer.handle;
        } catch (...) {
            m_device.destroy(vertex_buffer.handle);
            throw;
        }
    }
    ~Mesh() {
        if (m_lifetime.expired()) return; // device/session teardown already retired its buffers
        m_device.destroy(m_ib);
        m_device.destroy(m_vb);
        if (m_sb.valid()) m_device.destroy(m_sb);
    }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = delete;
    Mesh& operator=(Mesh&&) = delete;

    bool valid() const noexcept { return !m_lifetime.expired() && m_vb.valid() && m_ib.valid(); }
    uint32_t index_count() const noexcept { return m_index_count; }
    bool skinned() const noexcept { return m_sb.valid(); }
    /// Bytes of its vertex and index buffers, from their descriptors; 0 without buffers.
    size_t gpu_bytes() const noexcept {
        if (!valid()) return 0;
        const auto* vertices = m_device.describe(m_vb);
        const auto* indices = m_device.describe(m_ib);
        const auto* skin = m_sb.valid() ? m_device.describe(m_sb) : nullptr;
        return (vertices ? vertices->size : 0) + (indices ? indices->size : 0) + (skin ? skin->size : 0);
    }
    BufferHandle vertex_buffer() const noexcept { return m_vb; }
    BufferHandle index_buffer() const noexcept { return m_ib; }
    /// Binds vertices at buffer index 0 and draws `instances` copies, numbered from `first_instance`.
    /// Requires an open pass with a pipeline set.
    RhiDiagnostic draw(uint32_t instances = 1, uint32_t first_instance = 0) const {
        if (!valid()) return {RhiError::stale_handle, "Mesh has no GPU buffers (upload failed or device session ended)"};
        if (auto error = m_device.set_vertex_buffer(0, m_vb)) return error;
        if (m_sb.valid())
            if (auto error = m_device.set_vertex_buffer(5, m_sb)) return error; // the skinned vertex shaders read it
        return m_device.draw_indexed(m_ib, IndexType::uint32, m_index_count, 0, instances, first_instance);
    }
private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    BufferHandle m_vb{};
    BufferHandle m_ib{};
    BufferHandle m_sb{}; // skin stream, skinned meshes only
    uint32_t m_index_count = 0;
};
} // namespace maya
