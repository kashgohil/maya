#include "maya/rhi/graphics_device.hpp"
#include <map>

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#endif

namespace maya {

class MetalDevice : public GraphicsDevice {
public:
    MetalDevice();
    ~MetalDevice() override;

    bool initialize(void* native_window_handle) override;
    void shutdown() override;

    void begin_frame() override;
    void end_frame() override;

    bool create_pipeline(const std::string& shader_source) override;
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
#ifdef __OBJC__
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_command_queue;
    CAMetalLayer* m_layer;
    id<MTLCommandBuffer> m_current_command_buffer;
    id<MTLRenderPipelineState> m_pipeline_state;
    id<MTLRenderCommandEncoder> m_current_encoder;
    
    std::map<uint32_t, id<MTLBuffer>> m_buffers;
    std::map<uint32_t, id<MTLTexture>> m_textures;
    
    id<MTLSamplerState> m_sampler_state;
    id<MTLTexture> m_depth_texture;
    id<MTLDepthStencilState> m_depth_stencil_state;
    void* m_pool;
    uint32_t m_next_handle = 1;
#else
    void* m_device;
    void* m_command_queue;
    void* m_layer;
    void* m_current_command_buffer;
    void* m_pipeline_state;
    void* m_current_encoder;
    void* m_buffers;
    void* m_textures;
    void* m_sampler_state;
    void* m_depth_texture;
    void* m_depth_stencil_state;
    void* m_pool;
    uint32_t m_next_handle;
#endif
};

} // namespace maya
