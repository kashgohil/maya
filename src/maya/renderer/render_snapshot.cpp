#include "maya/renderer/render_snapshot.hpp"
#include "maya/assets/animation.hpp"
#include "maya/world/name_path.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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
    Extraction(AssetRegistry& assets, RenderSnapshot& out, bool stream) : m_assets(assets), m_out(out), m_stream(stream) {}

    /// Streaming, the resident version or a request for it; otherwise, the asset loaded now.
    template<Asset T> AssetResult<T> get(AssetRef<T> ref) { return m_stream ? m_assets.try_acquire(ref) : m_assets.acquire(ref); }
    static bool loading(const AssetDiagnostic& diagnostic) noexcept { return diagnostic.code == AssetError::loading; }
    bool pending(AssetId id) const { return m_pending.contains(id); }
    /// Streaming, starts the loads an instance waiting for its mesh will need next (its material, the
    /// material's maps, its skin), so they load together rather than one after another.
    void prefetch(AssetRef<MaterialAsset> material, AssetRef<SkinAsset> skin) {
        if (!m_stream) return;
        if (material.valid() && m_prefetched.insert(material.id).second)
            if (const auto acquired = m_assets.try_acquire(material)) {
                const auto& value = acquired.lease.value();
                for (const auto texture : {value.base_color_texture, value.metallic_roughness_texture, value.normal_texture,
                                           value.occlusion_texture, value.emissive_texture})
                    if (texture.valid()) (void)m_assets.try_acquire(texture);
            }
        if (skin.valid() && m_prefetched.insert(skin.id).second) (void)m_assets.try_acquire(skin);
    }

    void report(RenderIssue code, EntityId entity, AssetId asset, std::string message) {
        if (m_out.diagnostics.size() < max_render_diagnostics)
            m_out.diagnostics.push_back({code, entity, asset, std::move(message)});
    }
    std::optional<uint32_t> mesh(AssetRef<MeshAsset> ref, EntityId entity) {
        if (const auto found = m_meshes.find(ref.id); found != m_meshes.end()) {
            if (!found->second && !pending(ref.id)) report(RenderIssue::missing_mesh, entity, ref.id,
                "Entity " + id_text(entity) + " skipped: mesh " + id_text(ref.id) + " is unavailable");
            return found->second;
        }
        auto acquired = get(ref);
        auto index = std::optional<uint32_t>{};
        if (acquired && acquired.lease.value().mesh().valid()) {
            index = static_cast<uint32_t>(m_out.meshes.size());
            m_out.meshes.push_back(std::move(acquired.lease));
        } else if (loading(acquired.diagnostic)) {
            m_pending.insert(ref.id);
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
            auto acquired = get(ref);
            auto entry = TextureEntry{};
            if (loading(acquired.diagnostic)) {
                entry.pending = true;
                ++m_out.stats.pending_textures;
            } else if (acquired && acquired.lease.value().valid()) {
                entry.index = uint32_t(m_out.textures.size());
                entry.role = acquired.lease.value().role();
                m_out.textures.push_back(std::move(acquired.lease));
            } else {
                entry.problem = acquired.diagnostic ? acquired.diagnostic.message : "its GPU texture is gone";
            }
            found = m_textures.emplace(ref.id, std::move(entry)).first;
        }
        const auto& entry = found->second;
        if (entry.pending) return no_texture; // the factor alone until the texture is resident
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
    /// The material's index in the snapshot's materials, each copied once (#1025); nullopt while it loads.
    std::optional<uint32_t> material(AssetRef<MaterialAsset> ref, EntityId entity) {
        if (!ref.valid()) return shared(m_default, MaterialAsset{}, entity);
        if (pending(ref.id)) return std::nullopt;
        if (const auto found = m_materials.find(ref.id); found != m_materials.end()) {
            if (!found->second) report(RenderIssue::missing_material, entity, ref.id,
                "Entity " + id_text(entity) + " uses the fallback material: " + id_text(ref.id) + " is unavailable");
            return found->second ? *found->second : shared(m_fallback, fallback_material(), entity);
        }
        const auto acquired = get(ref);
        if (loading(acquired.diagnostic)) {
            m_pending.insert(ref.id);
            return std::nullopt;
        }
        auto value = std::optional<uint32_t>{};
        if (acquired) value = add(copy(acquired.lease.value(), ref.id, entity));
        else report(RenderIssue::missing_material, entity, ref.id, "Entity " + id_text(entity) +
            " uses the fallback material: " + id_text(ref.id) + " is unavailable: " + acquired.diagnostic.message);
        m_materials.emplace(ref.id, value);
        return value ? *value : shared(m_fallback, fallback_material(), entity);
    }

    /// A skin, acquired once per extraction; null, with the reason, when it cannot be.
    const AssetLease<SkinAsset>* skin(AssetRef<SkinAsset> ref, std::string& problem) {
        auto found = m_skins.find(ref.id);
        if (found == m_skins.end()) {
            auto acquired = get(ref);
            auto entry = SkinEntry{};
            if (loading(acquired.diagnostic)) m_pending.insert(ref.id);
            if (acquired) entry.lease = std::move(acquired.lease);
            else entry.problem = acquired.diagnostic.message;
            found = m_skins.emplace(ref.id, std::move(entry)).first;
        }
        if (!found->second.lease) {
            problem = found->second.problem;
            return nullptr;
        }
        return &found->second.lease;
    }

private:
    uint32_t add(const RenderMaterial& material) {
        m_out.materials.push_back(material);
        return uint32_t(m_out.materials.size() - 1);
    }
    /// The default or fallback material, added the first time an instance needs it.
    uint32_t shared(std::optional<uint32_t>& index, const MaterialAsset& value, EntityId entity) {
        if (!index) index = add(copy(value, {}, entity));
        return *index;
    }

    AssetRegistry& m_assets;
    RenderSnapshot& m_out;
    bool m_stream;
    std::unordered_set<AssetId, PersistentIdHash> m_pending; // meshes, materials, and skins still loading
    std::unordered_set<AssetId, PersistentIdHash> m_prefetched;
    std::optional<uint32_t> m_default, m_fallback;
    // Each asset is acquired once per extraction, so every instance draws the same version.
    std::unordered_map<AssetId, std::optional<uint32_t>, PersistentIdHash> m_meshes;
    std::unordered_map<AssetId, std::optional<uint32_t>, PersistentIdHash> m_materials; // index into the snapshot's
    struct TextureEntry {
        std::optional<uint32_t> index; // into the snapshot's textures, when it could be acquired
        bool pending = false; // still loading: drawn as the factor alone
        TextureRole role = TextureRole::color;
        std::string problem; // why it could not
    };
    std::unordered_map<AssetId, TextureEntry, PersistentIdHash> m_textures;
    struct SkinEntry {
        AssetLease<SkinAsset> lease;
        std::string problem;
    };
    std::unordered_map<AssetId, SkinEntry, PersistentIdHash> m_skins;
};

