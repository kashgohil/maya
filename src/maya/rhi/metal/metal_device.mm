#include "maya/rhi/metal/metal_device.hpp"
#include <string>
#include <map>
#include <cstring>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSView.h>

namespace maya {

std::unique_ptr<GraphicsDevice> GraphicsDevice::create_default() {
    return std::make_unique<MetalDevice>();
}

struct MetalDevice::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    CAMetalLayer* layer = nil;
    NSView* view = nil;
    id<MTLCommandBuffer> current_command_buffer = nil;
    id<MTLCommandBuffer> last_submission = nil;
    id<MTLRenderCommandEncoder> current_encoder = nil;
    id<CAMetalDrawable> current_drawable = nil;
    std::map<uint32_t, id<MTLRenderPipelineState>> pipeline_states;
    std::map<uint32_t, id<MTLBuffer>> buffers;
    std::map<uint32_t, id<MTLTexture>> textures;
    id<MTLSamplerState> sampler_state = nil;
    id<MTLTexture> depth_texture = nil;
    id<MTLDepthStencilState> depth_stencil_state = nil;
    uint32_t next_pipeline_handle = 1;
    uint32_t next_handle = 1;
};

MetalDevice::MetalDevice() : m_impl(std::make_unique<Impl>()) {}

MetalDevice::~MetalDevice() {
    shutdown();
}

bool MetalDevice::initialize(void* native_window_handle) {
    shutdown();
    @autoreleasepool {
        m_impl->device = MTLCreateSystemDefaultDevice();
        if (!m_impl->device) return false;

        m_impl->command_queue = [m_impl->device newCommandQueue];
        if (!m_impl->command_queue) { shutdown(); return false; }

        if (native_window_handle) {
            NSWindow* window = (__bridge NSWindow*)native_window_handle;
            m_impl->view = window.contentView;
            m_impl->layer = [CAMetalLayer layer];
            m_impl->layer.device = m_impl->device;
            m_impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

            window.contentView.layer = m_impl->layer;
            window.contentView.wantsLayer = YES;

            // Set drawable size explicitly
            NSRect frame = window.contentView.bounds;
            CGFloat scale = window.backingScaleFactor;
            m_impl->layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);

            // Create Depth Texture
            MTLTextureDescriptor* depthDescriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                             width:m_impl->layer.drawableSize.width
                                            height:m_impl->layer.drawableSize.height
                                         mipmapped:NO];
            depthDescriptor.usage = MTLTextureUsageRenderTarget;
            depthDescriptor.storageMode = MTLStorageModePrivate;
            m_impl->depth_texture = [m_impl->device newTextureWithDescriptor:depthDescriptor];

            // Create Depth Stencil State
            MTLDepthStencilDescriptor* depthStencilDescriptor = [[MTLDepthStencilDescriptor alloc] init];
            depthStencilDescriptor.depthCompareFunction = MTLCompareFunctionLess;
            depthStencilDescriptor.depthWriteEnabled = YES;
            m_impl->depth_stencil_state = [m_impl->device newDepthStencilStateWithDescriptor:depthStencilDescriptor];
            if (!m_impl->depth_texture || !m_impl->depth_stencil_state) { shutdown(); return false; }
        }

        return true;
    }
}

void MetalDevice::shutdown() {
    @autoreleasepool {
        // Finish a partially encoded frame, then drain this queue before releasing resources.
        end_frame();
        [m_impl->last_submission waitUntilCompleted];
        m_impl->last_submission = nil;
        m_impl->buffers.clear();
        m_impl->textures.clear();
        m_impl->pipeline_states.clear();
        m_impl->depth_texture = nil;
        m_impl->depth_stencil_state = nil;
        m_impl->sampler_state = nil;
        if (m_impl->layer && m_impl->view.layer == m_impl->layer)
            m_impl->view.layer = nil;
        m_impl->view = nil;
        m_impl->layer = nil;
        m_impl->command_queue = nil;
        m_impl->device = nil;
    }
}

