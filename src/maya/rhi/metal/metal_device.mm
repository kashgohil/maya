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

void MetalDevice::begin_frame() {
    m_current_command_buffer = [m_command_queue commandBuffer];
}

void MetalDevice::draw_triangle() {
    if (!m_layer || !m_pipeline_state) return;
    id<CAMetalDrawable> drawable = [m_layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    passDescriptor.colorAttachments[0].texture = drawable.texture;
    passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
    passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder = [m_current_command_buffer renderCommandEncoderWithDescriptor:passDescriptor];
    [encoder setRenderPipelineState:m_pipeline_state];
    
    // We'll draw 3 vertices. Since we don't have buffers yet, 
    // we'll generate coordinates inside the shader.
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    
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
