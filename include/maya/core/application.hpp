#pragma once

#include <cstdint>

namespace maya {

class GraphicsDevice;

/// Game/editor content boundary. Callbacks must not re-enter Engine lifecycle methods.
class Application {
public:
    virtual ~Application() = default;
    virtual bool on_start(GraphicsDevice&) { return true; }
    virtual void on_update(float /*delta_time*/, bool /*input_enabled*/) {}
    virtual void on_render(GraphicsDevice&) {}
    virtual void on_resize(uint32_t /*width*/, uint32_t /*height*/) {}
    /// Called once whenever on_start was entered, including a failed/throwing start.
    /// Release application state here; the device remains alive through destruction.
    virtual void on_stop() noexcept {}
};

} // namespace maya
