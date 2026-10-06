#pragma once
// What each pass draws (#1025, docs/renderer.md#culling-and-batching): the instances inside its frustum,
// grouped so that each group of one mesh and one material is a single instanced draw. Pure calculation
// from a snapshot, so it is tested without a GPU; the renderer uploads the order and encodes the batches.

#include "maya/renderer/render_snapshot.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace maya {

/// A view's or shadow map's clip volume as six planes (a, b, c, d with a x + b y + c z + d >= 0 inside),
/// from its view-projection; Metal's depth is [0, 1].
struct Frustum {
    std::array<math::Vec4, 6> planes{};
    static Frustum from(const math::Mat4& view_projection) noexcept;
    /// Whether a sphere reaches inside. An infinite radius always does.
    bool reaches(const math::Vec3& center, float radius) const noexcept;
};

/// One instanced draw: `count` instances of one mesh with one material, numbered from `first` in the
/// pass's order (DrawList::order).
struct DrawBatch {
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t mesh = 0; // index into RenderSnapshot::meshes
    uint32_t material = 0; // index into RenderSnapshot::materials
};

/// The instances one frame's passes draw, in drawing order: each pass's batches index one shared order,
/// uploaded once.
struct DrawList {
    std::vector<uint32_t> order; // RenderSnapshot::instances, grouped by batch
    void clear() noexcept { order.clear(); }
};

/// A view's batches: opaque and masked surfaces grouped by material and mesh (single-sided first, then
/// double-sided, each by material), then blended ones back to front, where only consecutive instances of
/// the same mesh and material share a batch.
struct ViewBatches {
    std::vector<DrawBatch> opaque;
    std::vector<DrawBatch> blended;
    size_t drawn = 0; // instances inside the view
    size_t culled = 0; // instances outside it
};

/// Reused working memory, so steady frames do not allocate.
struct BatchScratch {
    std::vector<uint32_t> group_of;
    std::vector<uint32_t> candidates;
    std::unordered_map<uint64_t, uint32_t> groups;
    struct Group { uint64_t key; uint32_t count; uint32_t next; uint32_t representative; };
    std::vector<Group> group_list;
    std::vector<uint32_t> blended;
    std::vector<float> distances;
};

/// Appends the view's visible instances to `list` and returns their batches.
ViewBatches plan_view_batches(const RenderSnapshot& snapshot, const RenderView& view, DrawList& list, BatchScratch& scratch);

/// Appends one shadow map's casters to `list` and returns their batches: instances `casts` accepts that
/// are not blended, opaque ones grouped by mesh alone (any material casts the same), masked ones by
/// material and mesh, opaque first.
std::vector<DrawBatch> plan_shadow_batches(const RenderSnapshot& snapshot, const std::function<bool(const RenderInstance&)>& casts,
                                           DrawList& list, BatchScratch& scratch);

} // namespace maya
