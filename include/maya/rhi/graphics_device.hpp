#pragma once

#include "maya/rhi/resource.hpp"
#include <memory>
#include <string>

namespace maya {

struct GraphicsResourceLifetime {};

class GraphicsDevice {
public:
    GraphicsDevice() = default;
    virtual ~GraphicsDevice() = default;
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    /// Borrowed resources must stop using this device when its session expires.
    std::weak_ptr<const GraphicsResourceLifetime> resource_lifetime() const noexcept { return m_resource_lifetime; }

    virtual bool initialize(void* native_window_handle) = 0;
    /// Idempotent, non-throwing cleanup, including after a failed initialize or unfinished frame.
    /// Retire submitted GPU work before releasing resources. The native window is still alive.
    virtual void shutdown() = 0;

    /// Backing-store dimensions in pixels (e.g. GLFW framebuffer size). No-op for headless backends.
    virtual void resize(uint32_t width, uint32_t height) { (void)width; (void)height; }

    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    // Resource Creation
    /// Compiles Metal source and builds a render pipeline. Entry names default to textured mesh shader.
    virtual PipelineHandle create_pipeline(const std::string& shader_source,
        const std::string& vertex_entry = "vertexMain",
        const std::string& fragment_entry = "fragmentMain") = 0;
    virtual void bind_pipeline(PipelineHandle handle) { (void)handle; }
    virtual VertexBufferHandle create_vertex_buffer(const void* data, size_t size) = 0;
    virtual IndexBufferHandle create_index_buffer(const void* data, size_t size) = 0;
    
    /// Drop application ownership; encoded/submitted work must retain its own resource reference.
    /// Transitional default for backends with device-wide ownership; Metal overrides both.
    virtual void release_vertex_buffer(VertexBufferHandle) noexcept {}
    virtual void release_index_buffer(IndexBufferHandle) noexcept {}

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
protected:
    void invalidate_resource_lifetime() noexcept { m_resource_lifetime.reset(); }
    void begin_resource_lifetime() { m_resource_lifetime = std::make_shared<const GraphicsResourceLifetime>(); }
private:
    std::shared_ptr<const GraphicsResourceLifetime> m_resource_lifetime = std::make_shared<const GraphicsResourceLifetime>();
};

} // namespace maya