/// Finds skins' joints below entities' ancestors, through a cache kept between extractions while the
/// World's names and hierarchy stay the same (SkinBindingCache), or one of its own for this extraction.
class SkinResolver {
public:
    SkinResolver(const World& world, SkinBindingCache* cache) : m_world(world), m_cache(cache ? *cache : m_own) {
        if (m_cache.world != world.token() || m_cache.names_revision != world.names_revision()) {
            m_cache.bindings.clear();
            m_cache.world = world.token();
            m_cache.names_revision = world.names_revision();
        }
    }
    /// The joints of `skin` below the nearest of `entity` and its ancestors that has them all, or why not:
    /// the first joint the farthest ancestor lacks.
    const SkinBindingCache::Binding& resolve(EntityHandle entity, const AssetLease<SkinAsset>& skin) {
        const auto key = std::tuple(entity, skin.handle());
        if (const auto found = m_cache.bindings.find(key); found != m_cache.bindings.end()) return found->second;
        auto binding = SkinBindingCache::Binding{};
        const auto& joints = skin.value().joints;
        for (auto at = std::optional<EntityHandle>{entity}; at; at = m_world.parent(*at)) {
            const auto found = resolve_name_paths(m_world, *at, joints);
            if (const auto gap = std::ranges::find_if(found, [](const auto& joint) { return !joint; }); gap != found.end()) {
                binding.missing = "'" + joints[size_t(gap - found.begin())] + "' is not below it or its ancestors (renamed or removed?)";
                continue;
            }
            binding.root = *at;
            for (const auto& joint : found) binding.joints.push_back(*joint);
            binding.missing.clear();
            break;
        }
        return m_cache.bindings.emplace(key, std::move(binding)).first->second;
    }

private:
    const World& m_world;
    SkinBindingCache m_own;
    SkinBindingCache& m_cache;
};

