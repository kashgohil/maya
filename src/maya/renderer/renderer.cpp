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

RhiDiagnostic Renderer::pipeline(Format format, PipelineKind kind, PipelineHandle& out) {
    session_changed();
    const auto found = std::ranges::find_if(m_pipelines, [&](const CachedPipeline& cached) {
        return cached.format == format && cached.kind == kind;
    });
    if (found != m_pipelines.end()) {
        out = found->handle;
        return found->error;
    }
    auto desc = PipelineDesc{};
    desc.shader_source = m_shader_source;
    desc.color_formats = {format};
    if (kind == PipelineKind::present) {
        desc.vertex_entry = "presentVertex";
        desc.fragment_entry = "presentFragment";
        desc.cull = CullMode::none;
        desc.label = "present view";
    } else if (kind == PipelineKind::tone_map) {
        desc.vertex_entry = "toneMapVertex";
        desc.fragment_entry = "toneMapFragment";
        desc.cull = CullMode::none;
        desc.label = "tone map";
    } else if (kind == PipelineKind::debug_front || kind == PipelineKind::debug_behind) {
        // Lines test against the scene's depth without writing it: in front of it, or behind it.
        desc.vertex_entry = "debugVertex";
        desc.fragment_entry = "debugFragment";
        desc.depth_format = Format::depth32_float;
        desc.depth = {true, false, kind == PipelineKind::debug_front ? CompareFunction::less_equal : CompareFunction::greater};
        desc.cull = CullMode::none;
        desc.blend = BlendMode::alpha;
        desc.label = kind == PipelineKind::debug_front ? "debug lines in front" : "debug lines behind";
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
    m_pipelines.push_back({format, kind, created.handle, created.diagnostic});
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
    auto lit = PipelineHandle{}, tone_map = PipelineHandle{};
    if (auto error = pipeline(target.scene_format(), PipelineKind::lit, lit)) return error;
    if (auto error = pipeline(target.color_format(), PipelineKind::tone_map, tone_map)) return error;
    auto debug_front = PipelineHandle{}, debug_behind = PipelineHandle{};
    if (!snapshot.debug.empty()) { // no debug pipelines until something is drawn with them
        if (auto error = pipeline(target.color_format(), PipelineKind::debug_front, debug_front)) return error;
        if (auto error = pipeline(target.color_format(), PipelineKind::debug_behind, debug_behind)) return error;
    }

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

    const auto debug = !snapshot.debug.empty();
    // The scene, into the HDR target. Depth is kept only when debug lines test against it.
    auto pass = RenderPassDesc{};
    pass.colors.push_back({target.scene_color(), LoadAction::clear, StoreAction::store, view.clear_color});
    pass.depth = DepthAttachment{target.depth(), LoadAction::clear, debug ? StoreAction::store : StoreAction::dont_care, 1.0};
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
    auto result = encode();
    if (auto closed = m_device.end_render_pass(); !result) result = std::move(closed);
    if (result) return result;

    // Exposure and tone mapping, into the view's color.
    auto output = RenderPassDesc{};
    output.colors.push_back({target.color(), LoadAction::dont_care, StoreAction::store});
    output.label = "tone map";
    if (auto error = m_device.begin_render_pass(output)) return error;
    const auto tone = [&]() -> RhiDiagnostic {
        const auto constants = ToneMapConstants{view.exposure, uint32_t(view.tone_mapping), uint32_t(view.exposure_view), 0};
        const auto uploaded = m_device.upload_transient(&constants, sizeof(constants));
        if (!uploaded) return uploaded.diagnostic;
        if (auto error = m_device.set_pipeline(tone_map)) return error;
        if (auto error = m_device.set_uniform_buffer(0, uploaded.slice)) return error;
        if (auto error = m_device.set_texture(0, target.scene_color())) return error;
        return m_device.draw(3);
    };
    result = tone();
    if (auto closed = m_device.end_render_pass(); !result) result = std::move(closed);
    if (result || !debug) return result;

    // Debug lines over the tone-mapped image, in their own colors, against the scene's depth.
    auto lines = RenderPassDesc{};
    lines.colors.push_back({target.color(), LoadAction::load, StoreAction::store});
    lines.depth = DepthAttachment{target.depth(), LoadAction::load, StoreAction::dont_care, 1.0};
    lines.label = "debug lines";
    if (auto error = m_device.begin_render_pass(lines)) return error;
    result = encode_debug(snapshot.debug, view, uploaded_view.slice, debug_front, debug_behind);
    if (auto closed = m_device.end_render_pass(); !result) result = std::move(closed);
    return result;
}

// Debug lines and outlines, over the scene: each kind's data is uploaded once and drawn twice, at
// full opacity where it is in front of the scene and faintly where the scene hides it.
RhiDiagnostic Renderer::encode_debug(const DebugDraw& debug, const RenderView& view, const TransientSlice& view_constants,
                                     PipelineHandle front, PipelineHandle behind) {
    constexpr float behind_opacity = 0.3f;
    struct Batch {
        uint32_t kind = 0; // 0 lines, else DebugShapeKind + 1
        uint32_t segments = 0;
        uint32_t count = 0;
        uint32_t vertices = 0; // per instance
        TransientSlice data{};
    };
    auto batches = std::array<Batch, 8>{}; // lines, boxes, and spheres and capsules at three levels of detail
    auto used = size_t{0};
    const auto upload = [&](uint32_t kind, uint32_t segments, uint32_t count, uint32_t vertices) -> RhiDiagnostic {
        const auto uploaded = m_device.upload_transient(m_debug_data.data(), m_debug_data.size() * sizeof(float), 16);
        if (!uploaded) return uploaded.diagnostic;
        batches[used++] = {kind, segments, count, vertices, uploaded.slice};
        return {};
    };
    // How many pixels a world length at a distance covers: the projection's vertical scale.
    const auto pixels_per_unit = view.matrices.projection.elements[5] * float(view.height) * 0.5f;
    const auto segments_of = [&](const DebugShape& shape) {
        if (shape.kind == DebugShapeKind::box) return 0u;
        const auto& m = shape.world.elements;
        const auto centre = math::Vec3{m[12], m[13], m[14]};
        const auto scale = std::max({math::Vec3{m[0], m[1], m[2]}.length(), math::Vec3{m[4], m[5], m[6]}.length(),
                                     math::Vec3{m[8], m[9], m[10]}.length()});
        const auto extent = (shape.size.x + (shape.kind == DebugShapeKind::capsule ? shape.size.y : 0.0f)) * scale;
        const auto distance = std::max((centre - view.position).length() - extent, 1e-3f);
        return debug_segments_for(extent * pixels_per_unit / distance);
    };
    const auto push = [&](const math::Vec3& v, float w) { m_debug_data.insert(m_debug_data.end(), {v.x, v.y, v.z, w}); };
    const auto push_color = [&](const DebugColor& c) { m_debug_data.insert(m_debug_data.end(), {c.x, c.y, c.z, c.w}); };
    if (!debug.lines.empty()) {
        m_debug_data.clear();
        m_debug_data.reserve(debug.lines.size() * debug_line_floats);
        for (const auto& line : debug.lines) {
            push(line.from, 1.0f);
            push(line.to, 1.0f);
            push_color(line.color);
        }
        if (auto error = upload(0, 0, 1, uint32_t(debug.lines.size()) * 6)) return error;
        m_stats.debug_lines += debug.lines.size();
    }
    // Shapes by kind, and spheres and capsules also by how many segments their size on screen needs.
    m_debug_segments.resize(debug.shapes.size());
    for (size_t i = 0; i < debug.shapes.size(); ++i) m_debug_segments[i] = segments_of(debug.shapes[i]);
    const std::pair<DebugShapeKind, uint32_t> groups[] = {{DebugShapeKind::box, 0}, {DebugShapeKind::sphere, 8}, {DebugShapeKind::sphere, 16},
                                                          {DebugShapeKind::sphere, 32}, {DebugShapeKind::capsule, 8},
                                                          {DebugShapeKind::capsule, 16}, {DebugShapeKind::capsule, 32}};
    for (const auto& [kind, segments] : groups) {
        m_debug_data.clear();
        auto count = uint32_t{0};
        for (size_t i = 0; i < debug.shapes.size(); ++i) {
            const auto& shape = debug.shapes[i];
            if (shape.kind != kind || m_debug_segments[i] != segments) continue;
            m_debug_data.insert(m_debug_data.end(), shape.world.elements, shape.world.elements + 16);
            push(shape.size, 0.0f);
            push_color(shape.color);
            ++count;
        }
        if (count == 0) continue;
        if (auto error = upload(uint32_t(kind) + 1, segments, count, debug_template_lines(kind, segments) * 6)) return error;
        m_stats.debug_shapes += count;
    }
    for (const auto [pipeline, opacity] : {std::pair{front, 1.0f}, std::pair{behind, behind_opacity}}) {
        if (auto error = m_device.set_pipeline(pipeline)) return error;
        if (auto error = m_device.set_uniform_buffer(2, view_constants)) return error;
        for (size_t i = 0; i < used; ++i) {
            const auto& batch = batches[i];
            auto constants = DebugConstants{};
            constants.viewport = {float(view.width), float(view.height), view.debug_line_width, opacity};
            constants.kind[0] = batch.kind;
            constants.kind[1] = batch.segments;
            const auto uploaded = m_device.upload_transient(&constants, sizeof(constants));
            if (!uploaded) return uploaded.diagnostic;
            if (auto error = m_device.set_uniform_buffer(1, uploaded.slice)) return error;
            if (auto error = m_device.set_vertex_buffer(0, batch.data)) return error;
            if (auto error = m_device.draw(batch.vertices, 0, batch.count)) return error;
            ++m_stats.debug_draws;
        }
    }
    return {};
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
    if (auto error = pipeline(target->format, PipelineKind::present, present)) return error;
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
