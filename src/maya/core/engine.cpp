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

    // Temporary: Hardcoded triangle shader
    std::string shader_source = R"(
        #include <metal_stdlib>
        using namespace metal;

        struct VertexOut {
            float4 position [[position]];
            float4 color;
        };

        vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
            float2 positions[3] = {
                float2(0.0, 0.5),
                float2(-0.5, -0.5),
                float2(0.5, -0.5)
            };
            float4 colors[3] = {
                float4(1.0, 0.0, 0.0, 1.0),
                float4(0.0, 1.0, 0.0, 1.0),
                float4(0.0, 0.0, 1.0, 1.0)
            };

            VertexOut out;
            out.position = float4(positions[vertexID], 0.0, 1.0);
            out.color = colors[vertexID];
            return out;
        }

        fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
            return in.color;
        }
    )";

    if (!m_graphics_device->create_pipeline(shader_source)) {
        return false;
    }

    m_is_running = true;
    return true;
}

void Engine::run() {
    while (m_is_running && !m_window->should_close()) {
        m_window->poll_events();

        m_graphics_device->begin_frame();
        m_graphics_device->draw_triangle();
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
