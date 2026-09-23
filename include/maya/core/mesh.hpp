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
        m_vb = m_device.create_vertex_buffer(vertices.data(), vertices.size() * sizeof(Vertex));
        if (m_vb.handle == INVALID_HANDLE) return;
        try {
            m_ib = m_device.create_index_buffer(indices.data(), indices.size() * sizeof(uint32_t));
        } catch (...) {
            m_device.release_vertex_buffer(m_vb);
            m_vb = {};
            throw;
        }
        if (m_ib.handle == INVALID_HANDLE) {
            m_device.release_vertex_buffer(m_vb);
            m_vb = {};
        }
    }
    ~Mesh() {
        if (m_lifetime.expired()) return; // device/session teardown already retired its buffers
        if (m_ib.handle != INVALID_HANDLE) m_device.release_index_buffer(m_ib);
        if (m_vb.handle != INVALID_HANDLE) m_device.release_vertex_buffer(m_vb);
    }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = delete;
    Mesh& operator=(Mesh&&) = delete;

    bool valid() const noexcept {
        return !m_lifetime.expired() && m_vb.handle != INVALID_HANDLE && m_ib.handle != INVALID_HANDLE;
    }
    uint32_t index_count() const noexcept { return m_index_count; }
    void draw() const {
        if (!valid()) return;
        m_device.bind_vertex_buffer(m_vb, 0);
        m_device.draw_indexed(m_ib, m_index_count);
    }
private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    VertexBufferHandle m_vb{};
    IndexBufferHandle m_ib{};
    uint32_t m_index_count = 0;
};
} // namespace maya
