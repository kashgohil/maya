#pragma once

#include "maya/renderer/render_snapshot.hpp"
#include "maya/renderer/render_target.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace maya {

/// A rectangle in the destination's framebuffer pixels, origin at the top left.
struct PixelRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
struct RendererStats {
    uint64_t views = 0;
    uint64_t draws = 0;
    uint64_t presents = 0;
};

/// Turns render snapshots into images. It owns pipelines and a sampler, created on first use for
/// each target format, and never reads a World. Single-owner thread, like the device it borrows.
/// The device must outlive the renderer; after a device session ends, the next call starts over.
class Renderer {
public:
    /// `shader_source` is the Metal Shading Language source of resources/shaders/metal/renderer.metal.
    Renderer(GraphicsDevice& device, std::string shader_source);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Inside a frame with no pass open: clears the target to the view's clear color, draws every
    /// instance with its own constants, and closes the pass. The view size must match the target.
    /// Returns the first error (e.g. exhausted upload memory); later instances are not drawn, the
    /// pass is still closed, and the frame can still end.
    RhiDiagnostic render(const RenderSnapshot& snapshot, const RenderView& view, const RenderTarget& target);
    /// Inside a frame with no pass open: clears `destination` (e.g. the acquired surface) to
    /// `background` and draws the target's color texture scaled into `area`.
    RhiDiagnostic present(const RenderTarget& source, TextureHandle destination, PixelRect area,
                          const std::array<double, 4>& background = {0.0, 0.0, 0.0, 1.0});
    const RendererStats& stats() const noexcept { return m_stats; }

private:
    struct CachedPipeline {
        Format format = Format::undefined;
        bool present = false;
        PipelineHandle handle;
        RhiDiagnostic error; // a failed compile is not retried every frame
    };
    RhiDiagnostic pipeline(Format format, bool present, PipelineHandle& out);
    bool session_changed() noexcept;
    void release() noexcept;

    GraphicsDevice& m_device;
    std::string m_shader_source;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    std::vector<CachedPipeline> m_pipelines;
    SamplerHandle m_sampler;
    RendererStats m_stats{};
};

} // namespace maya
