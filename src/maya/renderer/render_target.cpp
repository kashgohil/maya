#include "maya/renderer/render_target.hpp"

namespace maya {

RenderTarget::RenderTarget(GraphicsDevice& device, RenderTargetDesc desc)
    : m_device(device), m_desc(std::move(desc)) {}

RenderTarget::~RenderTarget() { release(); }

bool RenderTarget::valid() const noexcept {
    return !m_lifetime.expired() && m_device.describe(m_color) && m_device.describe(m_scene) && m_device.describe(m_depth);
}

void RenderTarget::release() noexcept {
    if (!m_lifetime.expired()) {
        m_device.destroy(m_color);
        m_device.destroy(m_scene);
        m_device.destroy(m_depth);
    }
    m_color = {};
    m_scene = {};
    m_depth = {};
    m_width = m_height = 0;
}

RhiDiagnostic RenderTarget::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return {RhiError::invalid_descriptor, "Render target '" + m_desc.label + "' needs a nonzero size"};
    if (valid() && width == m_width && height == m_height) return {};
    if (!is_color_format(m_desc.color_format) || !is_color_format(m_desc.scene_format))
        return {RhiError::invalid_descriptor, "Render target '" + m_desc.label + "' needs color formats"};
    auto usage = TextureUsage::render_target | TextureUsage::sampled;
    if (m_desc.readback) usage = usage | TextureUsage::readback;
    auto color = m_device.create_texture({width, height, m_desc.color_format, usage, m_desc.label + " color"});
    if (!color) return color.diagnostic;
    auto scene = m_device.create_texture({width, height, m_desc.scene_format, TextureUsage::render_target | TextureUsage::sampled,
                                          m_desc.label + " scene"});
    if (!scene) {
        m_device.destroy(color.handle);
        return scene.diagnostic;
    }
    auto depth = m_device.create_texture({width, height, Format::depth32_float, TextureUsage::render_target,
                                          m_desc.label + " depth"});
    if (!depth) {
        m_device.destroy(color.handle);
        m_device.destroy(scene.handle);
        return depth.diagnostic;
    }
    // A new device session invalidates the old handles without retiring anything here.
    release();
    m_lifetime = m_device.resource_lifetime();
    m_color = color.handle;
    m_scene = scene.handle;
    m_depth = depth.handle;
    m_width = width;
    m_height = height;
    ++m_allocations;
    return {};
}

} // namespace maya
