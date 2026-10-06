#include "maya/renderer/render_snapshot.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace maya {
namespace {
std::string id_text(uint64_t high, uint64_t low) {
    auto text = std::ostringstream{};
    text << std::hex << high << ' ' << low;
    return text.str();
}
template<class Tag> std::string id_text(PersistentId<Tag> id) { return id_text(id.high, id.low); }

/// Inverse transpose of the linear part, from the cofactor matrix in double precision.
/// Rejects the same angular degeneracy as inverse_affine.
std::optional<std::array<math::Vec3, 3>> normal_matrix(const math::Mat4& m) noexcept {
    double a[3][3];
    double lengths[3];
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) a[r][c] = m.at(r, c);
        lengths[c] = std::hypot(a[0][c], a[1][c], a[2][c]);
    }
    double cofactor[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            cofactor[r][c] = a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1];
        }
    const auto determinant = a[0][0] * cofactor[0][0] + a[0][1] * cofactor[0][1] + a[0][2] * cofactor[0][2];
    // Positive-scale TRS cannot reflect, so a nonpositive determinant means degenerate data.
    if (!std::isfinite(determinant) ||
        determinant <= spatial_singularity_tolerance * lengths[0] * lengths[1] * lengths[2]) return std::nullopt;
    // (A^-1)^T = cofactor / det. One positive factor for the whole matrix keeps directions intact
    // while keeping the entries representable; the shader normalizes the result.
    auto scale = 0.0;
    for (const auto& row : cofactor) for (const auto value : row) scale = std::max(scale, std::abs(value));
    if (!std::isfinite(scale) || scale == 0) return std::nullopt;
    auto result = std::array<math::Vec3, 3>{};
    for (int c = 0; c < 3; ++c)
        result[c] = {float(cofactor[0][c] / scale), float(cofactor[1][c] / scale), float(cofactor[2][c] / scale)};
    return result;
}

class Extraction {
public:
    Extraction(AssetRegistry& assets, RenderSnapshot& out) : m_assets(assets), m_out(out) {}

