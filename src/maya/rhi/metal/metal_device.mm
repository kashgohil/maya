#include "maya/rhi/metal/metal_device.hpp"
#include <string>
#import <Cocoa/Cocoa.h>

namespace maya {

std::unique_ptr<GraphicsDevice> GraphicsDevice::create_default() {
    return std::make_unique<MetalDevice>();
}

MetalDevice::MetalDevice() 
    : m_device(nil)
    , m_command_queue(nil)
    , m_layer(nil)
    , m_current_command_buffer(nil)
    , m_current_encoder(nil)
    , m_depth_texture(nil)
    , m_depth_stencil_state(nil) 
    , m_pool(nullptr) {}

MetalDevice::~MetalDevice() {
    shutdown();
}

bool MetalDevice::initialize(void* native_window_handle) {
    m_device = MTLCreateSystemDefaultDevice();
    if (!m_device) return false;

    m_command_queue = [m_device newCommandQueue];
    
    if (native_window_handle) {
        NSWindow* window = (NSWindow*)native_window_handle;
        m_layer = [CAMetalLayer layer];
        m_layer.device = m_device;
        m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        
        window.contentView.layer = m_layer;
        window.contentView.wantsLayer = YES;

        // Set drawable size explicitly
        NSRect frame = window.contentView.bounds;
        CGFloat scale = window.backingScaleFactor;
        m_layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);

        // Create Depth Texture
        MTLTextureDescriptor* depthDescriptor = [MTLTextureDescriptor 
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float 
                                         width:m_layer.drawableSize.width 
                                        height:m_layer.drawableSize.height 
                                     mipmapped:NO];
        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        depthDescriptor.storageMode = MTLStorageModePrivate;
        m_depth_texture = [m_device newTextureWithDescriptor:depthDescriptor];

        // Create Depth Stencil State
        MTLDepthStencilDescriptor* depthStencilDescriptor = [[MTLDepthStencilDescriptor alloc] init];
        depthStencilDescriptor.depthCompareFunction = MTLCompareFunctionLess;
        depthStencilDescriptor.depthWriteEnabled = YES;
        m_depth_stencil_state = [m_device newDepthStencilStateWithDescriptor:depthStencilDescriptor];
    }

    return true;
}

void MetalDevice::shutdown() {
    m_buffers.clear();
    m_textures.clear();
    m_depth_texture = nil;
    m_depth_stencil_state = nil;
    m_pipeline_state = nil;
    m_current_command_buffer = nil;
    m_layer = nil;
    m_command_queue = nil;
    m_device = nil;
}

bool MetalDevice::create_pipeline(const std::string& shader_source) {
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:shader_source.c_str()];
    id<MTLLibrary> library = [m_device newLibraryWithSource:source options:nil error:&error];
    
    if (!library) {
        NSLog(@"Failed to create library: %@", error);
        return false;
    }

    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertexMain"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragmentMain"];

    MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.colorAttachments[0].pixelFormat = m_layer.pixelFormat;
    pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    m_pipeline_state = [m_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    
    if (!m_pipeline_state) {
        NSLog(@"Failed to create pipeline state: %@", error);
        return false;
    }

    return true;
}

VertexBufferHandle MetalDevice::create_vertex_buffer(const void* data, size_t size) {
    id<MTLBuffer> buffer = [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    if (buffer) {
        uint32_t handle = m_next_handle++;
        m_buffers[handle] = buffer;
        return {handle};
    }
    return {INVALID_HANDLE};
}

IndexBufferHandle MetalDevice::create_index_buffer(const void* data, size_t size) {
    id<MTLBuffer> buffer = [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    if (buffer) {
        uint32_t handle = m_next_handle++;
        m_buffers[handle] = buffer;
        return {handle};
    }
    return {INVALID_HANDLE};
}

UniformBufferHandle MetalDevice::create_uniform_buffer(size_t size) {
    id<MTLBuffer> buffer = [m_device newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (buffer) {
        uint32_t handle = m_next_handle++;
        m_buffers[handle] = buffer;
        return {handle};
    }
    return {INVALID_HANDLE};
}

void MetalDevice::update_uniform_buffer(UniformBufferHandle handle, const void* data, size_t size) {
    auto it = m_buffers.find(handle.handle);
    if (it != m_buffers.end()) {
        memcpy(it->second.contents, data, size);
    }
}

TextureHandle MetalDevice::create_texture(const void* data, uint32_t width, uint32_t height) {
    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor 
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm 
                                     width:width 
                                    height:height 
                                 mipmapped:NO];
    
    id<MTLTexture> texture = [m_device newTextureWithDescriptor:textureDescriptor];
    if (!texture) return {INVALID_HANDLE};

    MTLRegion region = {{0, 0, 0}, {width, height, 1}};
    [texture replaceRegion:region mipmapLevel:0 withBytes:data bytesPerRow:4 * width];

    if (!m_sampler_state) {
        MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
        m_sampler_state = [m_device newSamplerStateWithDescriptor:samplerDescriptor];
    }

    uint32_t handle = m_next_handle++;
    m_textures[handle] = texture;
    return {handle};
}

void MetalDevice::begin_frame() {
    m_current_command_buffer = [m_command_queue commandBuffer];
    
    id<CAMetalDrawable> drawable = [m_layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    passDescriptor.colorAttachments[0].texture = drawable.texture;
    passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
    passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    passDescriptor.depthAttachment.texture = m_depth_texture;
    passDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
    passDescriptor.depthAttachment.clearDepth = 1.0;
    passDescriptor.depthAttachment.storeAction = MTLStoreActionDontCare;

    m_current_encoder = [m_current_command_buffer renderCommandEncoderWithDescriptor:passDescriptor];
    [m_current_encoder setRenderPipelineState:m_pipeline_state];
    [m_current_encoder setDepthStencilState:m_depth_stencil_state];
    [m_current_encoder setCullMode:MTLCullModeBack]; 
    [m_current_encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    
    if (m_sampler_state) {
        [m_current_encoder setFragmentSamplerState:m_sampler_state atIndex:0];
    }

    [m_current_command_buffer presentDrawable:drawable];
}

void MetalDevice::bind_vertex_buffer(VertexBufferHandle handle, uint32_t slot) {
    auto it = m_buffers.find(handle.handle);
    if (it != m_buffers.end()) {
        [m_current_encoder setVertexBuffer:it->second offset:0 atIndex:slot];
    }
}

void MetalDevice::bind_uniform_buffer(UniformBufferHandle handle, uint32_t slot) {
    auto it = m_buffers.find(handle.handle);
    if (it != m_buffers.end()) {
        [m_current_encoder setVertexBuffer:it->second offset:0 atIndex:slot];
    }
}

void MetalDevice::bind_texture(TextureHandle handle, uint32_t slot) {
    auto it = m_textures.find(handle.handle);
    if (it != m_textures.end()) {
        [m_current_encoder setFragmentTexture:it->second atIndex:slot];
    }
}

void MetalDevice::draw_indexed(IndexBufferHandle handle, uint32_t index_count) {
    auto it = m_buffers.find(handle.handle);
    if (it != m_buffers.end()) {
        [m_current_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle 
                           indexCount:index_count 
                            indexType:MTLIndexTypeUInt32 
                          indexBuffer:it->second 
                    indexBufferOffset:0];
    }
}

void MetalDevice::end_frame() {
    if (m_current_encoder) {
        [m_current_encoder endEncoding];
        m_current_encoder = nil;
    }
    if (m_current_command_buffer) {
        [m_current_command_buffer commit];
        m_current_command_buffer = nil;
    }
}

} // namespace maya