void MetalDevice::resize(uint32_t width, uint32_t height) {
    @autoreleasepool {
        if (!m_impl->layer || width == 0 || height == 0) {
            return;
        }

        m_impl->layer.drawableSize = CGSizeMake(width, height);

        MTLTextureDescriptor* depthDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:width
                                        height:height
                                     mipmapped:NO];
        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        depthDescriptor.storageMode = MTLStorageModePrivate;
        m_impl->depth_texture = [m_impl->device newTextureWithDescriptor:depthDescriptor];
    }
}

PipelineHandle MetalDevice::create_pipeline(const std::string& shader_source,
    const std::string& vertex_entry,
    const std::string& fragment_entry) {
    @autoreleasepool {
        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:shader_source.c_str()];
        id<MTLLibrary> library = [m_impl->device newLibraryWithSource:source options:nil error:&error];

        if (!library) {
            NSLog(@"Failed to create library: %@", error);
            return {INVALID_HANDLE};
        }

        NSString* vertexName = [NSString stringWithUTF8String:vertex_entry.c_str()];
        NSString* fragmentName = [NSString stringWithUTF8String:fragment_entry.c_str()];
        id<MTLFunction> vertexFunction = [library newFunctionWithName:vertexName];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:fragmentName];

        if (!vertexFunction || !fragmentFunction) {
            NSLog(@"Failed to create library: missing vertex or fragment function");
            return {INVALID_HANDLE};
        }

        MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;
        MTLPixelFormat color_format = m_impl->layer ? m_impl->layer.pixelFormat : MTLPixelFormatBGRA8Unorm;
        pipelineDescriptor.colorAttachments[0].pixelFormat = color_format;
        pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        id<MTLRenderPipelineState> m_pipeline_state = [m_impl->device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];

        if (!m_pipeline_state) {
            NSLog(@"Failed to create pipeline state: %@", error);
            return {INVALID_HANDLE};
        }

        uint32_t handle = m_impl->next_pipeline_handle++;
        m_impl->pipeline_states[handle] = m_pipeline_state;
        return {handle};
    }
}

void MetalDevice::bind_pipeline(PipelineHandle handle) {
    if (!m_impl->current_encoder) {
        return;
    }
    auto it = m_impl->pipeline_states.find(handle.handle);
    if (it != m_impl->pipeline_states.end()) {
        [m_impl->current_encoder setRenderPipelineState:it->second];
    }
}