    void report(RenderIssue code, EntityId entity, AssetId asset, std::string message) {
        if (m_out.diagnostics.size() < max_render_diagnostics)
            m_out.diagnostics.push_back({code, entity, asset, std::move(message)});
    }
    std::optional<uint32_t> mesh(AssetRef<MeshAsset> ref, EntityId entity) {
        if (const auto found = m_meshes.find(ref.id); found != m_meshes.end()) {
            if (!found->second) report(RenderIssue::missing_mesh, entity, ref.id,
                "Entity " + id_text(entity) + " skipped: mesh " + id_text(ref.id) + " is unavailable");
            return found->second;
        }
        auto acquired = m_assets.acquire(ref);
        auto index = std::optional<uint32_t>{};
        if (acquired && acquired.lease.value().mesh().valid()) {
            index = static_cast<uint32_t>(m_out.meshes.size());
            m_out.meshes.push_back(std::move(acquired.lease));
        } else {
            const auto why = acquired.diagnostic ? acquired.diagnostic.message : "its GPU buffers are gone";
            report(RenderIssue::missing_mesh, entity, ref.id,
                "Entity " + id_text(entity) + " skipped: mesh " + id_text(ref.id) + " is unavailable: " + why);
        }
        m_meshes.emplace(ref.id, index);
        return index;
    }
    /// A texture's index in the snapshot, or placeholder_texture when it cannot be drawn in `slot`.
    /// Problems are reported once per material, with the first entity that uses it.
    uint32_t texture(AssetRef<TextureAsset> ref, MaterialSlot slot, AssetId material, EntityId entity) {
        if (!ref.valid()) return no_texture;
        constexpr const char* slot_names[] = {"base color map", "metallic-roughness map", "normal map", "occlusion map", "emissive map"};
        const auto name = slot_names[size_t(slot)];
        auto found = m_textures.find(ref.id);
        if (found == m_textures.end()) {
            auto acquired = m_assets.acquire(ref);
            auto entry = TextureEntry{};
            if (acquired && acquired.lease.value().valid()) {
                entry.index = uint32_t(m_out.textures.size());
                entry.role = acquired.lease.value().role();
                m_out.textures.push_back(std::move(acquired.lease));
            } else {
                entry.problem = acquired.diagnostic ? acquired.diagnostic.message : "its GPU texture is gone";
            }
            found = m_textures.emplace(ref.id, std::move(entry)).first;
        }
        const auto& entry = found->second;
        if (!entry.index) {
            report(RenderIssue::missing_texture, entity, ref.id, "Material " + id_text(material) + "'s " + name + " " +
                id_text(ref.id) + " is unavailable, so the placeholder is drawn: " + entry.problem);
            return placeholder_texture;
        }
        // glTF's roles: color maps are sRGB; normal maps hold normals; the others hold linear data.
        const auto expected = slot == MaterialSlot::base_color || slot == MaterialSlot::emissive ? TextureRole::color
                            : slot == MaterialSlot::normal ? TextureRole::normal : TextureRole::data;
        if (entry.role != expected) {
            report(RenderIssue::texture_role, entity, ref.id, "Material " + id_text(material) + "'s " + name + " " + id_text(ref.id) +
                " is a " + texture_role_name(entry.role) + " texture, but the slot needs " + texture_role_name(expected) +
                ", so the placeholder is drawn; change the texture's usage");
            return placeholder_texture;
        }
        return *entry.index;
    }
    RenderMaterial copy(const MaterialAsset& value, AssetId id, EntityId entity) {
        auto material = RenderMaterial{};
        material.base_color = value.base_color;
        material.metallic = value.metallic;
        material.roughness = value.roughness;
        material.normal_scale = value.normal_scale;
        material.occlusion_strength = value.occlusion_strength;
        material.emissive = value.emissive * value.emissive_strength;
        material.alpha_mode = value.alpha_mode;
        material.alpha_cutoff = value.alpha_cutoff;
        material.double_sided = value.double_sided;
        material.uv_offset = value.uv_offset;
        material.uv_rotation = value.uv_rotation;
        material.uv_scale = value.uv_scale;
        const auto slot = [&](MaterialSlot which, AssetRef<TextureAsset> ref) {
            material.textures[size_t(which)] = texture(ref, which, id, entity);
        };
        slot(MaterialSlot::base_color, value.base_color_texture);
        slot(MaterialSlot::metallic_roughness, value.metallic_roughness_texture);
        slot(MaterialSlot::normal, value.normal_texture);
        slot(MaterialSlot::occlusion, value.occlusion_texture);
        slot(MaterialSlot::emissive, value.emissive_texture);
        return material;
    }
    RenderMaterial material(AssetRef<MaterialAsset> ref, EntityId entity) {
        if (!ref.valid()) return copy(MaterialAsset{}, {}, entity);
        if (const auto found = m_materials.find(ref.id); found != m_materials.end()) {
            if (!found->second) report(RenderIssue::missing_material, entity, ref.id,
                "Entity " + id_text(entity) + " uses the fallback material: " + id_text(ref.id) + " is unavailable");
            return found->second ? *found->second : copy(fallback_material(), {}, entity);
        }
        const auto acquired = m_assets.acquire(ref);
        auto value = std::optional<RenderMaterial>{};
        if (acquired) value = copy(acquired.lease.value(), ref.id, entity);
        else report(RenderIssue::missing_material, entity, ref.id, "Entity " + id_text(entity) +
            " uses the fallback material: " + id_text(ref.id) + " is unavailable: " + acquired.diagnostic.message);
        m_materials.emplace(ref.id, value);
        return value ? *value : copy(fallback_material(), {}, entity);
    }

private:
    AssetRegistry& m_assets;
    RenderSnapshot& m_out;
    // Each asset is acquired once per extraction, so every instance draws the same version.
    std::unordered_map<AssetId, std::optional<uint32_t>, PersistentIdHash> m_meshes;
    std::unordered_map<AssetId, std::optional<RenderMaterial>, PersistentIdHash> m_materials;
    struct TextureEntry {
        std::optional<uint32_t> index; // into the snapshot's textures, when it could be acquired
        TextureRole role = TextureRole::color;
        std::string problem; // why it could not
    };
    std::unordered_map<AssetId, TextureEntry, PersistentIdHash> m_textures;
};
} // namespace

