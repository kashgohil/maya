#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <string>

namespace maya {

/// Owns a sampled RGBA8 texture. The texture is released when this object is destroyed.
class Texture {
public:
    Texture(GraphicsDevice& device, const void* rgba8, uint32_t width, uint32_t height, std::string label = {})
        : m_device(device), m_lifetime(device.resource_lifetime()) {
        auto created = m_device.create_texture({width, height, Format::rgba8_unorm, TextureUsage::sampled,
                                                std::move(label)}, rgba8);
        m_handle = created.handle;
        m_error = std::move(created.diagnostic);
    }
    ~Texture() {
        if (!m_lifetime.expired()) m_device.destroy(m_handle);
    }
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    bool valid() const noexcept { return !m_lifetime.expired() && m_handle.valid(); }
    TextureHandle handle() const noexcept { return m_handle; }
    /// Why creation failed, if it did.
    const RhiDiagnostic& error() const noexcept { return m_error; }
    RhiDiagnostic bind(uint32_t slot = 0) const { return m_device.set_texture(slot, m_handle); }

private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    TextureHandle m_handle;
    RhiDiagnostic m_error;
};

} // namespace maya
