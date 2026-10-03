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
    uint64_t debug_draws = 0; // instanced draws of debug lines and outlines, both depth passes
    uint64_t debug_shapes = 0; // outlines drawn, counted once per view
    uint64_t debug_lines = 0; // lines drawn, counted once per view
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

    /// Inside a frame with no pass open, in three passes (docs/renderer.md#exposure-and-tone-mapping):
    /// `view` clears the target's HDR scene color to the view's clear color and draws every instance
    /// with its own constants and material (docs/renderer.md#materials): opaque and masked ones in
    /// snapshot order, then blended ones back to front; `tone map` scales it by the view's exposure and tone-maps it into the
    /// target's color, sRGB-encoded; `debug lines`, only when the snapshot has any, draws its lines and
    /// outlines over that (brightly in front of the scene, faintly behind it) in their own colors. The
    /// view size must match the target.
    /// Returns the first error (e.g. exhausted upload memory); later instances are not drawn, each
    /// open pass is still closed, and the frame can still end.
    RhiDiagnostic render(const RenderSnapshot& snapshot, const RenderView& view, const RenderTarget& target);
    /// Inside a frame with no pass open: clears `destination` (e.g. the acquired surface) to
    /// `background` and draws the target's color texture scaled into `area`.
    RhiDiagnostic present(const RenderTarget& source, TextureHandle destination, PixelRect area,
                          const std::array<double, 4>& background = {0.0, 0.0, 0.0, 1.0});
    const RendererStats& stats() const noexcept { return m_stats; }

private:
    // Lit surfaces by alpha mode (blend or not) and sidedness; masks discard in the opaque pipelines.
    enum class PipelineKind : uint8_t { lit, present, debug_front, debug_behind, tone_map, lit_double_sided, lit_blend,
                                        lit_blend_double_sided };
    struct CachedPipeline {
        Format format = Format::undefined;
        PipelineKind kind = PipelineKind::lit;
        PipelineHandle handle;
        RhiDiagnostic error; // a failed compile is not retried every frame
    };
    RhiDiagnostic pipeline(Format format, PipelineKind kind, PipelineHandle& out);
    RhiDiagnostic encode_debug(const DebugDraw& debug, const RenderView& view, const TransientSlice& view_constants,
                               PipelineHandle front, PipelineHandle behind);
    bool session_changed() noexcept;
    void release() noexcept;

    GraphicsDevice& m_device;
    std::string m_shader_source;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    std::vector<CachedPipeline> m_pipelines;
    SamplerHandle m_sampler;
    std::shared_ptr<const TextureAsset> m_placeholder; // drawn for missing textures, and bound to empty slots
    std::vector<uint32_t> m_order; // instances in drawing order, reused
    RendererStats m_stats{};
    std::vector<float> m_debug_data; // packed debug lines or outlines of one kind, reused
    std::vector<uint32_t> m_debug_segments; // each outline's circle segments, reused
};

} // namespace maya
