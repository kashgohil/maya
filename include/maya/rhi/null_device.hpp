#pragma once

#include "maya/rhi/graphics_device.hpp"

namespace maya {

struct NullDeviceOptions {
    bool surface = false; // emulate a presentable surface when initialized with a window handle
    uint32_t surface_width = 0; // zero until resize()
    uint32_t surface_height = 0;
    bool manual_completion = false; // frames complete only through complete_through()
};

/// CPU-only backend: validates and tracks resources through GraphicsDevice but executes nothing.
/// It is for testing API rules and lifetimes, not evidence of GPU behavior.
class NullGraphicsDevice : public GraphicsDevice {
public:
    explicit NullGraphicsDevice(NullDeviceOptions options = {}) : m_options(options) {}
    ~NullGraphicsDevice() override { shutdown(); }

    /// Simulates GPU completion of every frame up to `serial` when manual completion is enabled.
    void complete_through(uint64_t serial) noexcept { completion()->complete_through(serial); }
    void set_drawable_available(bool available) noexcept { m_drawable_available = available; }
    size_t native_resources() const noexcept { return m_native; }
    size_t presented_frames() const noexcept { return m_presented; }

protected:
    bool backend_initialize(void* window, RhiLimits&, Format& surface_format) override;
    void backend_shutdown() noexcept override { m_native = 0; }
    void backend_resize(uint32_t width, uint32_t height) override;
    RhiDiagnostic backend_create_buffer(uint32_t, const BufferDesc&, const void*) override { ++m_native; return {}; }
    RhiDiagnostic backend_create_texture(uint32_t, const TextureDesc&, const void*) override { ++m_native; return {}; }
    RhiDiagnostic backend_create_sampler(uint32_t, const SamplerDesc&) override { ++m_native; return {}; }
    RhiDiagnostic backend_create_pipeline(uint32_t, const PipelineDesc&) override { ++m_native; return {}; }
    void backend_release(ResourceKind, uint32_t) noexcept override { --m_native; }
    void backend_write_buffer(uint32_t, size_t, const void*, size_t) noexcept override {}
    RhiDiagnostic backend_read_texture(uint32_t, const TextureDesc& desc, std::vector<std::byte>& pixels) override;
    RhiDiagnostic backend_begin_frame() override { return {}; }
    BackendSurface backend_acquire_surface(uint32_t slot) override;
    RhiDiagnostic backend_begin_pass(const RenderPassDesc&) override { return {}; }
    void backend_set_pipeline(uint32_t) override {}
    void backend_set_vertex_buffer(uint32_t, uint32_t, size_t) override {}
    void backend_set_uniform_buffer(uint32_t, uint32_t, size_t) override {}
    void backend_set_texture(uint32_t, uint32_t) override {}
    void backend_set_sampler(uint32_t, uint32_t) override {}
    void backend_set_scissor(const ScissorRect&) override {}
    void backend_draw(uint32_t, uint32_t, uint32_t) override {}
    void backend_draw_indexed(uint32_t, IndexType, uint32_t, size_t, uint32_t) override {}
    void backend_end_pass() override {}
    void backend_submit(uint64_t serial, bool present) override;
    void backend_abandon_frame() noexcept override {}
    void backend_wait_idle() noexcept override { completion()->complete_through(m_last_submitted); }
    /// Automatic completion never blocks; manual completion cannot wait and reports a timeout.
    bool backend_wait_frame(uint64_t serial) noexcept override {
        return !m_options.manual_completion || completion()->completed.load() >= serial;
    }
    void backend_release_surface(uint32_t) noexcept override {}

private:
    NullDeviceOptions m_options;
    bool m_has_surface = false;
    bool m_drawable_available = true;
    size_t m_native = 0;
    size_t m_presented = 0;
    uint64_t m_last_submitted = 0;
};

} // namespace maya
