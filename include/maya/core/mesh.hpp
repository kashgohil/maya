#pragma once

#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/vertex.hpp"
#include <vector>

namespace maya {

class Mesh {
public:
    Mesh(GraphicsDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
        : m_device(device), m_index_count(static_cast<uint32_t>(indices.size())) {
        m_vb = m_device.create_vertex_buffer(vertices.data(), vertices.size() * sizeof(Vertex));
        m_ib = m_device.create_index_buffer(indices.data(), indices.size() * sizeof(uint32_t));
    }

    void draw() {
        m_device.bind_vertex_buffer(m_vb, 0);
        m_device.draw_indexed(m_ib, m_index_count);
    }

private:
    GraphicsDevice& m_device;
    VertexBufferHandle m_vb;
    IndexBufferHandle m_ib;
    uint32_t m_index_count;
};

} // namespace maya
