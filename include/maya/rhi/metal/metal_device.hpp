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

private:
#ifdef __OBJC__
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_command_queue;
    CAMetalLayer* m_layer;
    id<MTLCommandBuffer> m_current_command_buffer;
#else
    void* m_device;
    void* m_command_queue;
    void* m_layer;
    void* m_current_command_buffer;
#endif
};

} // namespace maya
