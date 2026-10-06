#pragma once

#include "maya/renderer/draw_batches.hpp"
#include "maya/renderer/light_plan.hpp"
#include "maya/renderer/shader_constants.hpp"
#include "maya/renderer/render_snapshot.hpp"
#include "maya/renderer/render_target.hpp"
#include "maya/core/texture.hpp"
#include <array>
#include <functional>
#include <optional>
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
    uint64_t draws = 0; // the views' instanced draws, one per batch (#1025)
    uint64_t instances = 0; // instances drawn in views
    uint64_t culled = 0; // instances left out of views by their frustum
    uint64_t presents = 0;
    uint64_t debug_draws = 0; // instanced draws of debug lines and outlines, both depth passes
    uint64_t debug_shapes = 0; // outlines drawn, counted once per view
    uint64_t debug_lines = 0; // lines drawn, counted once per view
    uint64_t shadow_maps = 0; // cascades and spot light maps rendered
    uint64_t shadow_draws = 0; // instanced draws into them, one per batch
    uint64_t shadow_instances = 0; // instances drawn into them
};
/// What the last view drew (docs/renderer.md#culling-and-batching).
struct RenderViewReport {
    size_t drawn = 0; // instances inside the view
    size_t culled = 0; // instances outside it
    size_t batches = 0; // the view pass's instanced draws
};
/// What lit the last view (docs/renderer.md#lights): lights beyond the limits are reported here, never
/// silently dropped. The editor shows it in Diagnostics.
struct RenderLightReport {
    size_t local = 0; // point and spot lights drawn
    std::vector<EntityId> dropped; // reaching the view beyond max_local_lights: not drawn
    std::vector<EntityId> unshadowed; // spot lights casting shadows beyond max_shadowed_spot_lights: drawn without
    std::optional<EntityId> sun; // the directional light with cascades
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
    /// snapshot order, then the environment's sky where nothing was drawn, then blended ones back to front; `tone map` scales it by the view's exposure and tone-maps it into the
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
    /// The last view's lights.
    const RenderLightReport& last_lights() const noexcept { return m_lights; }
    const RenderViewReport& last_view() const noexcept { return m_view_report; }

private:
    // Lit surfaces by alpha mode (blend or not) and sidedness; masks discard in the opaque pipelines.
    enum class PipelineKind : uint8_t { lit, present, debug_front, debug_behind, tone_map, lit_double_sided, lit_blend,
                                        lit_blend_double_sided, sky, shadow, shadow_masked };
    struct CachedPipeline {
        Format format = Format::undefined;
        PipelineKind kind = PipelineKind::lit;
        bool debug = false; // compiled with the debug views (docs/renderer.md#debug-views)
        PipelineHandle handle;
        RhiDiagnostic error; // a failed compile is not retried every frame
    };
    RhiDiagnostic pipeline(Format format, PipelineKind kind, PipelineHandle& out, bool debug = false);
    RhiDiagnostic encode_debug(const DebugDraw& debug, const RenderView& view, const TransientSlice& view_constants,
                               PipelineHandle front, PipelineHandle behind);
    /// Renders one atlas of 2 x 2 shadow maps: each from its view-projection, drawing its batches.
    RhiDiagnostic encode_shadow_atlas(const Texture& atlas, const RenderSnapshot& snapshot, const char* label,
                                      const std::vector<math::Mat4>& maps, const std::vector<std::vector<DrawBatch>>& batches);
    /// Binds the frame's instances and draw order (buffers 1 and 4) in the open pass.
    RhiDiagnostic bind_instances();
    RhiDiagnostic prepare_shadows(const LightPlan& plan);
    bool session_changed() noexcept;
    void release() noexcept;

    GraphicsDevice& m_device;
    std::string m_shader_source;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    std::vector<CachedPipeline> m_pipelines;
    SamplerHandle m_sampler;
    std::shared_ptr<const TextureAsset> m_placeholder; // drawn for missing textures, and bound to empty slots
    /// The split-sum table (brdf_table), and a black cube bound in the environment's slots when there is none.
    std::unique_ptr<Texture> m_brdf_table, m_empty_cube;
    std::unique_ptr<Sampler> m_table_sampler;
    /// Shadow maps (docs/renderer.md#shadows): the sun's four cascades and the spot lights' four maps, each in
    /// a 2 x 2 atlas made on first use; a cleared 1 x 1 depth texture bound in their place when there are none.
    std::unique_ptr<Texture> m_sun_atlas, m_spot_atlas, m_no_shadows;
    bool m_no_shadows_cleared = false;
    std::unique_ptr<Sampler> m_shadow_sampler; // linear comparison: filtered 0/1 results
    /// Each view's instances (their transforms) and every pass's drawing order, uploaded once per view and
    /// read by instanced draws (#1025).
    std::vector<DrawConstants> m_instance_data;
    DrawList m_draws;
    BatchScratch m_batch_scratch;
    TransientSlice m_instances, m_order;
    RenderLightReport m_lights;
    RenderViewReport m_view_report;
    RendererStats m_stats{};
    std::vector<float> m_debug_data; // packed debug lines or outlines of one kind, reused
    std::vector<uint32_t> m_debug_segments; // each outline's circle segments, reused
};

} // namespace maya
