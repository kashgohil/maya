#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <string>

namespace maya {

class Texture {
public:
    Texture(GraphicsDevice& device, const void* data, uint32_t width, uint32_t height)
        : m_device(device) {
        m_handle = m_device.create_texture(data, width, height);
    }

    void bind(uint32_t slot = 0) {
        m_device.bind_texture(m_handle, slot);
    }

private:
    GraphicsDevice& m_device;
    TextureHandle m_handle;
};

} // namespace maya
