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
MaterialConstants material_constants(const RenderMaterial& material) {
    auto constants = MaterialConstants{};
    constants.base_color = material.base_color;
    constants.factors = {material.metallic, material.roughness, material.normal_scale, material.occlusion_strength};
    constants.emissive = {material.emissive, material.alpha_cutoff};
    constants.flags[1] = uint32_t(material.alpha_mode);
    constants.uv_transform = material_uv_transform(material.uv_rotation, material.uv_scale);
    constants.uv_offset = {material.uv_offset.x, material.uv_offset.y, 0.0f, 0.0f};
    for (uint32_t slot = 0; slot < material_slots; ++slot)
        if (material.textures[slot] != no_texture) constants.flags[0] |= 1u << slot;
    return constants;
}
/// Slope-scaled depth bias in shadow maps, against acne where surfaces face the light at grazing angles.
constexpr float shadow_slope_bias = 2.0f;
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
    m_sun_atlas.reset();
    m_spot_atlas.reset();
    m_no_shadows.reset();
    m_no_shadows_cleared = false;
    m_shadow_sampler.reset();
}

bool Renderer::session_changed() noexcept {
    if (!m_lifetime.expired()) return false;
    // The old session released everything; start again with the current one.
    release();
    m_lifetime = m_device.resource_lifetime();
    return true;
}

RhiDiagnostic Renderer::pipeline(Format format, PipelineKind kind, PipelineHandle& out, bool debug, bool skinned) {
    session_changed();
    const auto found = std::ranges::find_if(m_pipelines, [&](const CachedPipeline& cached) {
        return cached.format == format && cached.kind == kind && cached.debug == debug && cached.skinned == skinned;
    });
    if (found != m_pipelines.end()) {
        out = found->handle;
        return found->error;
    }
    auto desc = PipelineDesc{};
    // Debug views are compiled only into their own pipelines, so the lit ones never carry them.
    desc.shader_source = debug ? "#define MAYA_DEBUG_VIEWS 1\n" + m_shader_source : m_shader_source;
    desc.color_formats = {format};
    if (kind == PipelineKind::shadow || kind == PipelineKind::shadow_masked) {
        // Depth alone; masked materials cut themselves out. Both faces cast, so thin and open surfaces do.
        desc.color_formats.clear();
        desc.vertex_entry = skinned ? "shadowSkinnedVertex" : "shadowVertex";
        desc.fragment_entry = kind == PipelineKind::shadow_masked ? "shadowMaskFragment" : "";
        desc.depth_format = Format::depth32_float;
        desc.depth = {true, true, CompareFunction::less};
        desc.cull = CullMode::none;
        desc.label = std::string(kind == PipelineKind::shadow_masked ? "masked shadow caster" : "shadow caster") + (skinned ? " (skinned)" : "");
    } else if (kind == PipelineKind::present) {
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
        desc.vertex_entry = skinned ? "litSkinnedVertex" : "litVertex";
        desc.fragment_entry = "litFragment";
        desc.depth_format = Format::depth32_float;
        // Blended surfaces are tested against the opaque ones but do not hide what is drawn after them.
        desc.depth = {true, !blend, CompareFunction::less};
        desc.blend = blend ? BlendMode::alpha : BlendMode::opaque;
        desc.cull = double_sided ? CullMode::none : CullMode::back; // positive-scale transforms never flip winding
        desc.front_face = Winding::counter_clockwise;
        desc.label = std::string(blend ? "blended" : "lit") + (double_sided ? " double-sided" : "") + " mesh" + (skinned ? " (skinned)" : "") + (debug ? " (debug view)" : "");
    }
    auto created = m_device.create_pipeline(desc);
    // A lost session is not cached, so the next session tries again.
    if (!created && created.diagnostic.code == RhiError::device_unavailable) return created.diagnostic;
    m_pipelines.push_back({format, kind, debug, skinned, created.handle, created.diagnostic});
    out = created.handle;
    return std::move(created.diagnostic);
}

