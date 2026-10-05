#include "maya/renderer/renderer.hpp"
#include "maya/renderer/shader_constants.hpp"
#include "maya/assets/environment_cook.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace maya {
namespace {
/// A general 4x4 inverse in double precision, for the sky's view rays; identity when singular.
math::Mat4 inverse(const math::Mat4& m) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m.at(r, c) : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        auto pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (std::abs(a[pivot][c]) < 1e-30) return math::Mat4::identity();
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const auto scale = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const auto factor = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    auto result = math::Mat4::identity();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) result.at(r, c) = float(a[r][c + 4]);
    return result;
}
/// The split-sum table, computed once per process.
std::span<const std::byte> split_sum_table() {
    static const auto table = brdf_table();
    return table;
}
} // namespace

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
    m_placeholder.reset(); // a lost session already retired its texture
    m_brdf_table.reset();
    m_empty_cube.reset();
    m_table_sampler.reset();
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
    } else if (kind == PipelineKind::sky) {
        // At the far plane, where the depth was cleared: only where no surface was drawn.
        desc.vertex_entry = "skyVertex";
        desc.fragment_entry = "skyFragment";
        desc.depth_format = Format::depth32_float;
        desc.depth = {true, false, CompareFunction::less_equal};
        desc.cull = CullMode::none;
        desc.label = "sky";
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
        const auto blend = kind == PipelineKind::lit_blend || kind == PipelineKind::lit_blend_double_sided;
        const auto double_sided = kind == PipelineKind::lit_double_sided || kind == PipelineKind::lit_blend_double_sided;
        desc.vertex_entry = "litVertex";
        desc.fragment_entry = "litFragment";
        desc.depth_format = Format::depth32_float;
        // Blended surfaces are tested against the opaque ones but do not hide what is drawn after them.
        desc.depth = {true, !blend, CompareFunction::less};
        desc.blend = blend ? BlendMode::alpha : BlendMode::opaque;
        desc.cull = double_sided ? CullMode::none : CullMode::back; // positive-scale transforms never flip winding
        desc.front_face = Winding::counter_clockwise;
        desc.label = std::string(blend ? "blended" : "lit") + (double_sided ? " double-sided" : "") + " mesh";
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
    for (const auto& instance : snapshot.instances) {
        if (instance.mesh >= snapshot.meshes.size())
            return {RhiError::invalid_usage, "Render snapshot instance refers to a mesh it does not hold"};
        for (const auto texture : instance.material.textures)
            if (texture != no_texture && texture != placeholder_texture && texture >= snapshot.textures.size())
                return {RhiError::invalid_usage, "Render snapshot instance refers to a texture it does not hold"};
    }
    // Lit pipelines are created when a material first needs them.
    const auto lit_kind = [](const RenderMaterial& material) {
        if (material.alpha_mode == AlphaMode::blend)
            return material.double_sided ? PipelineKind::lit_blend_double_sided : PipelineKind::lit_blend;
        return material.double_sided ? PipelineKind::lit_double_sided : PipelineKind::lit;
    };
    auto lit = std::array<PipelineHandle, 4>{}; // lit, double-sided, blend, blend double-sided
    const auto lit_index = [](PipelineKind kind) {
        return kind == PipelineKind::lit ? 0 : kind == PipelineKind::lit_double_sided ? 1 : kind == PipelineKind::lit_blend ? 2 : 3;
    };
    for (const auto& instance : snapshot.instances) {
        const auto kind = lit_kind(instance.material);
        if (!lit[lit_index(kind)].valid())
            if (auto error = pipeline(target.scene_format(), kind, lit[lit_index(kind)])) return error;
    }
    auto tone_map = PipelineHandle{};
    if (auto error = pipeline(target.color_format(), PipelineKind::tone_map, tone_map)) return error;
    if (!m_placeholder) { // pipeline() above drops it when the device session changed
        m_placeholder = make_placeholder_texture(m_device);
        if (!m_placeholder) return {RhiError::device_unavailable, "The texture placeholder could not be created"};
    }
    if (!m_brdf_table) {
        m_brdf_table = std::make_unique<Texture>(m_device, TextureDesc{brdf_table_size, brdf_table_size, Format::rgba16_float,
                                                                       TextureUsage::sampled, "split-sum table"}, split_sum_table());
        const auto black = std::vector<std::byte>(6 * 8);
        m_empty_cube = std::make_unique<Texture>(m_device, TextureDesc{1, 1, Format::rgba16_float, TextureUsage::sampled,
                                                                       "no environment", 1, TextureType::cube}, black);
        m_table_sampler = std::make_unique<Sampler>(m_device, SamplerDesc{Filter::linear, Filter::linear, AddressMode::clamp_to_edge,
                                                                          AddressMode::clamp_to_edge, "split-sum table"});
        if (!m_brdf_table->valid() || !m_empty_cube->valid() || !m_table_sampler->valid())
            return {RhiError::device_unavailable, "The renderer's environment textures could not be created"};
    }
    const auto& environment = snapshot.environment;
    const auto sky = environment && environment->background;
    auto sky_pipeline = PipelineHandle{};
    if (sky)
        if (auto error = pipeline(target.scene_format(), PipelineKind::sky, sky_pipeline)) return error;
    // Opaque and masked surfaces in snapshot order, then blended ones back to front.
    m_order.clear();
    for (uint32_t i = 0; i < snapshot.instances.size(); ++i)
        if (snapshot.instances[i].material.alpha_mode != AlphaMode::blend) m_order.push_back(i);
    const auto opaque = m_order.size();
    for (uint32_t i = 0; i < snapshot.instances.size(); ++i)
        if (snapshot.instances[i].material.alpha_mode == AlphaMode::blend) m_order.push_back(i);
    const auto distance = [&](uint32_t i) {
        const auto& world = snapshot.instances[i].world;
        return (math::Vec3{world.at(0, 3), world.at(1, 3), world.at(2, 3)} - view.position).length_squared();
    };
    std::stable_sort(m_order.begin() + std::ptrdiff_t(opaque), m_order.end(),
                     [&](uint32_t a, uint32_t b) { return distance(a) > distance(b); });
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
    constants.inverse_view_projection = inverse(view.matrices.view_projection);
    if (environment) {
        const auto& asset = environment->asset.value();
        constants.environment = {environment->intensity, std::cos(environment->rotation), std::sin(environment->rotation),
                                 float(asset.specular_levels() - 1)};
        constants.environment_flags[0] = 1;
        constants.environment_flags[1] = sky ? 1 : 0;
        for (size_t i = 0; i < 9; ++i) constants.irradiance[i] = {asset.irradiance()[i], 0.0f};
    }
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
        auto bound_pipeline = PipelineHandle{};
        auto bound = std::array<const TextureAsset*, material_slots>{}; // rebound only when they change
        auto bound_material = std::optional<MaterialConstants>{}; // uploaded only when it changes
        auto bound_maps = std::optional<std::array<uint32_t, material_slots>>{};
        // The environment's textures and the split-sum table stay bound for the whole pass.
        const auto* cube = environment ? &environment->asset.value().specular() : m_empty_cube.get();
        const auto* environment_sampler = environment ? &environment->asset.value().sampler() : m_table_sampler.get();
        if (auto error = m_device.set_texture(5, cube->handle())) return error;
        if (auto error = m_device.set_sampler(5, environment_sampler->handle())) return error;
        if (auto error = m_device.set_texture(7, m_brdf_table->handle())) return error;
        if (auto error = m_device.set_sampler(7, m_table_sampler->handle())) return error;
        auto sky_drawn = !sky;
        const auto draw_sky = [&]() -> RhiDiagnostic { // after opaque surfaces, before blended ones
            sky_drawn = true;
            if (auto error = m_device.set_pipeline(sky_pipeline)) return error;
            bound_pipeline = sky_pipeline;
            if (auto error = m_device.set_uniform_buffer(2, uploaded_view.slice)) return error;
            if (auto error = m_device.set_texture(6, environment->asset.value().background().handle())) return error;
            return m_device.draw(3);
        };
        for (const auto index : m_order) {
            const auto& instance = snapshot.instances[index];
            const auto& material = instance.material;
            if (!sky_drawn && material.alpha_mode == AlphaMode::blend)
                if (auto error = draw_sky()) return error;
            if (const auto pipeline = lit[lit_index(lit_kind(material))]; pipeline != bound_pipeline) {
                if (auto error = m_device.set_pipeline(pipeline)) return error;
                if (!bound_pipeline.valid())
                    if (auto error = m_device.set_uniform_buffer(2, uploaded_view.slice)) return error;
                bound_pipeline = pipeline;
            }
            auto constants = MaterialConstants{};
            constants.base_color = material.base_color;
            constants.factors = {material.metallic, material.roughness, material.normal_scale, material.occlusion_strength};
            constants.emissive = {material.emissive, material.alpha_cutoff};
            constants.flags[1] = uint32_t(material.alpha_mode);
            constants.uv_transform = material_uv_transform(material.uv_rotation, material.uv_scale);
            constants.uv_offset = {material.uv_offset.x, material.uv_offset.y, 0.0f, 0.0f};
            for (uint32_t slot = 0; slot < material_slots; ++slot)
                if (material.textures[slot] != no_texture) constants.flags[0] |= 1u << slot;
            if (bound_maps != material.textures) {
                for (uint32_t slot = 0; slot < material_slots; ++slot) {
                    // Empty slots are not sampled, but every slot is bound.
                    const auto texture = material.textures[slot];
                    const auto* asset = texture < snapshot.textures.size() ? &snapshot.textures[texture].value() : m_placeholder.get();
                    if (asset == bound[slot]) continue;
                    if (auto error = m_device.set_texture(slot, asset->texture().handle())) return error;
                    if (auto error = m_device.set_sampler(slot, asset->sampler().handle())) return error;
                    bound[slot] = asset;
                }
                bound_maps = material.textures;
            }
            if (!bound_material || std::memcmp(&*bound_material, &constants, sizeof(constants)) != 0) {
                const auto uploaded = m_device.upload_transient(&constants, sizeof(constants));
                if (!uploaded) return uploaded.diagnostic;
                if (auto error = m_device.set_uniform_buffer(3, uploaded.slice)) return error;
                bound_material = constants;
            }
            auto draw = DrawConstants{};
            draw.model = instance.world;
            for (size_t c = 0; c < 3; ++c) draw.normal_matrix[c] = {instance.normal_matrix[c], 0.0f};
            const auto uploaded = m_device.upload_transient(&draw, sizeof(draw));
            if (!uploaded) return uploaded.diagnostic;
            if (auto error = m_device.set_uniform_buffer(1, uploaded.slice)) return error;
            if (auto error = snapshot.meshes[instance.mesh].value().mesh().draw()) return error;
            ++m_stats.draws;
        }
        if (!sky_drawn) return draw_sky();
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
