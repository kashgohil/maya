#include "maya/renderer/renderer.hpp"
#include "maya/renderer/shader_constants.hpp"
#include <algorithm>

namespace maya {

Renderer::Renderer(GraphicsDevice& device, std::string shader_source)
    : m_device(device), m_shader_source(std::move(shader_source)), m_lifetime(device.resource_lifetime()) {}

Renderer::~Renderer() { release(); }

void Renderer::release() noexcept {
    if (!m_lifetime.expired()) {
        for (const auto& cached : m_pipelines) m_device.destroy(cached.handle);
        m_device.destroy(m_sampler);
    }
    m_pipelines.clear();
    m_sampler = {};
}

bool Renderer::session_changed() noexcept {
    if (!m_lifetime.expired()) return false;
    // The old session released everything; start again with the current one.
    release();
    m_lifetime = m_device.resource_lifetime();
    return true;
}

RhiDiagnostic Renderer::pipeline(Format format, bool present, PipelineHandle& out) {
    session_changed();
    const auto found = std::ranges::find_if(m_pipelines, [&](const CachedPipeline& cached) {
        return cached.format == format && cached.present == present;
    });
    if (found != m_pipelines.end()) {
        out = found->handle;
        return found->error;
    }
    auto desc = PipelineDesc{};
    desc.shader_source = m_shader_source;
    desc.color_formats = {format};
    if (present) {
        desc.vertex_entry = "presentVertex";
        desc.fragment_entry = "presentFragment";
        desc.cull = CullMode::none;
        desc.label = "present view";
    } else {
        desc.vertex_entry = "litVertex";
        desc.fragment_entry = "litFragment";
        desc.depth_format = Format::depth32_float;
        desc.depth = {true, true, CompareFunction::less};
        desc.cull = CullMode::back; // positive-scale transforms never flip winding
        desc.front_face = Winding::counter_clockwise;
        desc.label = "lit mesh";
    }
    auto created = m_device.create_pipeline(desc);
    // A lost session is not cached, so the next session tries again.
    if (!created && created.diagnostic.code == RhiError::device_unavailable) return created.diagnostic;
    m_pipelines.push_back({format, present, created.handle, created.diagnostic});
    out = created.handle;
    return std::move(created.diagnostic);
}

RhiDiagnostic Renderer::render(const RenderSnapshot& snapshot, const RenderView& view, const RenderTarget& target) {
    if (!target.valid())
        return {RhiError::stale_handle, "Render target has no textures in this device session; resize it first"};
    if (view.width != target.width() || view.height != target.height())
        return {RhiError::invalid_usage, "View is " + std::to_string(view.width) + "x" + std::to_string(view.height) +
            " but its target is " + std::to_string(target.width()) + "x" + std::to_string(target.height())};
    for (const auto& instance : snapshot.instances)
        if (instance.mesh >= snapshot.meshes.size())
            return {RhiError::invalid_usage, "Render snapshot instance refers to a mesh it does not hold"};
    auto lit = PipelineHandle{};
    if (auto error = pipeline(target.color_format(), false, lit)) return error;

    auto constants = ViewConstants{};
    constants.view_projection = view.matrices.view_projection;
    constants.camera_position = {view.position, 1.0f};
    constants.ambient = {snapshot.ambient, 0.0f};
    const auto lights = std::min(snapshot.lights.size(), max_directional_lights);
    constants.light_count[0] = static_cast<uint32_t>(lights);
    for (size_t i = 0; i < lights; ++i)
        constants.lights[i] = {{snapshot.lights[i].direction_to_light, 0.0f}, {snapshot.lights[i].radiance, 0.0f}};
    const auto uploaded_view = m_device.upload_transient(&constants, sizeof(constants));
    if (!uploaded_view) return uploaded_view.diagnostic;

    auto pass = RenderPassDesc{};
    pass.colors.push_back({target.color(), LoadAction::clear, StoreAction::store, view.clear_color});
    pass.depth = DepthAttachment{target.depth(), LoadAction::clear, StoreAction::dont_care, 1.0};
    pass.label = "view";
    if (auto error = m_device.begin_render_pass(pass)) return error;
    ++m_stats.views;
    const auto encode = [&]() -> RhiDiagnostic {
        if (auto error = m_device.set_pipeline(lit)) return error;
        if (auto error = m_device.set_uniform_buffer(2, uploaded_view.slice)) return error;
        for (const auto& instance : snapshot.instances) {
            auto draw = DrawConstants{};
            draw.model = instance.world;
            for (size_t c = 0; c < 3; ++c) draw.normal_matrix[c] = {instance.normal_matrix[c], 0.0f};
            draw.base_color = instance.material.base_color;
            draw.material = {instance.material.metallic, instance.material.roughness, 0.0f, 0.0f};
            const auto uploaded = m_device.upload_transient(&draw, sizeof(draw));
            if (!uploaded) return uploaded.diagnostic;
            if (auto error = m_device.set_uniform_buffer(1, uploaded.slice)) return error;
            if (auto error = snapshot.meshes[instance.mesh].value().mesh().draw()) return error;
            ++m_stats.draws;
        }
        return {};
    };
    const auto result = encode();
    const auto closed = m_device.end_render_pass();
    return result ? result : closed;
}

RhiDiagnostic Renderer::present(const RenderTarget& source, TextureHandle destination, PixelRect area,
                                const std::array<double, 4>& background) {
    if (!source.valid())
        return {RhiError::stale_handle, "Render target has no textures in this device session; resize it first"};
    const auto* target = m_device.describe(destination);
    if (!target) return {RhiError::stale_handle, "Presentation destination is not a live texture"};
    if (area.width == 0 || area.height == 0 || area.x > target->width || area.y > target->height ||
        area.width > target->width - area.x || area.height > target->height - area.y)
        return {RhiError::out_of_range, "Presentation area must be nonempty and inside the " +
            std::to_string(target->width) + "x" + std::to_string(target->height) + " destination"};
    auto present = PipelineHandle{};
    if (auto error = pipeline(target->format, true, present)) return error;
    if (!m_sampler.valid()) {
        auto sampler = m_device.create_sampler({Filter::linear, Filter::linear, AddressMode::clamp_to_edge,
                                                AddressMode::clamp_to_edge, "present view"});
        if (!sampler) return sampler.diagnostic;
        m_sampler = sampler.handle;
    }
    const auto w = float(target->width), h = float(target->height);
    const auto constants = PresentConstants{{2.0f * float(area.x) / w - 1.0f, 1.0f - 2.0f * float(area.y + area.height) / h,
                                             2.0f * float(area.x + area.width) / w - 1.0f, 1.0f - 2.0f * float(area.y) / h}};
    const auto uploaded = m_device.upload_transient(&constants, sizeof(constants));
    if (!uploaded) return uploaded.diagnostic;

    auto pass = RenderPassDesc{};
    pass.colors.push_back({destination, LoadAction::clear, StoreAction::store, background});
    pass.label = "present view";
    if (auto error = m_device.begin_render_pass(pass)) return error;
    const auto encode = [&]() -> RhiDiagnostic {
        if (auto error = m_device.set_pipeline(present)) return error;
        if (auto error = m_device.set_uniform_buffer(1, uploaded.slice)) return error;
        if (auto error = m_device.set_texture(0, source.color())) return error;
        if (auto error = m_device.set_sampler(0, m_sampler)) return error;
        return m_device.draw(6);
    };
    const auto result = encode();
    const auto closed = m_device.end_render_pass();
    if (!result && !closed) ++m_stats.presents;
    return result ? result : closed;
}

} // namespace maya
