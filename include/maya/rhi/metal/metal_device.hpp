#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <memory>

namespace maya {

class MetalDevice : public GraphicsDevice {
public:
    MetalDevice();
    ~MetalDevice() override;

    bool initialize(void* native_window_handle) override;
    void shutdown() override;

    void resize(uint32_t width, uint32_t height) override;

    void begin_frame() override;
    void end_frame() override;

    PipelineHandle create_pipeline(const std::string& shader_source,
        const std::string& vertex_entry = "vertexMain",
        const std::string& fragment_entry = "fragmentMain") override;
    void bind_pipeline(PipelineHandle handle) override;
    VertexBufferHandle create_vertex_buffer(const void* data, size_t size) override;
    IndexBufferHandle create_index_buffer(const void* data, size_t size) override;
    
    UniformBufferHandle create_uniform_buffer(size_t size) override;
    void update_uniform_buffer(UniformBufferHandle handle, const void* data, size_t size) override;

    TextureHandle create_texture(const void* data, uint32_t width, uint32_t height) override;

    void bind_vertex_buffer(VertexBufferHandle handle, uint32_t slot) override;
    void bind_uniform_buffer(UniformBufferHandle handle, uint32_t slot) override;
    void bind_texture(TextureHandle handle, uint32_t slot) override;

    void draw_indexed(IndexBufferHandle handle, uint32_t index_count) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace maya
