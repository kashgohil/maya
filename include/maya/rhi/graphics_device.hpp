#pragma once

#include "maya/rhi/resource.hpp"
#include <memory>
#include <string>

namespace maya {

class GraphicsDevice {
public:
    virtual ~GraphicsDevice() = default;

    virtual bool initialize(void* native_window_handle) = 0;
    virtual void shutdown() = 0;

    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    // Resource Creation
    virtual bool create_pipeline(const std::string& shader_source) = 0;
    virtual VertexBufferHandle create_vertex_buffer(const void* data, size_t size) = 0;
    virtual IndexBufferHandle create_index_buffer(const void* data, size_t size) = 0;
    
    // Uniforms (Constants)
    virtual UniformBufferHandle create_uniform_buffer(size_t size) = 0;
    virtual void update_uniform_buffer(UniformBufferHandle handle, const void* data, size_t size) = 0;

    // Textures
    virtual TextureHandle create_texture(const void* data, uint32_t width, uint32_t height) = 0;

    // Command Execution
    virtual void bind_vertex_buffer(VertexBufferHandle handle, uint32_t slot) = 0;
    virtual void bind_uniform_buffer(UniformBufferHandle handle, uint32_t slot) = 0;
    virtual void bind_texture(TextureHandle handle, uint32_t slot) = 0;

    virtual void draw_indexed(IndexBufferHandle handle, uint32_t index_count) = 0;

    static std::unique_ptr<GraphicsDevice> create_default();
};

} // namespace maya
