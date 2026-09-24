#pragma once

#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/vertex.hpp"
#include <limits>
#include <stdexcept>
#include <vector>

namespace maya {
/// Owns buffer allocations. Share a mesh through an asset lease, never copy its handles.
class Mesh {
public:
    Mesh(GraphicsDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
        : m_device(device), m_lifetime(device.resource_lifetime()) {
        if (indices.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Mesh index count exceeds uint32_t");
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
    }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = delete;
    Mesh& operator=(Mesh&&) = delete;

    bool valid() const noexcept { return !m_lifetime.expired() && m_vb.valid() && m_ib.valid(); }
    uint32_t index_count() const noexcept { return m_index_count; }
    BufferHandle vertex_buffer() const noexcept { return m_vb; }
    BufferHandle index_buffer() const noexcept { return m_ib; }
    /// Binds vertices at buffer index 0 and draws. Requires an open pass with a pipeline set.
    RhiDiagnostic draw() const {
        if (!valid()) return {RhiError::stale_handle, "Mesh has no GPU buffers (upload failed or device session ended)"};
        if (auto error = m_device.set_vertex_buffer(0, m_vb)) return error;
        return m_device.draw_indexed(m_ib, IndexType::uint32, m_index_count);
    }
private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    BufferHandle m_vb{};
    BufferHandle m_ib{};
    uint32_t m_index_count = 0;
};
} // namespace maya
