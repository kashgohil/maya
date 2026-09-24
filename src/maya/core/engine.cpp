#include "maya/core/engine.hpp"
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace maya {

Engine::~Engine() {
    shutdown();
}

bool Engine::initialize(std::unique_ptr<GraphicsDevice> device, void* native_window,
    std::unique_ptr<Application> application) {
    if (m_device || !device || !application) {
        std::cerr << "[Engine] initialize requires an idle engine, device, and application\n";
        return false;
    }
    m_device = std::move(device);
    m_application = std::move(application);
    try {
        if (m_device->initialize(native_window)) {
            m_application_started = true;
            if (m_application->on_start(*m_device)) {
                m_initialized = true;
                return true;
            }
            std::cerr << "[Engine] application startup failed\n";
        } else {
            std::cerr << "[Engine] graphics initialization failed\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "[Engine] startup: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[Engine] startup: unknown exception\n";
    }
    shutdown();
    return false;
}

bool Engine::tick(float delta_time, bool input_enabled) {
    if (!m_initialized || !std::isfinite(delta_time) || delta_time < 0.0f) return false;
    try {
        m_application->on_update(delta_time, input_enabled);
        if (auto error = m_device->begin_frame()) throw std::runtime_error(error.message);
        m_application->on_render(*m_device);
        if (auto error = m_device->end_frame()) throw std::runtime_error(error.message);
        for (const auto& error : m_device->take_gpu_errors()) std::cerr << "[Engine] GPU: " << error.message << '\n';
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[Engine] frame: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[Engine] frame: unknown exception\n";
    }
    // Device shutdown retires an unfinished frame before releasing resources.
    shutdown();
    return false;
}

bool Engine::resize(uint32_t width, uint32_t height) {
    if (!m_initialized) return false;
    if (width == 0 || height == 0) return true;
    try {
        m_device->resize(width, height);
        m_application->on_resize(width, height);
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[Engine] resize: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[Engine] resize: unknown exception\n";
    }
    shutdown();
    return false;
}

void Engine::shutdown() {
    m_initialized = false;
    if (m_application_started) {
        m_application_started = false;
        m_application->on_stop();
    }
    // Application destructors may still reference the device. Shutdown must not throw.
    m_application.reset();
    if (m_device) {
        m_device->shutdown();
        m_device.reset();
    }
}

} // namespace maya