RenderSnapshot extract_render_snapshot(const World& world, AssetRegistry& assets,
                                       const RenderExtractOptions& options) {
    auto snapshot = RenderSnapshot{};
    snapshot.world = world.token();
    snapshot.ambient = options.ambient;
    auto extraction = Extraction(assets, snapshot);
    const auto world_matrix = [&](EntityHandle entity) {
        return options.poses ? options.poses->world_matrix(world, entity) : world.world_matrix(entity);
    };

    world.for_each<MeshRendererComponent>([&](EntityHandle entity, const MeshRendererComponent& renderer) {
        ++snapshot.stats.mesh_renderers;
        if (!renderer.visible || !renderer.mesh.valid()) {
            ++snapshot.stats.hidden;
            return;
        }
        const auto id = *world.persistent_id(entity);
        const auto skip = [&](RenderIssue code, std::string message) {
            ++snapshot.stats.skipped;
            extraction.report(code, id, {}, std::move(message));
        };
        if (!world.has<TransformComponent>(entity))
            return skip(RenderIssue::missing_transform, "Entity " + id_text(id) + " skipped: a mesh renderer needs a transform");
        const auto matrix = world_matrix(entity);
        const auto normals = matrix ? normal_matrix(*matrix) : std::nullopt;
        if (!normals)
            return skip(RenderIssue::invalid_transform, "Entity " + id_text(id) + " skipped: its world transform is degenerate or unrepresentable");
        const auto mesh = extraction.mesh(renderer.mesh, id);
        if (!mesh) {
            ++snapshot.stats.skipped;
            return;
        }
        auto instance = RenderInstance{id, *mesh, *matrix, *normals, extraction.material(renderer.material, id)};
        // A sphere around the mesh's local bounds, carried into the world (radius times the largest scale).
        if (const auto& geometry = snapshot.meshes[*mesh].value().geometry(); !geometry.empty()) {
            const auto local = (geometry.min + geometry.max) * 0.5f;
            const auto& m = *matrix;
            instance.bounds_center = {m.at(0, 0) * local.x + m.at(0, 1) * local.y + m.at(0, 2) * local.z + m.at(0, 3),
                                      m.at(1, 0) * local.x + m.at(1, 1) * local.y + m.at(1, 2) * local.z + m.at(1, 3),
                                      m.at(2, 0) * local.x + m.at(2, 1) * local.y + m.at(2, 2) * local.z + m.at(2, 3)};
            const auto column = [&](int c) { return math::Vec3{m.at(0, c), m.at(1, c), m.at(2, c)}.length(); };
            instance.bounds_radius = (geometry.max - geometry.min).length() * 0.5f * std::max({column(0), column(1), column(2)});
        }
        snapshot.instances.push_back(std::move(instance));
    });

    world.for_each<LightComponent>([&](EntityHandle entity, const LightComponent& light) {
        if (!light.enabled) return;
        const auto id = *world.persistent_id(entity);
        const auto matrix = world_matrix(entity);
        if (!matrix)
            return extraction.report(RenderIssue::missing_transform, id, {},
                "Light " + id_text(id) + " ignored: a light needs a valid transform");
        const auto z = math::Vec3{matrix->at(0, 2), matrix->at(1, 2), matrix->at(2, 2)}.normalized();
        const auto shadow = RenderShadow{light.cast_shadows, light.shadow_bias, light.shadow_normal_bias};
        if (light.kind == LightKind::directional) {
            snapshot.lights.push_back({id, z, light.color * light.intensity, shadow, light.shadow_distance});
            return;
        }
        auto local = RenderLocalLight{id, light.kind, {matrix->at(0, 3), matrix->at(1, 3), matrix->at(2, 3)}, -z,
                                      light.color * light.intensity, light.range};
        if (light.kind == LightKind::spot) {
            local.cos_inner = std::cos(light.inner_cone * 0.5f);
            local.cos_outer = std::cos(light.outer_cone * 0.5f);
            local.shadow = shadow;
        }
        snapshot.local_lights.push_back(local);
    });
    // Deterministic selection when there are more lights than the shader supports.
    std::ranges::sort(snapshot.lights, {}, &RenderDirectionalLight::entity);
    std::ranges::sort(snapshot.local_lights, {}, &RenderLocalLight::entity);
    if (snapshot.local_lights.size() > max_extracted_local_lights) {
        for (auto it = snapshot.local_lights.begin() + max_extracted_local_lights; it != snapshot.local_lights.end(); ++it)
            extraction.report(RenderIssue::light_limit, it->entity, {}, "Light " + id_text(it->entity) + " ignored: at most " +
                std::to_string(max_extracted_local_lights) + " point and spot lights are extracted");
        snapshot.local_lights.resize(max_extracted_local_lights);
    }
    if (snapshot.lights.size() > max_directional_lights) {
        for (auto it = snapshot.lights.begin() + max_directional_lights; it != snapshot.lights.end(); ++it)
            extraction.report(RenderIssue::light_limit, it->entity, {}, "Light " + id_text(it->entity) +
                " ignored: at most " + std::to_string(max_directional_lights) + " directional lights are rendered");
        snapshot.lights.resize(max_directional_lights);
    }
    // The sun: the first directional light that casts shadows gets the cascades; the rest are drawn
    // without shadows.
    auto shadowed = false;
    for (auto& light : snapshot.lights) {
        if (!light.shadow.cast) continue;
        if (shadowed) {
            light.shadow.cast = false;
            extraction.report(RenderIssue::shadow_limit, light.entity, {}, "Light " + id_text(light.entity) +
                " is drawn without shadows: only one directional light casts them");
        }
        shadowed = true;
    }
    // At most one environment: the lowest EntityId's. An unassigned one lights nothing and says nothing.
    auto environments = std::vector<std::pair<EntityId, EnvironmentComponent>>{};
    world.for_each<EnvironmentComponent>([&](EntityHandle entity, const EnvironmentComponent& environment) {
        environments.emplace_back(*world.persistent_id(entity), environment);
    });
    std::ranges::sort(environments, {}, &std::pair<EntityId, EnvironmentComponent>::first);
    for (size_t i = 1; i < environments.size(); ++i)
        extraction.report(RenderIssue::environment_limit, environments[i].first, {}, "Environment " + id_text(environments[i].first) +
            " ignored: a scene uses one environment, " + id_text(environments.front().first) + "'s");
    if (!environments.empty() && environments.front().second.environment.valid()) {
        const auto& [id, component] = environments.front();
        if (auto acquired = assets.acquire(component.environment); acquired && acquired.lease.value().valid())
            snapshot.environment = RenderEnvironment{id, std::move(acquired.lease), component.intensity, component.rotation, component.background};
        else
            extraction.report(RenderIssue::missing_environment, id, component.environment.id, "Environment " +
                id_text(component.environment.id) + " is unavailable, so the ambient light is used: " +
                (acquired.diagnostic ? acquired.diagnostic.message : std::string("its GPU textures are gone")));
    }
    if (options.debug) snapshot.debug = *options.debug;
    return snapshot;
}

