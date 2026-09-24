#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <memory>
#include <string>

namespace maya {

struct RenderTargetDesc {
    Format color_format = Format::rgba8_unorm;
    bool readback = false; // allow GraphicsDevice::read_texture on the color texture
    std::string label = "view";
};

/// Offscreen color and depth textures for one view. The color texture can be sampled, e.g. to
/// present it in a window or an editor panel. Nothing is allocated until the first resize.
class RenderTarget {
public:
    RenderTarget(GraphicsDevice& device, RenderTargetDesc desc = {});
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    /// Allocates textures of this size. The same size is a no-op; a new size replaces both textures
    /// and retires the old ones after the frames using them complete. On failure the previous
    /// textures stay in use. Zero sizes are rejected; skip the view instead.
    RhiDiagnostic resize(uint32_t width, uint32_t height);
    bool valid() const noexcept;
    TextureHandle color() const noexcept { return m_color; }
    TextureHandle depth() const noexcept { return m_depth; }
    uint32_t width() const noexcept { return m_width; }
    uint32_t height() const noexcept { return m_height; }
    Format color_format() const noexcept { return m_desc.color_format; }
    /// Number of successful allocations, for checking that steady frames do not reallocate.
    uint64_t allocations() const noexcept { return m_allocations; }

private:
    void release() noexcept;

    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    RenderTargetDesc m_desc;
    TextureHandle m_color;
    TextureHandle m_depth;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_allocations = 0;
};

} // namespace maya