RhiDiagnostic Renderer::prepare_shadows(const LightPlan& plan) {
    const auto depth = [&](uint32_t size, const char* label) {
        return std::make_unique<Texture>(m_device, TextureDesc{size, size, Format::depth32_float,
                                                               TextureUsage::sampled | TextureUsage::render_target, label},
                                         std::span<const std::byte>{});
    };
    if (!m_shadow_sampler) {
        m_shadow_sampler = std::make_unique<Sampler>(m_device, SamplerDesc{Filter::linear, Filter::linear, AddressMode::clamp_to_edge,
            AddressMode::clamp_to_edge, "shadow comparison", MipFilter::none, 1, CompareFunction::less_equal});
        m_no_shadows = depth(1, "no shadows");
        m_no_shadows_cleared = false;
    }
    if (plan.sun && !m_sun_atlas) m_sun_atlas = depth(2 * sun_cascade_size, "sun shadow atlas");
    if (!plan.spot_shadows.empty() && !m_spot_atlas) m_spot_atlas = depth(2 * spot_shadow_size, "spot shadow atlas");
    for (const auto* texture : {m_no_shadows.get(), plan.sun ? m_sun_atlas.get() : nullptr, plan.spot_shadows.empty() ? nullptr : m_spot_atlas.get()})
        if (texture && !texture->valid()) return texture->error() ? texture->error() : RhiDiagnostic{RhiError::device_unavailable, "A shadow map could not be created"};
    if (!m_shadow_sampler->valid()) return m_shadow_sampler->error();
    if (!m_no_shadows_cleared) { // depth 1: everything lit
        auto pass = RenderPassDesc{};
        pass.depth = DepthAttachment{m_no_shadows->handle(), LoadAction::clear, StoreAction::store, 1.0};
        pass.label = "no shadows";
        if (auto error = m_device.begin_render_pass(pass)) return error;
        if (auto error = m_device.end_render_pass()) return error;
        m_no_shadows_cleared = true;
    }
    return {};
}

RhiDiagnostic Renderer::bind_instances() {
    if (auto error = m_device.set_uniform_buffer(1, m_instances)) return error;
    if (m_palette.size > 0)
        if (auto error = m_device.set_uniform_buffer(6, m_palette)) return error;
    return m_device.set_uniform_buffer(4, m_order);
}

