#include "maya/renderer/draw_batches.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace maya {

Frustum Frustum::from(const math::Mat4& m) noexcept {
    const auto row = [&](int r) { return math::Vec4{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto frustum = Frustum{{r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2}};
    for (auto& p : frustum.planes) {
        const auto length = math::Vec3{p.x, p.y, p.z}.length();
        if (length > 0) p = p * (1.0f / length);
    }
    return frustum;
}

bool Frustum::reaches(const math::Vec3& center, float radius) const noexcept {
    if (!std::isfinite(radius)) return true;
    for (const auto& p : planes)
        if (p.x * center.x + p.y * center.y + p.z * center.z + p.w < -radius) return false;
    return true;
}

namespace {
/// Groups `candidates` by `key`, appending them to the list's order: groups in ascending key order, and
/// instances within a group in their order in `candidates`. A counting sort, linear in the instances.
template<class Key>
std::vector<DrawBatch> group(const RenderSnapshot& snapshot, const std::vector<uint32_t>& candidates, Key&& key,
                             DrawList& list, BatchScratch& scratch) {
    auto& groups = scratch.groups;
    auto& found = scratch.group_list;
    groups.clear();
    found.clear();
    scratch.group_of.resize(candidates.size());
    auto last_key = uint64_t{0};
    auto last_group = UINT32_MAX;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto k = key(snapshot.instances[candidates[i]]);
        if (last_group == UINT32_MAX || k != last_key) { // neighbours usually share a key
            const auto [at, added] = groups.try_emplace(k, uint32_t(found.size()));
            if (added) found.push_back({k, 0, 0, candidates[i]});
            last_key = k;
            last_group = at->second;
        }
        ++found[last_group].count;
        scratch.group_of[i] = last_group;
    }
    auto sorted = std::vector<uint32_t>(found.size());
    std::iota(sorted.begin(), sorted.end(), 0u);
    std::ranges::sort(sorted, {}, [&](uint32_t g) { return found[g].key; });
    auto batches = std::vector<DrawBatch>{};
    batches.reserve(found.size());
    auto next = uint32_t(list.order.size());
    for (const auto g : sorted) {
        const auto& instance = snapshot.instances[found[g].representative];
        batches.push_back({next, found[g].count, instance.mesh, instance.material});
        found[g].next = next;
        next += found[g].count;
    }
    list.order.resize(next);
    for (size_t i = 0; i < candidates.size(); ++i) list.order[found[scratch.group_of[i]].next++] = candidates[i];
    return batches;
}
} // namespace

ViewBatches plan_view_batches(const RenderSnapshot& snapshot, const RenderView& view, DrawList& list, BatchScratch& scratch) {
    auto result = ViewBatches{};
    const auto frustum = Frustum::from(view.matrices.view_projection);
    auto& visible = scratch.candidates;
    visible.clear();
    auto& blended = scratch.blended;
    blended.clear();
    for (uint32_t i = 0; i < snapshot.instances.size(); ++i) {
        const auto& instance = snapshot.instances[i];
        if (!frustum.reaches(instance.bounds_center, instance.bounds_radius)) {
            ++result.culled;
            continue;
        }
        (snapshot.materials[instance.material].alpha_mode == AlphaMode::blend ? blended : visible).push_back(i);
    }
    result.drawn = visible.size() + blended.size();
    // Single-sided before double-sided (their pipelines differ), then by material, then by mesh.
    result.opaque = group(snapshot, visible, [&](const RenderInstance& instance) {
        return uint64_t(snapshot.materials[instance.material].double_sided) << 63 | uint64_t(instance.material) << 32 | instance.mesh;
    }, list, scratch);
    // Blended surfaces back to front by their origins, as before; neighbours of one mesh and material share a draw.
    auto& distance = scratch.distances;
    distance.resize(snapshot.instances.size());
    for (const auto i : blended) {
        const auto& world = snapshot.instances[i].world;
        distance[i] = (math::Vec3{world.at(0, 3), world.at(1, 3), world.at(2, 3)} - view.position).length_squared();
    }
    std::ranges::stable_sort(blended, [&](uint32_t a, uint32_t b) { return distance[a] > distance[b]; });
    for (const auto i : blended) {
        const auto& instance = snapshot.instances[i];
        auto& batches = result.blended;
        if (batches.empty() || batches.back().mesh != instance.mesh || batches.back().material != instance.material)
            batches.push_back({uint32_t(list.order.size()), 0, instance.mesh, instance.material});
        list.order.push_back(i);
        ++batches.back().count;
    }
    return result;
}

std::vector<DrawBatch> plan_shadow_batches(const RenderSnapshot& snapshot, const std::function<bool(const RenderInstance&)>& casts,
                                           DrawList& list, BatchScratch& scratch) {
    auto& casters = scratch.candidates;
    casters.clear();
    for (uint32_t i = 0; i < snapshot.instances.size(); ++i) {
        const auto& instance = snapshot.instances[i];
        if (snapshot.materials[instance.material].alpha_mode != AlphaMode::blend && casts(instance)) casters.push_back(i);
    }
    return group(snapshot, casters, [&](const RenderInstance& instance) {
        const auto masked = snapshot.materials[instance.material].alpha_mode == AlphaMode::mask;
        return masked ? uint64_t(1) << 63 | uint64_t(instance.material) << 32 | instance.mesh : uint64_t(instance.mesh);
    }, list, scratch);
}

} // namespace maya