/// Skin matrices in the snapshot's joints: once per skin and the ancestor its joints are below, which an
/// imported mesh's primitives share.
class SkinPalettes {
public:
    SkinPalettes(SkinResolver& resolver, RenderSnapshot& out, std::function<std::optional<math::Mat4>(EntityHandle)> world_matrix)
        : m_resolver(resolver), m_out(out), m_world_matrix(std::move(world_matrix)) {}

    /// The skin's first joint in the snapshot's joints, or nullopt with why not.
    std::optional<uint32_t> bind(EntityHandle entity, const AssetLease<SkinAsset>& lease, std::string& missing) {
        const auto& binding = m_resolver.resolve(entity, lease);
        if (!binding.root) {
            missing = binding.missing;
            return std::nullopt;
        }
        const auto key = std::tuple(*binding.root, lease.handle());
        auto found = m_palettes.find(key);
        if (found == m_palettes.end()) {
            // Each joint's shown pose times its inverse bind matrix; an unrepresentable pose unbinds the skin.
            const auto& skin = lease.value();
            const auto start = m_out.joints.size();
            auto palette = Palette{uint32_t(start), {}};
            for (size_t j = 0; j < binding.joints.size(); ++j) {
                const auto pose = m_world_matrix(binding.joints[j]);
                if (!pose) {
                    m_out.joints.resize(start);
                    palette = {std::nullopt, "'" + skin.joints[j] + "' has an unrepresentable pose"};
                    break;
                }
                m_out.joints.push_back(*pose * skin.inverse_bind[j]);
            }
            found = m_palettes.emplace(key, std::move(palette)).first;
        }
        if (!found->second.first) missing = found->second.missing;
        return found->second.first;
    }

private:
    struct Palette {
        std::optional<uint32_t> first;
        std::string missing;
    };
    SkinResolver& m_resolver;
    RenderSnapshot& m_out;
    std::function<std::optional<math::Mat4>(EntityHandle)> m_world_matrix;
    std::map<std::tuple<EntityHandle, AssetHandle<SkinAsset>>, Palette> m_palettes; // by root and skin version
};

/// A sphere around a mesh's bounds carried by each of its joints' skin matrices, which encloses every
/// skinned vertex: each is a weighted average of its joints' placements.
void skinned_bounds(RenderInstance& instance, const MeshGeometry& geometry, std::span<const math::Mat4> joints) {
    const auto local = (geometry.min + geometry.max) * 0.5f;
    const auto half = (geometry.max - geometry.min).length() * 0.5f;
    auto low = math::Vec3{std::numeric_limits<float>::infinity()}, high = math::Vec3{-std::numeric_limits<float>::infinity()};
    for (const auto& m : joints) {
        const auto center = math::Vec3{m.at(0, 0) * local.x + m.at(0, 1) * local.y + m.at(0, 2) * local.z + m.at(0, 3),
                                       m.at(1, 0) * local.x + m.at(1, 1) * local.y + m.at(1, 2) * local.z + m.at(1, 3),
                                       m.at(2, 0) * local.x + m.at(2, 1) * local.y + m.at(2, 2) * local.z + m.at(2, 3)};
        const auto column = [&](int c) { return math::Vec3{m.at(0, c), m.at(1, c), m.at(2, c)}.length(); };
        const auto radius = half * std::max({column(0), column(1), column(2)});
        low = {std::min(low.x, center.x - radius), std::min(low.y, center.y - radius), std::min(low.z, center.z - radius)};
        high = {std::max(high.x, center.x + radius), std::max(high.y, center.y + radius), std::max(high.z, center.z + radius)};
    }
    instance.bounds_center = (low + high) * 0.5f;
    instance.bounds_radius = (high - low).length() * 0.5f;
    if (!std::isfinite(instance.bounds_radius)) instance.bounds_radius = std::numeric_limits<float>::infinity();
}
} // namespace

