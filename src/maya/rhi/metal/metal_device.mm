#include "maya/rhi/metal/metal_device.hpp"
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
    NSWindow* window = (NSWindow*)native_window_handle;
    m_device = MTLCreateSystemDefaultDevice();
    if (!m_device) return false;

    m_command_queue = [m_device newCommandQueue];
    
    m_layer = [CAMetalLayer layer];
    m_layer.device = m_device;
    m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    
    window.contentView.layer = m_layer;
    window.contentView.wantsLayer = YES;

    return true;
}

void MetalDevice::shutdown() {
    m_current_command_buffer = nil;
    m_layer = nil;
    m_command_queue = nil;
    m_device = nil;
}

void MetalDevice::begin_frame() {
    m_current_command_buffer = [m_command_queue commandBuffer];
    
    id<CAMetalDrawable> drawable = [m_layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    passDescriptor.colorAttachments[0].texture = drawable.texture;
    passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
    passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder = [m_current_command_buffer renderCommandEncoderWithDescriptor:passDescriptor];
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
