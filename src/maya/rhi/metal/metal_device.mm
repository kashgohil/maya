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
    , m_current_command_buffer(nil) {}

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
    }

    return true;
}

void MetalDevice::shutdown() {
    m_index_buffer = nil;
    m_vertex_buffer = nil;
    m_uniform_buffer = nil;
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

    m_pipeline_state = [m_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    
    if (!m_pipeline_state) {
        NSLog(@"Failed to create pipeline state: %@", error);
        return false;
    }

    return true;
}

bool MetalDevice::create_vertex_buffer(const void* data, size_t size) {
    m_vertex_buffer = [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    return m_vertex_buffer != nil;
}

bool MetalDevice::create_index_buffer(const void* data, size_t size) {
    m_index_buffer = [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    return m_index_buffer != nil;
}

bool MetalDevice::create_uniform_buffer(size_t size) {
    // Shared mode allows CPU to write, GPU to read
    m_uniform_buffer = [m_device newBufferWithLength:size options:MTLResourceStorageModeShared];
    return m_uniform_buffer != nil;
}

void MetalDevice::update_uniform_buffer(const void* data, size_t size) {
    if (m_uniform_buffer) {
        memcpy(m_uniform_buffer.contents, data, size);
    }
}

void MetalDevice::begin_frame() {
    m_current_command_buffer = [m_command_queue commandBuffer];
}

void MetalDevice::draw_indexed(uint32_t index_count) {
    if (!m_layer || !m_pipeline_state || !m_vertex_buffer || !m_index_buffer) return;
    id<CAMetalDrawable> drawable = [m_layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    passDescriptor.colorAttachments[0].texture = drawable.texture;
    passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
    passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder = [m_current_command_buffer renderCommandEncoderWithDescriptor:passDescriptor];
    [encoder setRenderPipelineState:m_pipeline_state];
    [encoder setCullMode:MTLCullModeNone]; // Disable culling to see both sides
    
    // Bind vertex buffer to index 0
    [encoder setVertexBuffer:m_vertex_buffer offset:0 atIndex:0];

    // Bind uniform buffer to index 1 (if it exists)
    if (m_uniform_buffer) {
        [encoder setVertexBuffer:m_uniform_buffer offset:0 atIndex:1];
    }
    
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle 
                       indexCount:index_count 
                        indexType:MTLIndexTypeUInt32 
                      indexBuffer:m_index_buffer 
                indexBufferOffset:0];
    
    [encoder endEncoding];
    [m_current_command_buffer presentDrawable:drawable];
}

void MetalDevice::end_frame() {
    if (m_current_command_buffer) {
        [m_current_command_buffer commit];
        m_current_command_buffer = nil;
    }
}

} // namespace maya
