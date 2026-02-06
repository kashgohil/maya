#include "maya/core/engine.hpp"
#include <iostream>

namespace maya {

Engine::Engine() = default;
Engine::~Engine() {
    shutdown();
}

bool Engine::initialize() {
    m_window = std::make_unique<Window>(1280, 720, "Maya Engine - RHI Metal Backend");
    if (!m_window->get_native_handle()) {
        return false;
    }

    m_graphics_device = GraphicsDevice::create_default();
    if (!m_graphics_device->initialize(m_window->get_native_handle())) {
        return false;
    }

    m_is_running = true;
    return true;
}

void Engine::run() {
    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();

        m_graphics_device->begin_frame();
        // Render logic here
        m_graphics_device->end_frame();
    }
}

void Engine::shutdown() {
    if (m_graphics_device) {
        m_graphics_device->shutdown();
    }
    m_is_running = false;
}

} // namespace maya
