#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <span>
#include <string>

namespace maya {

/// Owns a sampled texture. The texture is released when this object is destroyed.
class Texture {
public:
    /// A single-level RGBA8 (unorm) texture.
    Texture(GraphicsDevice& device, const void* rgba8, uint32_t width, uint32_t height, std::string label = {})
        : Texture(device, {width, height, Format::rgba8_unorm, TextureUsage::sampled, std::move(label)}, rgba8) {}
    /// Any sampled texture; `data` holds every level as GraphicsDevice::create_texture describes, and
    /// a size other than texture_bytes(desc) fails creation.
    Texture(GraphicsDevice& device, TextureDesc desc, std::span<const std::byte> data)
        : m_device(device), m_lifetime(device.resource_lifetime()), m_desc(std::move(desc)) {
        auto created = m_device.create_texture(m_desc, data);
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
    const TextureDesc& desc() const noexcept { return m_desc; }
    /// Tracked size of every level (texture_bytes), as RhiStats counts it.
    size_t gpu_bytes() const noexcept { return m_handle.valid() ? texture_bytes(m_desc) : 0; }
    /// Why creation failed, if it did.
    const RhiDiagnostic& error() const noexcept { return m_error; }
    RhiDiagnostic bind(uint32_t slot = 0) const { return m_device.set_texture(slot, m_handle); }

private:
    Texture(GraphicsDevice& device, TextureDesc desc, const void* data)
        : m_device(device), m_lifetime(device.resource_lifetime()), m_desc(std::move(desc)) {
        auto created = m_device.create_texture(m_desc, data);
        m_handle = created.handle;
        m_error = std::move(created.diagnostic);
    }

    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    TextureDesc m_desc;
    TextureHandle m_handle;
    RhiDiagnostic m_error;
};

/// Owns a sampler state; released when this object is destroyed.
class Sampler {
public:
    Sampler(GraphicsDevice& device, SamplerDesc desc)
        : m_device(device), m_lifetime(device.resource_lifetime()), m_desc(std::move(desc)) {
        auto created = m_device.create_sampler(m_desc);
        m_handle = created.handle;
        m_error = std::move(created.diagnostic);
    }
    ~Sampler() {
        if (!m_lifetime.expired()) m_device.destroy(m_handle);
    }
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    bool valid() const noexcept { return !m_lifetime.expired() && m_handle.valid(); }
    SamplerHandle handle() const noexcept { return m_handle; }
    const SamplerDesc& desc() const noexcept { return m_desc; }
    const RhiDiagnostic& error() const noexcept { return m_error; }
    RhiDiagnostic bind(uint32_t slot = 0) const { return m_device.set_sampler(slot, m_handle); }

private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    SamplerDesc m_desc;
    SamplerHandle m_handle;
    RhiDiagnostic m_error;
};

} // namespace maya
