#pragma once

#include "maya/rhi/graphics_device.hpp"

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
    bool create_vertex_buffer(const void* data, size_t size) override;
    bool create_index_buffer(const void* data, size_t size) override;
    
    bool create_uniform_buffer(size_t size) override;
    void update_uniform_buffer(const void* data, size_t size) override;

    bool create_texture(const void* data, uint32_t width, uint32_t height) override;
    void bind_texture(uint32_t slot) override;

    void draw_indexed(uint32_t index_count) override;

private:
#ifdef __OBJC__
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_command_queue;
    CAMetalLayer* m_layer;
    id<MTLCommandBuffer> m_current_command_buffer;
    id<MTLRenderPipelineState> m_pipeline_state;
    id<MTLBuffer> m_vertex_buffer;
    id<MTLBuffer> m_index_buffer;
    id<MTLBuffer> m_uniform_buffer;
    id<MTLTexture> m_texture;
    id<MTLSamplerState> m_sampler_state;
    id<MTLTexture> m_depth_texture;
    id<MTLDepthStencilState> m_depth_stencil_state;
    void* m_pool;
#else
    void* m_device;
    void* m_command_queue;
    void* m_layer;
    void* m_current_command_buffer;
    void* m_pipeline_state;
    void* m_vertex_buffer;
    void* m_index_buffer;
    void* m_uniform_buffer;
    void* m_depth_texture;
    void* m_depth_stencil_state;
    void* m_pool;
#endif
};

} // namespace maya