RenderSnapshot extract_render_snapshot(const World& world, AssetRegistry& assets,
                                       const RenderExtractOptions& options) {
    auto snapshot = RenderSnapshot{};
    snapshot.world = world.token();
    snapshot.ambient = options.ambient;
    snapshot.origin = options.origin;
    auto extraction = Extraction(assets, snapshot, options.loading == AssetLoading::stream);
    // Every pose relative to the snapshot's origin (#1065), subtracted in double: what follows is in float.
    const auto world_matrix = [&](EntityHandle entity) -> std::optional<math::Mat4> {
        const auto pose = options.poses ? options.poses->world_matrix(world, entity) : world.world_matrix(entity);
        if (!pose) return std::nullopt;
        return pose->relative_to(options.origin);
    };

    auto resolver = SkinResolver(world, options.skins);
    auto skins = SkinPalettes(resolver, snapshot, world_matrix);
    // Skins by entity slot, gathered once: a scene without them pays nothing per instance.
    auto skin_of = std::vector<AssetId>{};
    world.for_each<SkinComponent>([&](EntityHandle entity, const SkinComponent& component) {
        if (entity.slot >= skin_of.size()) skin_of.resize(size_t(entity.slot) + 1);
        skin_of[entity.slot] = component.skin.id;
    });
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
            if (!extraction.pending(renderer.mesh.id)) {
                ++snapshot.stats.skipped;
                return;
            }
            ++snapshot.stats.pending;
            extraction.prefetch(renderer.material, AssetRef<SkinAsset>{entity.slot < skin_of.size() ? skin_of[entity.slot] : AssetId{}});
            return;
        }
        const auto material = extraction.material(renderer.material, id);
        if (!material) {
            ++snapshot.stats.pending;
            return;
        }
        auto instance = RenderInstance{id, *mesh, *matrix, *normals, *material};
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
        // A skinned mesh (#1038) with its skin's joints found is placed by them; otherwise it is drawn as authored.
        const auto skin = AssetRef<SkinAsset>{entity.slot < skin_of.size() ? skin_of[entity.slot] : AssetId{}};
        if (skin.valid() && snapshot.meshes[*mesh].value().mesh().skinned()) {
            auto problem = std::string{};
            const auto asset = extraction.skin(skin, problem);
            if (!asset && extraction.pending(skin.id)) {
                ++snapshot.stats.pending; // drawn once its skin is resident, never unskinned meanwhile
                return;
            }
            if (asset) {
                if (const auto first = skins.bind(entity, *asset, problem)) {
                    instance.first_joint = *first;
                    instance.joint_count = uint32_t(asset->value().joints.size());
                    if (const auto& geometry = snapshot.meshes[*mesh].value().geometry(); !geometry.empty())
                        skinned_bounds(instance, geometry, std::span(snapshot.joints).subspan(*first, instance.joint_count));
                    else
                        instance.bounds_radius = std::numeric_limits<float>::infinity();
                } else {
                    extraction.report(RenderIssue::unbound_skin, id, skin.id, "Entity " + id_text(id) + " is drawn unskinned: its skin's joint " +
                        problem);
                }
            } else {
                extraction.report(RenderIssue::unbound_skin, id, skin.id, "Entity " + id_text(id) + " is drawn unskinned: skin " +
                    id_text(skin.id) + " is unavailable: " + problem);
            }
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
        auto acquired = options.loading == AssetLoading::stream ? assets.try_acquire(component.environment) : assets.acquire(component.environment);
        if (acquired.diagnostic.code == AssetError::loading)
            snapshot.stats.environment_pending = true;
        else if (acquired && acquired.lease.value().valid())
            snapshot.environment = RenderEnvironment{id, std::move(acquired.lease), component.intensity, component.rotation, component.background};
        else
            extraction.report(RenderIssue::missing_environment, id, component.environment.id, "Environment " +
                id_text(component.environment.id) + " is unavailable, so the ambient light is used: " +
                (acquired.diagnostic ? acquired.diagnostic.message : std::string("its GPU textures are gone")));
    }
    if (options.debug) {
        snapshot.debug = *options.debug;
        snapshot.debug.rebase(snapshot.origin);
    }
    return snapshot;
}