float exposure_scale(float ev100) noexcept { return 1.0f / (1.2f * std::exp2(ev100)); }

namespace {
constexpr std::array<std::string_view, 15> debug_view_names = {
    "none", "luminance", "false-color", "base-color", "normals", "shading-normals", "metallic", "roughness", "occlusion",
    "emissive", "direct-light", "environment-light", "lighting", "cascades", "texels"};
} // namespace
std::string_view debug_view_name(DebugView view) noexcept {
    return size_t(view) < debug_view_names.size() ? debug_view_names[size_t(view)] : std::string_view{};
}
std::optional<DebugView> debug_view_named(std::string_view name) noexcept {
    const auto found = std::ranges::find(debug_view_names, name);
    if (found == debug_view_names.end()) return std::nullopt;
    return DebugView(found - debug_view_names.begin());
}

std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Mat4& pose,
                                           uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return std::nullopt;
    const auto matrices = camera_matrices(camera, pose, float(width) / float(height));
    if (!matrices) return std::nullopt;
    auto view = RenderView{width, height, *matrices, {pose.at(0, 3), pose.at(1, 3), pose.at(2, 3)}};
    view.exposure = exposure_scale(camera.exposure);
    view.tone_mapping = camera.tone_mapping;
    return view;
}

std::optional<RenderView> extract_render_view(const World& world, EntityHandle camera,
                                              uint32_t width, uint32_t height, const PresentationPoses* poses) {
    if (width == 0 || height == 0) return std::nullopt;
    if (const auto pose = poses ? poses->find(camera) : std::nullopt) {
        auto component = std::optional<CameraComponent>{};
        world.with<CameraComponent>(camera, [&](const CameraComponent& value) { component = value; });
        return component ? make_render_view(*component, *pose, width, height) : std::nullopt;
    }
    const auto matrices = world.camera(camera, float(width) / float(height));
    if (!matrices) return std::nullopt;
    const auto pose = *world.world_matrix(camera);
    auto view = RenderView{width, height, *matrices, {pose.at(0, 3), pose.at(1, 3), pose.at(2, 3)}};
    world.with<CameraComponent>(camera, [&](const CameraComponent& value) {
        view.exposure = exposure_scale(value.exposure);
        view.tone_mapping = value.tone_mapping;
    });
    return view;
}
} // namespace maya
