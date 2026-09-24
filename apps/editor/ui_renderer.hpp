#pragma once

#include "maya/rhi/graphics_device.hpp"
#include <imgui.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace maya::editor {

struct UiRendererStats {
    uint64_t draws = 0;
    uint64_t clipped = 0; // commands whose clip rectangle was empty after clamping
    uint64_t missing_textures = 0; // commands skipped because their texture was not live
};

/// Draws Dear ImGui draw data through the GraphicsDevice: vertices and indices go to frame upload
/// memory, clip rectangles become scissor rectangles, and texture IDs name device textures.
class UiRenderer {
public:
    /// `shader_source` is resources/shaders/metal/editor_ui.metal. The device must outlive this.
    UiRenderer(GraphicsDevice& device, std::string shader_source);
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    /// Uploads the font atlas as a texture (replacing any previous one) and sets its texture ID.
    RhiDiagnostic upload_fonts(ImFontAtlas& atlas);
    /// Reserves a texture ID. Its texture can change every frame, e.g. when a view is resized.
    ImTextureID add_texture(TextureHandle texture = {});
    void set_texture(ImTextureID id, TextureHandle texture);
    /// Inside a frame with no pass open: clears `destination` and draws the UI into it.
    RhiDiagnostic render(const ImDrawData& data, TextureHandle destination,
                         const std::array<double, 4>& clear_color);
    const UiRendererStats& stats() const noexcept { return m_stats; }

private:
    TextureHandle lookup(ImTextureID id) const;
    RhiDiagnostic pipeline(Format format, PipelineHandle& out);

    GraphicsDevice& m_device;
    std::string m_shader_source;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    std::vector<std::pair<Format, PipelineHandle>> m_pipelines;
    SamplerHandle m_sampler;
    TextureHandle m_font;
    ImTextureID m_font_id = 0;
    std::vector<TextureHandle> m_textures; // ID n refers to m_textures[n - 1]
    UiRendererStats m_stats{};
};

} // namespace maya::editor