size_t preload_render_assets(const World& world, AssetRegistry& assets) {
    const auto before = assets.residency().ready;
    const auto wait = AssetRegistry::ExplicitWait(assets);
    extract_render_snapshot(world, assets, {}); // waits for each asset it draws
    const auto after = assets.residency().ready;
    return after > before ? after - before : 0;
}

void skeleton_debug(const World& world, AssetRegistry& assets, const PresentationPoses* poses, DebugDraw& out, SkinBindingCache* cache) {
    auto resolver = SkinResolver(world, cache);
    auto drawn = std::set<std::pair<uint32_t, AssetId>>{}; // by ancestor slot and skin: once each
    world.for_each<SkinComponent>([&](EntityHandle entity, const SkinComponent& component) {
        if (!component.skin.valid()) return;
        const auto acquired = assets.try_acquire(component.skin); // a debug view: never waits
        if (!acquired) return;
        const auto& binding = resolver.resolve(entity, acquired.lease);
        if (!binding.root || !drawn.insert({binding.root->slot, component.skin.id}).second) return;
        const auto& joints = binding.joints;
        auto index = std::unordered_map<uint32_t, size_t>{}; // joints by entity slot
        auto pose = std::vector<std::optional<math::Affine>>(joints.size());
        for (size_t j = 0; j < joints.size(); ++j) {
            index.emplace(joints[j].slot, j);
            pose[j] = poses ? poses->world_matrix(world, joints[j]) : world.world_matrix(joints[j]);
        }
        const auto origin = [&](size_t j) { return pose[j]->translation; };
        auto total = 0.0f;
        auto bones = 0;
        for (size_t j = 0; j < joints.size(); ++j) {
            const auto parent = world.parent(joints[j]);
            const auto found = parent ? index.find(parent->slot) : index.end();
            if (found == index.end() || !pose[j] || !pose[found->second]) continue;
            out.xray_line(origin(found->second), origin(j), skeleton_bone_color);
            total += float((origin(j) - origin(found->second)).length());
            ++bones;
        }
        const auto axis = bones > 0 ? total / float(bones) / 3.0f : 0.0f;
        if (axis > 0.0f)
            for (size_t j = 0; j < joints.size(); ++j) {
                if (!pose[j]) continue;
                for (int c = 0; c < 3; ++c) {
                    const auto direction = pose[j]->axis(c).normalized();
                    const auto color = DebugColor{c == 0 ? 0.95f : 0.25f, c == 1 ? 0.85f : 0.25f, c == 2 ? 1.0f : 0.25f, 1.0f};
                    out.xray_line(origin(j), origin(j) + math::DVec3(direction * axis), color);
                }
            }
    });
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

std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Affine& pose,
                                           uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return std::nullopt;
    const auto matrices = camera_matrices(camera, pose, float(width) / float(height));
    if (!matrices) return std::nullopt;
    auto view = RenderView{width, height, *matrices, pose.translation};
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
    auto view = RenderView{width, height, *matrices, matrices->origin};
    world.with<CameraComponent>(camera, [&](const CameraComponent& value) {
        view.exposure = exposure_scale(value.exposure);
        view.tone_mapping = value.tone_mapping;
    });
    return view;
}

ViewFrame view_frame(const RenderSnapshot& snapshot, const RenderView& view) noexcept {
    auto frame = ViewFrame{view.matrices, (view.position - snapshot.origin).to_float()};
    frame.matrices.view = view.matrices.view * math::Mat4::translate(-frame.eye);
    frame.matrices.view_projection = view.matrices.projection * frame.matrices.view;
    frame.matrices.origin = snapshot.origin;
    return frame;
}
} // namespace maya
