#include "ui_renderer.hpp"
#include <algorithm>
#include <cmath>

namespace maya::editor {
namespace {
struct UiConstants {
    float scale[2];
    float translate[2];
};
static_assert(sizeof(ImDrawVert) == 20, "editor_ui.metal expects 20-byte ImGui vertices");
static_assert(sizeof(ImDrawIdx) == 4, "maya_imgui_config.h selects 32-bit ImGui indices");
} // namespace

UiRenderer::UiRenderer(GraphicsDevice& device, std::string shader_source)
    : m_device(device), m_shader_source(std::move(shader_source)), m_lifetime(device.resource_lifetime()) {}

UiRenderer::~UiRenderer() {
    if (m_lifetime.expired()) return;
    for (const auto& [format, handle] : m_pipelines) m_device.destroy(handle);
    m_device.destroy(m_sampler);
    m_device.destroy(m_font);
}

ImTextureID UiRenderer::add_texture(TextureHandle texture) {
    m_textures.push_back(texture);
    return static_cast<ImTextureID>(m_textures.size());
}

void UiRenderer::set_texture(ImTextureID id, TextureHandle texture) {
    if (id > 0 && id <= m_textures.size()) m_textures[id - 1] = texture;
}

TextureHandle UiRenderer::lookup(ImTextureID id) const {
    return id > 0 && id <= m_textures.size() ? m_textures[id - 1] : TextureHandle{};
}

RhiDiagnostic UiRenderer::upload_fonts(ImFontAtlas& atlas) {
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels || width <= 0 || height <= 0)
        return {RhiError::invalid_descriptor, "ImGui produced an empty font atlas"};
    auto created = m_device.create_texture({uint32_t(width), uint32_t(height), Format::rgba8_unorm,
                                            TextureUsage::sampled, "editor font atlas"}, pixels);
    if (!created) return created.diagnostic;
    if (!m_lifetime.expired()) m_device.destroy(m_font); // retired after frames that sampled it
    m_font = created.handle;
    if (m_font_id == 0) m_font_id = add_texture(m_font);
    else set_texture(m_font_id, m_font);
    atlas.SetTexID(m_font_id);
    return {};
}

RhiDiagnostic UiRenderer::pipeline(Format format, PipelineHandle& out) {
    if (m_lifetime.expired()) { // a new device session: earlier handles are gone
        m_pipelines.clear();
        m_sampler = {};
        m_lifetime = m_device.resource_lifetime();
    }
    if (const auto found = std::ranges::find(m_pipelines, format, &std::pair<Format, PipelineHandle>::first);
        found != m_pipelines.end()) {
        out = found->second;
        return {};
    }
    auto desc = PipelineDesc{};
    desc.shader_source = m_shader_source;
    desc.vertex_entry = "uiVertex";
    desc.fragment_entry = "uiFragment";
    desc.color_formats = {format};
    desc.cull = CullMode::none;
    desc.blend = BlendMode::alpha;
    desc.label = "editor ui";
    auto created = m_device.create_pipeline(desc);
    if (!created) return created.diagnostic;
    m_pipelines.emplace_back(format, created.handle);
    out = created.handle;
    return {};
}

RhiDiagnostic UiRenderer::render(const ImDrawData& data, TextureHandle destination,
                                 const std::array<double, 4>& clear_color) {
    const auto* target = m_device.describe(destination);
    if (!target) return {RhiError::stale_handle, "UI destination is not a live texture"};
    auto ui = PipelineHandle{};
    if (auto error = pipeline(target->format, ui)) return error;
    if (!m_sampler.valid()) {
        auto sampler = m_device.create_sampler({Filter::linear, Filter::linear, AddressMode::clamp_to_edge,
                                                AddressMode::clamp_to_edge, "editor ui"});
        if (!sampler) return sampler.diagnostic;
        m_sampler = sampler.handle;
    }
    const auto origin = data.DisplayPos;
    const auto size = data.DisplaySize;
    const auto scale = data.FramebufferScale;
    const auto drawable = size.x > 0.0f && size.y > 0.0f;
    auto constants = TransientSlice{};
    if (drawable) {
        // Points to clip space, y down: the display's top-left corner maps to (-1, 1).
        const auto values = UiConstants{{2.0f / size.x, -2.0f / size.y},
                                        {-1.0f - 2.0f * origin.x / size.x, 1.0f + 2.0f * origin.y / size.y}};
        const auto uploaded = m_device.upload_transient(&values, sizeof(values));
        if (!uploaded) return uploaded.diagnostic;
        constants = uploaded.slice;
    }

    auto pass = RenderPassDesc{};
    pass.colors.push_back({destination, LoadAction::clear, StoreAction::store, clear_color});
    pass.label = "editor ui";
    if (auto error = m_device.begin_render_pass(pass)) return error;
    const auto encode = [&]() -> RhiDiagnostic {
        if (!drawable) return {};
        const auto bind_state = [&]() -> RhiDiagnostic {
            if (auto error = m_device.set_pipeline(ui)) return error;
            if (auto error = m_device.set_uniform_buffer(1, constants)) return error;
            return m_device.set_sampler(0, m_sampler);
        };
        if (auto error = bind_state()) return error;
        for (const ImDrawList* list : data.CmdLists) {
            if (list->VtxBuffer.Size == 0 || list->IdxBuffer.Size == 0) continue;
            const auto vertices = m_device.upload_transient(list->VtxBuffer.Data,
                size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert), 4);
            if (!vertices) return vertices.diagnostic;
            const auto indices = m_device.upload_transient(list->IdxBuffer.Data,
                size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx), 4);
            if (!indices) return indices.diagnostic;
            if (auto error = m_device.set_vertex_buffer(0, vertices.slice)) return error;
            auto bound = TextureHandle{};
            for (const auto& command : list->CmdBuffer) {
                if (command.UserCallback) {
                    if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                        if (auto error = bind_state()) return error;
                        if (auto error = m_device.set_vertex_buffer(0, vertices.slice)) return error;
                        bound = {};
                    } else {
                        command.UserCallback(list, &command);
                    }
                    continue;
                }
                // Clip rectangles are in points; the scissor is in destination pixels.
                const auto x0 = std::clamp(std::floor((command.ClipRect.x - origin.x) * scale.x), 0.0f, float(target->width));
                const auto y0 = std::clamp(std::floor((command.ClipRect.y - origin.y) * scale.y), 0.0f, float(target->height));
                const auto x1 = std::clamp(std::ceil((command.ClipRect.z - origin.x) * scale.x), 0.0f, float(target->width));
                const auto y1 = std::clamp(std::ceil((command.ClipRect.w - origin.y) * scale.y), 0.0f, float(target->height));
                if (x1 <= x0 || y1 <= y0 || command.ElemCount == 0) {
                    ++m_stats.clipped;
                    continue;
                }
                const auto texture = lookup(command.GetTexID());
                if (!m_device.describe(texture)) {
                    ++m_stats.missing_textures;
                    continue;
                }
                if (auto error = m_device.set_scissor({uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)}))
                    return error;
                if (texture != bound) {
                    if (auto error = m_device.set_texture(0, texture)) return error;
                    bound = texture;
                }
                if (auto error = m_device.draw_indexed(indices.slice, IndexType::uint32, command.ElemCount,
                                                       size_t(command.IdxOffset) * sizeof(ImDrawIdx)))
                    return error;
                ++m_stats.draws;
            }
        }
        return {};
    };
    const auto result = encode();
    const auto closed = m_device.end_render_pass();
    return result ? result : closed;
}

} // namespace maya::editor
