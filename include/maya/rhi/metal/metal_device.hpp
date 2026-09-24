#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <memory>

namespace maya {

/// Metal backend. Frames use command buffers that do not retain resources, so GraphicsDevice's
/// deferred retirement is what keeps destroyed resources alive until their frames complete.
class MetalDevice final : public GraphicsDevice {
public:
    MetalDevice();
    ~MetalDevice() override;

    /// Native objects currently owned by the backend, including ones awaiting retirement.
    size_t native_buffer_count() const noexcept;
    size_t native_texture_count() const noexcept;

protected:
    bool backend_initialize(void* native_window, RhiLimits& limits, Format& surface_format) override;
    void backend_shutdown() noexcept override;
    void backend_resize(uint32_t width, uint32_t height) override;
    RhiDiagnostic backend_create_buffer(uint32_t slot, const BufferDesc& desc, const void* data) override;
    RhiDiagnostic backend_create_texture(uint32_t slot, const TextureDesc& desc, const void* data) override;
    RhiDiagnostic backend_create_sampler(uint32_t slot, const SamplerDesc& desc) override;
    RhiDiagnostic backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) override;
    void backend_release(ResourceKind kind, uint32_t slot) noexcept override;
    void backend_write_buffer(uint32_t slot, size_t offset, const void* data, size_t size) noexcept override;
    RhiDiagnostic backend_read_texture(uint32_t slot, const TextureDesc& desc, std::vector<std::byte>& pixels) override;
    RhiDiagnostic backend_begin_frame() override;
    BackendSurface backend_acquire_surface(uint32_t slot) override;
    RhiDiagnostic backend_begin_pass(const RenderPassDesc& desc) override;
    void backend_set_pipeline(uint32_t slot) override;
    void backend_set_vertex_buffer(uint32_t index, uint32_t slot, size_t offset) override;
    void backend_set_uniform_buffer(uint32_t index, uint32_t slot, size_t offset) override;
    void backend_set_texture(uint32_t index, uint32_t slot) override;
    void backend_set_sampler(uint32_t index, uint32_t slot) override;
    void backend_draw(uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count) override;
    void backend_draw_indexed(uint32_t slot, IndexType type, uint32_t index_count, size_t offset,
                              uint32_t instance_count) override;
    void backend_end_pass() override;
    void backend_submit(uint64_t serial, bool present) override;
    void backend_abandon_frame() noexcept override;
    void backend_wait_idle() noexcept override;
    bool backend_wait_frame(uint64_t serial) noexcept override;
    void backend_release_surface(uint32_t slot) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace maya
