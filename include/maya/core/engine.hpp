#pragma once

#include "maya/core/application.hpp"
#include "maya/rhi/graphics_device.hpp"
#include <memory>

namespace maya {

/// Owns an application session and its device; all calls run on the host thread.
/// The host keeps its native window alive until shutdown completes.
class Engine {
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Consumes ownership. Failure rolls back; shut down before starting another session.
    bool initialize(std::unique_ptr<GraphicsDevice> device, void* native_window,
        std::unique_ptr<Application> application, const DeviceOptions& options = {});
    bool tick(float delta_time, bool input_enabled = true);
    bool resize(uint32_t width, uint32_t height);
    void shutdown();
    bool is_initialized() const { return m_initialized; }

private:
    std::unique_ptr<GraphicsDevice> m_device;
    std::unique_ptr<Application> m_application;
    bool m_application_started = false;
    bool m_initialized = false;
};

} // namespace maya
