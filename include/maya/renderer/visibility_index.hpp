#pragma once
// The visibility index (#1066, docs/renderer.md#the-visibility-index): a loose grid of renderable bounds,
// kept up to date from the World's change log, so extraction visits what a view and its shadow maps may
// see rather than every mesh renderer. #1060's record chose it: 32 m cells (4 x 4 a 128 m streaming cell)
// whose bounds grow to hold what is in them; objects larger than the slack go in their 128 m cell's list of
// large objects. A static entity costs nothing per frame.

#include "maya/assets/registry.hpp"
#include "maya/math/vector.hpp"
#include "maya/renderer/draw_batches.hpp"
#include "maya/world/components.hpp"
#include "maya/world/world.hpp"
#include <span>
#include <unordered_map>
#include <vector>

namespace maya {

struct VisibilityStats {
    size_t renderers = 0; // mesh renderers in the World
    size_t hidden = 0; // of those, not visible or with no mesh: never returned
    size_t gridded = 0, large = 0; // bounded, in a 32 m cell, or in a 128 m cell's large list
    size_t unbounded = 0; // returned by every query: no transform yet, a mesh not resident, no CPU geometry, or skinned
    size_t cells = 0, large_cells = 0; // occupied
    size_t updated = 0; // entities the last update looked at again
    size_t rebuilds = 0; // updates that indexed every mesh renderer again: the first, and after the World's log overflowed
    size_t returned = 0; // entities the last query returned
    double update_ms = 0.0, query_ms = 0.0; // the last update's and query's time
};

class VisibilityIndex {
public:
    static constexpr double cell_size = 32.0; // metres: 4 x 4 a streaming cell
    static constexpr double large_cell_size = 128.0; // the streaming cells' grid
    static constexpr float slack = 8.0f; // metres: objects with a larger bounding radius are large

    /// An index of `world`, which it reads through a change log (World::open_change_log). It may outlive the
    /// World: it is then empty, and updating it is an error.
    explicit VisibilityIndex(const World& world);
    ~VisibilityIndex();
    VisibilityIndex(const VisibilityIndex&) = delete;
    VisibilityIndex& operator=(const VisibilityIndex&) = delete;

    uint64_t world() const noexcept { return m_token; }
    /// Applies the World's changes since the last update (the first indexes every mesh renderer): what was
    /// created, destroyed, changed, moved (and what is below it), published, or staged, and the instances
    /// of meshes that became resident or were reloaded. Bounds are the World's poses with the meshes' CPU
    /// bounds; it never loads an asset.
    void update(const World& world, const AssetRegistry& assets);
    /// Appends every entity whose bounds may reach one of `volumes` (in the frame of `origin`, as the
    /// renderer's views are, #1065), and every entity it cannot bound, each once, in slot order.
    void query(std::span<const Frustum> volumes, const math::DVec3& origin, std::vector<EntityHandle>& out);
    /// Whether the last query returned `entity`, with no update since.
    bool returned(EntityHandle entity) const noexcept;
    /// The mesh renderer of an entity the last query returned, as the World had it at the last update: what
    /// extraction draws, without looking it up in the World again.
    const MeshRendererComponent& renderer(EntityHandle entity) const noexcept { return m_items[entity.slot].renderer; }
    /// The level of detail last chosen for an entity's instance in this index's view, for hysteresis
    /// (docs/renderer.md#levels-of-detail); none when it was not drawn by the last frame.
    std::optional<uint8_t> level(EntityHandle entity) const noexcept;
    void set_level(EntityHandle entity, uint8_t level) noexcept;
    const VisibilityStats& stats() const noexcept { return m_stats; }

private:
    enum class Where : uint8_t { none, hidden, grid, large, unbounded };
    struct Item {
        EntityHandle entity;
        Where where = Where::none;
        uint64_t cell = 0; // its key in m_grid or m_large
        uint32_t position = 0; // its index in that cell's list, or in m_unbounded
        math::DVec3 center{};
        float radius = 0.0f;
        AssetId mesh{};
        MeshRendererComponent renderer{}; // as the World had it at the last update
        uint8_t level = 0; // last level of detail drawn
        uint64_t drawn = 0; // the query that last set it
        uint64_t seen = 0; // the query or update that last visited it
        uint64_t expanded = 0; // the update that last walked its slot's children
    };
    struct Cell {
        math::DVec3 low{}, high{};
        std::vector<uint32_t> slots;
    };
    struct MeshBounds {
        uint64_t generation = 0;
        math::Vec3 center{0.0f}; // of the local bounds
        float half_diagonal = 0.0f;
        bool bounded = false; // it has CPU geometry
        bool skinned = false;
    };
    void evaluate(const World& world, const AssetRegistry& assets, EntityHandle entity);
    void place(Item& item, Where where, uint64_t cell);
    void unplace(Item& item);
    Item& item(EntityHandle entity);
    const MeshBounds* bounds(const AssetRegistry& assets, AssetId mesh);

    const World* m_world;
    std::weak_ptr<const bool> m_lifetime;
    uint64_t m_token;
    uint32_t m_log = 0;
    bool m_built = false;
    std::vector<Item> m_items; // by entity slot
    std::unordered_map<uint64_t, Cell> m_grid, m_large;
    std::vector<uint32_t> m_unbounded;
    std::unordered_map<AssetId, MeshBounds, PersistentIdHash> m_meshes;
    std::vector<EntityHandle> m_work;
    uint64_t m_stamp = 0; // updates and queries, for visiting each item once
    uint64_t m_queried = 0; // the stamp of the last query
    std::vector<uint64_t> m_returned; // a bit per slot the last query returned
    uint64_t m_frame = 0; // queries: what was drawn by the last one
    VisibilityStats m_stats;
};

} // namespace maya