RhiDiagnostic Renderer::encode_shadow_atlas(const Texture& atlas, const RenderSnapshot& snapshot, const char* label,
                                            const std::vector<math::Mat4>& maps, const std::vector<std::vector<DrawBatch>>& batches) {
    // Opaque and masked casters, each unskinned and skinned; skinned ones only when a skinned mesh casts.
    auto casters = std::array<std::array<PipelineHandle, 2>, 2>{};
    for (const auto& list : batches)
        for (const auto& batch : list) {
            const auto skinned = snapshot.meshes[batch.mesh].value().mesh().skinned();
            const auto mask = snapshot.materials[batch.material].alpha_mode == AlphaMode::mask;
            if (!casters[skinned][mask].valid())
                if (auto error = pipeline(Format::depth32_float, mask ? PipelineKind::shadow_masked : PipelineKind::shadow,
                                          casters[skinned][mask], false, skinned)) return error;
        }
    auto pass = RenderPassDesc{};
    pass.depth = DepthAttachment{atlas.handle(), LoadAction::clear, StoreAction::store, 1.0};
    pass.label = label;
    if (auto error = m_device.begin_render_pass(pass)) return error;
    const auto encode = [&]() -> RhiDiagnostic {
        const auto size = atlas.desc().width / 2;
        if (auto error = m_device.set_depth_bias(0.0f, shadow_slope_bias)) return error;
        if (m_order.size > 0)
            if (auto error = bind_instances()) return error;
        for (uint32_t map = 0; map < maps.size(); ++map) {
            const auto x = (map & 1) * size, y = (map >> 1) * size;
            if (auto error = m_device.set_viewport({float(x), float(y), float(size), float(size)})) return error;
            if (auto error = m_device.set_scissor({x, y, size, size})) return error;
            const auto constants = ShadowConstants{maps[map]};
            const auto uploaded = m_device.upload_transient(&constants, sizeof(constants));
            if (!uploaded) return uploaded.diagnostic;
            auto bound = PipelineHandle{};
            for (const auto& batch : batches[map]) { // opaque casters by mesh, then masked ones by material and mesh
                const auto& material = snapshot.materials[batch.material];
                const auto mask = material.alpha_mode == AlphaMode::mask;
                const auto skinned = snapshot.meshes[batch.mesh].value().mesh().skinned();
                if (const auto wanted = casters[skinned][mask]; wanted != bound) {
                    if (auto error = m_device.set_pipeline(wanted)) return error;
                    if (auto error = m_device.set_uniform_buffer(2, uploaded.slice)) return error;
                    bound = wanted;
                }
                if (mask) {
                    const auto constants = material_constants(material);
                    const auto material_uploaded = m_device.upload_transient(&constants, sizeof(constants));
                    if (!material_uploaded) return material_uploaded.diagnostic;
                    if (auto error = m_device.set_uniform_buffer(3, material_uploaded.slice)) return error;
                    const auto texture = material.textures[size_t(MaterialSlot::base_color)];
                    const auto* asset = texture < snapshot.textures.size() ? &snapshot.textures[texture].value() : m_placeholder.get();
                    if (auto error = m_device.set_texture(0, asset->texture().handle())) return error;
                    if (auto error = m_device.set_sampler(0, asset->sampler().handle())) return error;
                }
                if (auto error = snapshot.meshes[batch.mesh].value().mesh().draw(batch.count, batch.first)) return error;
                ++m_stats.shadow_draws;
                m_stats.shadow_instances += batch.count;
            }
            ++m_stats.shadow_maps;
        }
        return {};
    };
    auto result = encode();
    if (auto closed = m_device.end_render_pass(); !result) result = std::move(closed);
    return result;
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
        if (instance.material >= snapshot.materials.size())
            return {RhiError::invalid_usage, "Render snapshot instance refers to a material it does not hold"};
        if (instance.joint_count > 0 && (instance.first_joint > snapshot.joints.size() ||
                                         instance.joint_count > snapshot.joints.size() - instance.first_joint))
            return {RhiError::invalid_usage, "Render snapshot instance refers to joints it does not hold"};
    }
    for (const auto& material : snapshot.materials)
        for (const auto texture : material.textures)
            if (texture != no_texture && texture != placeholder_texture && texture >= snapshot.textures.size())
                return {RhiError::invalid_usage, "Render snapshot material refers to a texture it does not hold"};
    // Lit pipelines are created when a material first needs them; a debug view drawn by the lit pass uses
    // their debug variants instead.
    const auto debugging = lit_debug_view(view.debug_view);
    const auto lit_kind = [](const RenderMaterial& material) {
        if (material.alpha_mode == AlphaMode::blend)
            return material.double_sided ? PipelineKind::lit_blend_double_sided : PipelineKind::lit_blend;
        return material.double_sided ? PipelineKind::lit_double_sided : PipelineKind::lit;
    };
    // By skinning (docs/animation.md#skinning), then lit, double-sided, blend, blend double-sided.
    auto lit = std::array<std::array<PipelineHandle, 4>, 2>{};
    const auto lit_index = [](PipelineKind kind) {
        return kind == PipelineKind::lit ? 0 : kind == PipelineKind::lit_double_sided ? 1 : kind == PipelineKind::lit_blend ? 2 : 3;
    };
    // Which meshes are skinned, once each rather than per instance.
    m_skinned_meshes.assign(snapshot.meshes.size(), false);
    auto skinning = false;
    for (size_t m = 0; m < snapshot.meshes.size(); ++m) {
        const auto skinned = snapshot.meshes[m].value().mesh().skinned();
        m_skinned_meshes[m] = skinned;
        skinning |= skinned;
    }
    for (const auto& instance : snapshot.instances) {
        const auto kind = lit_kind(snapshot.materials[instance.material]);
        const auto skinned = bool(m_skinned_meshes[instance.mesh]);
        if (auto& slot = lit[skinned][lit_index(kind)]; !slot.valid())
            if (auto error = pipeline(target.scene_format(), kind, slot, debugging, skinned)) return error;
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
    const auto sky = environment && environment->background && !material_debug_view(view.debug_view); // inputs, not light
    auto sky_pipeline = PipelineHandle{};
    if (sky)
        if (auto error = pipeline(target.scene_format(), PipelineKind::sky, sky_pipeline)) return error;
    auto debug_front = PipelineHandle{}, debug_behind = PipelineHandle{};
    if (!snapshot.debug.empty()) { // no debug pipelines until something is drawn with them
        if (auto error = pipeline(target.color_format(), PipelineKind::debug_front, debug_front)) return error;
        if (auto error = pipeline(target.color_format(), PipelineKind::debug_behind, debug_behind)) return error;
    }

    // Lights and shadows for this view (docs/renderer.md#shadows): the shadow maps are rendered first, from
    // every instance's constants, uploaded once for every pass.
    const auto plan = plan_lights(snapshot, view);
    m_lights = {plan.local.size(), {}, {}, plan.sun ? std::optional(snapshot.lights[*plan.sun].entity) : std::nullopt};
    for (const auto index : plan.dropped) m_lights.dropped.push_back(snapshot.local_lights[index].entity);
    for (const auto index : plan.unshadowed) m_lights.unshadowed.push_back(snapshot.local_lights[index].entity);
    if (auto error = prepare_shadows(plan)) return error;
    // What every pass draws (docs/renderer.md#culling-and-batching): the view's instances inside its frustum
    // and each shadow map's casters, grouped into instanced draws of one mesh and material. Every
    // instance's transform and every pass's order are uploaded once, and read by all passes.
    m_draws.clear();
    const auto view_batches = plan_view_batches(snapshot, view, m_draws, m_batch_scratch);
    m_view_report = {view_batches.drawn, view_batches.culled, view_batches.opaque.size() + view_batches.blended.size()};
    auto sun_maps = std::vector<math::Mat4>{}, spot_maps = std::vector<math::Mat4>{};
    auto sun_batches = std::vector<std::vector<DrawBatch>>{}, spot_batches = std::vector<std::vector<DrawBatch>>{};
    if (plan.sun)
        for (const auto& cascade : plan.cascades) {
            sun_maps.push_back(cascade.view_projection);
            sun_batches.push_back(plan_shadow_batches(snapshot, [&](const RenderInstance& instance) { return casts_into(cascade, instance); },
                                                      m_draws, m_batch_scratch));
        }
    for (const auto& spot : plan.spot_shadows) {
        spot_maps.push_back(spot.view_projection);
        const auto& light = snapshot.local_lights[spot.light];
        spot_batches.push_back(plan_shadow_batches(snapshot, [&](const RenderInstance& instance) { return casts_into(light, instance); },
                                                   m_draws, m_batch_scratch));
    }
    m_instances = {};
    m_order = {};
    if (!m_draws.order.empty()) {
        m_instance_data.resize(snapshot.instances.size());
        for (size_t i = 0; i < snapshot.instances.size(); ++i) {
            const auto& instance = snapshot.instances[i];
            auto& draw = m_instance_data[i];
            draw.model = instance.world;
            for (size_t c = 0; c < 3; ++c) draw.normal_matrix[c] = {instance.normal_matrix[c], 0.0f};
            draw.skin[0] = instance.first_joint;
            draw.skin[1] = instance.joint_count;
        }
        const auto instances = m_device.upload_transient(m_instance_data.data(), m_instance_data.size() * sizeof(DrawConstants));
        if (!instances) return instances.diagnostic;
        const auto order = m_device.upload_transient(m_draws.order.data(), m_draws.order.size() * sizeof(uint32_t));
        if (!order) return order.diagnostic;
        m_instances = instances.slice;
        m_order = order.slice;
    }
    // Skinned meshes' joints, uploaded once and read by every pass; one unused matrix when none bound.
    m_palette = {};
    if (skinning && !m_draws.order.empty()) {
        static const auto unused = math::Mat4::identity();
        const auto* data = snapshot.joints.empty() ? &unused : snapshot.joints.data();
        const auto palette = m_device.upload_transient(data, std::max<size_t>(snapshot.joints.size(), 1) * sizeof(math::Mat4));
        if (!palette) return palette.diagnostic;
        m_palette = palette.slice;
        m_stats.joints += snapshot.joints.size();
    }
    if (plan.sun)
        if (auto error = encode_shadow_atlas(*m_sun_atlas, snapshot, "sun shadows", sun_maps, sun_batches)) return error;
    if (!plan.spot_shadows.empty())
        if (auto error = encode_shadow_atlas(*m_spot_atlas, snapshot, "spot shadows", spot_maps, spot_batches)) return error;

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
    constants.local_count[0] = uint32_t(plan.local.size());
    for (size_t k = 0; k < plan.local.size(); ++k) {
        const auto& light = snapshot.local_lights[plan.local[k]];
        const auto spot = light.kind == LightKind::spot;
        const auto map = std::ranges::find(plan.spot_shadows, plan.local[k], &SpotShadow::light);
        const auto shadow_map = map == plan.spot_shadows.end() ? 0.0f : float(map - plan.spot_shadows.begin() + 1);
        constants.local_lights[k] = {{light.position, light.range}, {light.direction, spot ? 1.0f : 0.0f}, {light.intensity, shadow_map},
                                     {light.cos_outer, 1.0f / std::max(light.cos_inner - light.cos_outer, 1e-4f), light.shadow.bias,
                                      light.shadow.normal_bias}};
    }
    auto& shadows = constants.shadows;
    if (plan.sun) {
        const auto& sun = snapshot.lights[*plan.sun];
        for (uint32_t c = 0; c < sun_cascades; ++c) {
            shadows.cascades[c] = plan.cascades[c].view_projection;
            (&shadows.cascade_far.x)[c] = plan.cascades[c].far;
            (&shadows.cascade_texel.x)[c] = plan.cascades[c].texel;
        }
        shadows.sun = {sun.shadow.bias, sun.shadow.normal_bias, plan.cascades[0].near, float(*plan.sun + 1)};
    }
    for (size_t m = 0; m < plan.spot_shadows.size(); ++m) {
        shadows.spots[m] = plan.spot_shadows[m].view_projection;
        (&shadows.spot_texel.x)[m] = plan.spot_shadows[m].texel_per_metre;
    }
    shadows.view_forward = {plan.view_forward, float(view.debug_view)};
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
        if (auto error = m_device.set_texture(8, (plan.sun ? *m_sun_atlas : *m_no_shadows).handle())) return error;
        if (auto error = m_device.set_texture(9, (plan.spot_shadows.empty() ? *m_no_shadows : *m_spot_atlas).handle())) return error;
        if (auto error = m_device.set_sampler(8, m_shadow_sampler->handle())) return error;
        // The view's constants, instances, and order stay bound for the whole pass.
        if (auto error = m_device.set_uniform_buffer(2, uploaded_view.slice)) return error;
        if (m_order.size > 0)
            if (auto error = bind_instances()) return error;
        auto sky_drawn = !sky;
        const auto draw_sky = [&]() -> RhiDiagnostic { // after opaque surfaces, before blended ones
            sky_drawn = true;
            if (auto error = m_device.set_pipeline(sky_pipeline)) return error;
            bound_pipeline = sky_pipeline;
            if (auto error = m_device.set_uniform_buffer(2, uploaded_view.slice)) return error;
            if (auto error = m_device.set_texture(6, environment->asset.value().background().handle())) return error;
            return m_device.draw(3);
        };
        const auto draw = [&](const DrawBatch& batch) -> RhiDiagnostic {
            const auto& material = snapshot.materials[batch.material];
            const auto skinned = snapshot.meshes[batch.mesh].value().mesh().skinned();
            if (const auto pipeline = lit[skinned][lit_index(lit_kind(material))]; pipeline != bound_pipeline) {
                if (auto error = m_device.set_pipeline(pipeline)) return error;
                bound_pipeline = pipeline;
            }
            const auto constants = material_constants(material);
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
            if (auto error = snapshot.meshes[batch.mesh].value().mesh().draw(batch.count, batch.first)) return error;
            ++m_stats.draws;
            m_stats.instances += batch.count;
            return {};
        };
        // Opaque and masked surfaces, the sky where none was drawn, then blended surfaces back to front.
        for (const auto& batch : view_batches.opaque)
            if (auto error = draw(batch)) return error;
        if (!sky_drawn)
            if (auto error = draw_sky()) return error;
        for (const auto& batch : view_batches.blended)
            if (auto error = draw(batch)) return error;
        m_stats.culled += view_batches.culled;
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
        // 1 luminance, 2 false color, 3 material inputs shown as they are; light otherwise.
        const auto mode = view.debug_view == DebugView::luminance ? 1u : view.debug_view == DebugView::false_color ? 2u
                          : material_debug_view(view.debug_view) ? 3u : 0u;
        const auto constants = ToneMapConstants{view.exposure, uint32_t(view.tone_mapping), mode, 0};
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
        bool xray = false; // as bright behind the scene as in front
    };
    auto batches = std::array<Batch, 9>{}; // lines, x-ray lines, boxes, and spheres and capsules at three levels of detail
    auto used = size_t{0};
    const auto upload = [&](uint32_t kind, uint32_t segments, uint32_t count, uint32_t vertices, bool xray = false) -> RhiDiagnostic {
        const auto uploaded = m_device.upload_transient(m_debug_data.data(), m_debug_data.size() * sizeof(float), 16);
        if (!uploaded) return uploaded.diagnostic;
        batches[used++] = {kind, segments, count, vertices, uploaded.slice, xray};
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
    for (const auto* lines : {&debug.lines, &debug.xray_lines}) {
        if (lines->empty()) continue;
        m_debug_data.clear();
        m_debug_data.reserve(lines->size() * debug_line_floats);
        for (const auto& line : *lines) {
            push(line.from, 1.0f);
            push(line.to, 1.0f);
            push_color(line.color);
        }
        if (auto error = upload(0, 0, 1, uint32_t(lines->size()) * 6, lines == &debug.xray_lines)) return error;
        m_stats.debug_lines += lines->size();
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
            constants.viewport = {float(view.width), float(view.height), view.debug_line_width, batch.xray ? 1.0f : opacity};
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
