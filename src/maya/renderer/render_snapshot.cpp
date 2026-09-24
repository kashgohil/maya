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
    RenderMaterial material(AssetRef<MaterialAsset> ref, EntityId entity) {
        const auto copy = [](const MaterialAsset& value) {
            return RenderMaterial{value.base_color, value.metallic, value.roughness};
        };
        if (!ref.valid()) return copy(MaterialAsset{});
        if (const auto found = m_materials.find(ref.id); found != m_materials.end()) {
            if (!found->second) report(RenderIssue::missing_material, entity, ref.id,
                "Entity " + id_text(entity) + " uses the fallback material: " + id_text(ref.id) + " is unavailable");
            return found->second.value_or(copy(fallback_material()));
        }
        const auto acquired = m_assets.acquire(ref);
        auto value = std::optional<RenderMaterial>{};
        if (acquired) value = copy(acquired.lease.value());
        else report(RenderIssue::missing_material, entity, ref.id, "Entity " + id_text(entity) +
            " uses the fallback material: " + id_text(ref.id) + " is unavailable: " + acquired.diagnostic.message);
        m_materials.emplace(ref.id, value);
        return value.value_or(copy(fallback_material()));
    }

private:
    AssetRegistry& m_assets;
    RenderSnapshot& m_out;
    // Each asset is acquired once per extraction, so every instance draws the same version.
    std::unordered_map<AssetId, std::optional<uint32_t>, PersistentIdHash> m_meshes;
    std::unordered_map<AssetId, std::optional<RenderMaterial>, PersistentIdHash> m_materials;
};
} // namespace

RenderSnapshot extract_render_snapshot(const World& world, AssetRegistry& assets,
                                       const RenderExtractOptions& options) {
    auto snapshot = RenderSnapshot{};
    snapshot.world = world.token();
    snapshot.ambient = options.ambient;
    auto extraction = Extraction(assets, snapshot);

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
        const auto matrix = world.world_matrix(entity);
        const auto normals = matrix ? normal_matrix(*matrix) : std::nullopt;
        if (!normals)
            return skip(RenderIssue::invalid_transform, "Entity " + id_text(id) + " skipped: its world transform is degenerate or unrepresentable");
        const auto mesh = extraction.mesh(renderer.mesh, id);
        if (!mesh) {
            ++snapshot.stats.skipped;
            return;
        }
        snapshot.instances.push_back({id, *mesh, *matrix, *normals, extraction.material(renderer.material, id)});
    });

    world.for_each<LightComponent>([&](EntityHandle entity, const LightComponent& light) {
        if (!light.enabled) return;
        const auto id = *world.persistent_id(entity);
        if (light.kind != LightKind::directional)
            return extraction.report(RenderIssue::unsupported_light, id, {},
                "Light " + id_text(id) + " ignored: only directional lights are rendered so far");
        const auto matrix = world.world_matrix(entity);
        if (!matrix)
            return extraction.report(RenderIssue::missing_transform, id, {},
                "Light " + id_text(id) + " ignored: a directional light needs a valid transform");
        const auto z = math::Vec3{matrix->at(0, 2), matrix->at(1, 2), matrix->at(2, 2)};
        snapshot.lights.push_back({id, z.normalized(), light.color * light.intensity});
    });
    // Deterministic selection when there are more lights than the shader supports.
    std::ranges::sort(snapshot.lights, {}, &RenderDirectionalLight::entity);
    if (snapshot.lights.size() > max_directional_lights) {
        for (auto it = snapshot.lights.begin() + max_directional_lights; it != snapshot.lights.end(); ++it)
            extraction.report(RenderIssue::light_limit, it->entity, {}, "Light " + id_text(it->entity) +
                " ignored: at most " + std::to_string(max_directional_lights) + " directional lights are rendered");
        snapshot.lights.resize(max_directional_lights);
    }
    return snapshot;
}

std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Mat4& pose,
                                           uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return std::nullopt;
    const auto matrices = camera_matrices(camera, pose, float(width) / float(height));
    if (!matrices) return std::nullopt;
    return RenderView{width, height, *matrices, {pose.at(0, 3), pose.at(1, 3), pose.at(2, 3)}};
}

std::optional<RenderView> extract_render_view(const World& world, EntityHandle camera,
                                              uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return std::nullopt;
    const auto matrices = world.camera(camera, float(width) / float(height));
    if (!matrices) return std::nullopt;
    const auto pose = *world.world_matrix(camera);
    return RenderView{width, height, *matrices, {pose.at(0, 3), pose.at(1, 3), pose.at(2, 3)}};
}
} // namespace maya
