#pragma once

#include <memory>

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
    virtual void draw_triangle() = 0;

    static std::unique_ptr<GraphicsDevice> create_default();
};

} // namespace maya