namespace {
    template<typename HandleType>
    HandleType create_buffer_helper(id<MTLDevice> device, std::map<uint32_t, id<MTLBuffer>>& buffers,
                                    uint32_t& next_handle, const void* data, size_t size) {
        id<MTLBuffer> buffer = [device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
        if (buffer) {
            uint32_t handle = next_handle++;
            buffers[handle] = buffer;
            return {handle};
        }
        return {INVALID_HANDLE};
    }
}

VertexBufferHandle MetalDevice::create_vertex_buffer(const void* data, size_t size) {
    return create_buffer_helper<VertexBufferHandle>(m_impl->device, m_impl->buffers, m_impl->next_handle, data, size);
}

IndexBufferHandle MetalDevice::create_index_buffer(const void* data, size_t size) {
    return create_buffer_helper<IndexBufferHandle>(m_impl->device, m_impl->buffers, m_impl->next_handle, data, size);
}

UniformBufferHandle MetalDevice::create_uniform_buffer(size_t size) {
    id<MTLBuffer> buffer = [m_impl->device newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (buffer) {
        uint32_t handle = m_impl->next_handle++;
        m_impl->buffers[handle] = buffer;
        return {handle};
    }
    return {INVALID_HANDLE};
}

void MetalDevice::update_uniform_buffer(UniformBufferHandle handle, const void* data, size_t size) {
    auto it = m_impl->buffers.find(handle.handle);
    if (it != m_impl->buffers.end() && data && size <= it->second.length) {
        memcpy(it->second.contents, data, size);
    }
}

TextureHandle MetalDevice::create_texture(const void* data, uint32_t width, uint32_t height) {
    @autoreleasepool {
        MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:width
                                        height:height
                                     mipmapped:NO];

        id<MTLTexture> texture = [m_impl->device newTextureWithDescriptor:textureDescriptor];
        if (!texture) return {INVALID_HANDLE};

        MTLRegion region = {{0, 0, 0}, {width, height, 1}};
        [texture replaceRegion:region mipmapLevel:0 withBytes:data bytesPerRow:4 * width];

        if (!m_impl->sampler_state) {
            MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
            samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
            samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
            samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
            samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
            m_impl->sampler_state = [m_impl->device newSamplerStateWithDescriptor:samplerDescriptor];
        }

        uint32_t handle = m_impl->next_handle++;
        m_impl->textures[handle] = texture;
        return {handle};
    }
}

void MetalDevice::begin_frame() {
    @autoreleasepool {
        m_impl->current_command_buffer = [m_impl->command_queue commandBuffer];

        id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
        if (!drawable) return;

        MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        passDescriptor.colorAttachments[0].texture = drawable.texture;
        passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
        passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

        passDescriptor.depthAttachment.texture = m_impl->depth_texture;
        passDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
        passDescriptor.depthAttachment.clearDepth = 1.0;
        passDescriptor.depthAttachment.storeAction = MTLStoreActionDontCare;

        m_impl->current_encoder = [m_impl->current_command_buffer renderCommandEncoderWithDescriptor:passDescriptor];
        [m_impl->current_encoder setDepthStencilState:m_impl->depth_stencil_state];
        [m_impl->current_encoder setCullMode:MTLCullModeBack];
        [m_impl->current_encoder setFrontFacingWinding:MTLWindingCounterClockwise];

        // Store drawable to present it later
        m_impl->current_drawable = drawable;

        if (m_impl->sampler_state) {
            [m_impl->current_encoder setFragmentSamplerState:m_impl->sampler_state atIndex:0];
        }
    }
}

void MetalDevice::bind_vertex_buffer(VertexBufferHandle handle, uint32_t slot) {
    auto it = m_impl->buffers.find(handle.handle);
    if (it != m_impl->buffers.end()) {
        [m_impl->current_encoder setVertexBuffer:it->second offset:0 atIndex:slot];
    }
}

void MetalDevice::bind_uniform_buffer(UniformBufferHandle handle, uint32_t slot) {
    auto it = m_impl->buffers.find(handle.handle);
    if (it == m_impl->buffers.end() || !m_impl->current_encoder) {
        return;
    }
    id<MTLBuffer> buf = it->second;
    [m_impl->current_encoder setVertexBuffer:buf offset:0 atIndex:slot];
    [m_impl->current_encoder setFragmentBuffer:buf offset:0 atIndex:slot];
}

void MetalDevice::bind_texture(TextureHandle handle, uint32_t slot) {
    auto it = m_impl->textures.find(handle.handle);
    if (it != m_impl->textures.end()) {
        [m_impl->current_encoder setFragmentTexture:it->second atIndex:slot];
    }
}

void MetalDevice::draw_indexed(IndexBufferHandle handle, uint32_t index_count) {
    auto it = m_impl->buffers.find(handle.handle);
    if (it != m_impl->buffers.end()) {
        [m_impl->current_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                           indexCount:index_count
                            indexType:MTLIndexTypeUInt32
                          indexBuffer:it->second
                    indexBufferOffset:0];
    }
}

void MetalDevice::end_frame() {
    @autoreleasepool {
        if (m_impl->current_encoder) {
            [m_impl->current_encoder endEncoding];
            m_impl->current_encoder = nil;
        }
        if (m_impl->current_command_buffer) {
            if (m_impl->current_drawable) {
                [m_impl->current_command_buffer presentDrawable:m_impl->current_drawable];
                m_impl->current_drawable = nil;
            }
            m_impl->last_submission = m_impl->current_command_buffer;
            [m_impl->current_command_buffer commit];
            m_impl->current_command_buffer = nil;
        }
    }
}

} // namespace maya
