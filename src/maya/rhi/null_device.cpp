#include "maya/rhi/null_device.hpp"

namespace maya {

bool NullGraphicsDevice::backend_initialize(void* window, RhiLimits&, Format& surface_format) {
    m_has_surface = m_options.surface && window;
    m_native = 0;
    m_last_submitted = 0;
    if (m_has_surface) surface_format = Format::bgra8_unorm;
    return true;
}

void NullGraphicsDevice::backend_resize(uint32_t width, uint32_t height) {
    m_options.surface_width = width;
    m_options.surface_height = height;
}

RhiDiagnostic NullGraphicsDevice::backend_read_texture(uint32_t, const TextureDesc& desc,
                                                       std::vector<std::byte>& pixels) {
    pixels.assign(size_t{desc.width} * desc.height * bytes_per_pixel(desc.format), std::byte{0});
    return {};
}

GraphicsDevice::BackendSurface NullGraphicsDevice::backend_acquire_surface(uint32_t) {
    if (m_options.surface_width == 0 || m_options.surface_height == 0)
        return {0, 0, {RhiError::surface_unavailable, "Surface has zero size"}};
    if (!m_drawable_available)
        return {0, 0, {RhiError::surface_unavailable, "No drawable is available this frame"}};
    return {m_options.surface_width, m_options.surface_height, {}};
}

void NullGraphicsDevice::backend_submit(uint64_t serial, bool present) {
    m_last_submitted = serial;
    if (present) ++m_presented;
    if (!m_options.manual_completion) completion()->complete(serial);
}

} // namespace maya
