#pragma once

#include "maya/world/commands.hpp"
#include "maya/world/spatial.hpp"
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace maya {
enum class WorldError {
    none, busy, wrong_world, invalid_entity, invalid_id, duplicate_id,
    invalid_pending_entity, component_exists, component_missing, capacity_exhausted,
    invalid_transform, hierarchy_cycle, hierarchy_in_use, unrepresentable_transform, invalid_policy,
    stage_mismatch // a parent and child in different stage groups, or one staged and one visible
};

/// A short lowercase name for messages, such as "hierarchy cycle".
const char* error_name(WorldError error) noexcept;

/// An entry of the edit journal: an entity published from stage group `origin` that a commit changed.
struct WorldEdit {
    EntityId id;
    StageGroup origin = 0;
};

struct WorldCommitResult {
    WorldError error = WorldError::none;
    size_t command_index = 0; // failing command, or command count on success
    std::vector<EntityHandle> created; // indexed by PendingEntity::index, only on success
    /// Every entity the batch destroyed (destroyed subtrees included), as their handles were, so owners of
    /// per-entity state (physics) find them without scanning (#1064). Only on success.
    std::vector<EntityHandle> destroyed;
    explicit operator bool() const noexcept { return error == WorldError::none; }
};

/// Single-owner-thread World. Nonmovable: runtime handles belong to this lifetime.
/// Callback references are scoped borrows and must not escape. Structural edits use commands.
class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    uint64_t token() const noexcept { return m_token; }
    /// Counts commits that created, destroyed, or reparented entities, or added, replaced, or removed a
    /// NameComponent: what entities' name paths depend on (name_path.hpp). Caches of them, such as
    /// animation's joint bindings, stay valid while it is unchanged.
    uint64_t names_revision() const noexcept { return m_names_revision; }
    /// Visible entities: staged ones are not counted.
    size_t size() const noexcept { return m_live.size(); }
    /// The entity exists and is visible (not staged).
    bool alive(EntityHandle entity) const noexcept;
    std::optional<EntityId> persistent_id(EntityHandle entity) const noexcept;
    std::optional<EntityHandle> find(EntityId id) const;
    WorldCommands commands() const { return WorldCommands(m_token); }

    // Stage groups (#1064): a cell's entities are committed over several frames, then shown at once.
    /// Makes every entity of `group` visible together, and returns them. Throws only on allocation
    /// failure, before anything changes. Commits must not be in progress.
    std::vector<EntityHandle> publish(StageGroup group);
    /// Destroys the entities of `group` (still invisible), at most `limit` of them, as if they had never been
    /// committed. Returns how many are left, so a big group can go over several frames.
    size_t discard(StageGroup group, size_t limit = std::numeric_limits<size_t>::max());
    /// Hides visible entities into stage group `group` in one step: what an unloading cell does before it is
    /// torn down over several frames. They must be whole hierarchies (no parent or child outside them);
    /// otherwise nothing changes and the error is stage_mismatch.
    WorldError stage(std::span<const EntityHandle> entities, StageGroup group);
    /// The entities of a stage group, by handle: what a later batch of the same activation targets.
    std::vector<EntityHandle> staged(StageGroup group) const;
    size_t staged_count() const noexcept { return m_staged; }

    // The transform journal (#1064): owners that mirror entity poses (physics) learn which ones changed.
    /// Turns on recording of entities whose local transform was set, added, removed, or reparented.
    void record_transform_changes(bool on) noexcept {
        m_journal_on = on;
        if (!on) m_journal.clear();
    }
    /// The entities recorded since the last call (a moved entity's descendants moved too), and clears them.
    std::vector<EntityHandle> take_transform_changes() { return std::exchange(m_journal, {}); }
    // The edit journal (#1064): a streaming world learns which of a cell's entities changed in play.
    /// Turns on recording of entities published from a stage group whose components were added, replaced,
    /// removed, or moved, or that were reparented or destroyed. Each edit names the group, so its owner
    /// needs no index of every entity.
    void record_edits(bool on) noexcept {
        m_edits_on = on;
        if (!on) m_edits.clear();
    }
    std::vector<WorldEdit> take_edits() { return std::exchange(m_edits, {}); }
    /// Room for this many entities, visible or staged, before a commit must grow the World's tables:
    /// growing them costs time in proportion to the entities already there.
    void reserve(size_t entities);
    /// Failure leaves logical state unchanged; allocation failure throws before publication.
    /// Success consumes commands. Commit cannot run inside with/for_each callbacks.
    WorldCommitResult commit(WorldCommands& commands);

    /// Root/invalid/no-transform returns nullopt; use has/alive to distinguish.
    std::optional<EntityHandle> parent(EntityHandle entity) const;
    std::vector<EntityHandle> children(EntityHandle entity) const;
    /// The entity's world pose (translation in double, #1065), as a copy. Lazily updates dirty ancestors;
    /// invalid/overflowing poses fail.
    std::optional<math::Affine> world_matrix(EntityHandle entity) const;
    std::optional<CameraMatrices> camera(EntityHandle entity, float aspect) const;

    /// Counts staged entities' components too.
    template<Component T> bool has(EntityHandle entity) const {
        if (m_committing || !alive(entity)) return false;
        const auto pool = find_pool<T>();
        return pool && pool->get().contains(entity.slot);
    }
    template<Component T> size_t component_count() const {
        if (m_committing) return 0;
        const auto pool = find_pool<T>();
        return pool ? pool->get().size() : 0;
    }
    template<Component T, class F>
    bool with(EntityHandle entity, F&& callback) {
        if (!has<T>(entity)) return false;
        auto borrow = Borrow(*this);
        std::invoke(callback, query_value<T>(*this, find_pool<T>()->get(), entity.slot));
        return true;
    }
    template<Component T, class F>
    bool with(EntityHandle entity, F&& callback) const {
        if (!has<T>(entity)) return false;
        auto borrow = Borrow(*this);
        std::invoke(callback, query_value<T>(*this, find_pool<T>()->get(), entity.slot));
        return true;
    }
    template<class F> requires std::invocable<F&, EntityHandle>
    void for_each_entity(F&& callback) const {
        auto borrow = Borrow(*this);
        for (const auto slot : m_live) std::invoke(callback, handle(slot));
    }
    template<Component... Ts, class F>
        requires (sizeof...(Ts) > 0)
    void for_each(F&& callback) { each_impl<Ts...>(*this, callback); }
    template<Component... Ts, class F>
        requires (sizeof...(Ts) > 0)
    void for_each(F&& callback) const { each_impl<Ts...>(*this, callback); }

private:
    struct SpatialNode {
        uint32_t parent = invalid_entity_slot;
        uint32_t first_child = invalid_entity_slot;
        uint32_t previous = invalid_entity_slot;
        uint32_t next = invalid_entity_slot;
        bool dirty = true;
        bool valid = false;
        bool rigid_ancestry = true;
        math::Affine world{};
    };
    template<Component T, class Self, class Pool>
    static decltype(auto) query_value(Self&, Pool& pool, uint32_t slot) {
        if constexpr (std::is_same_v<T, TransformComponent>)
            return std::as_const(pool.value(slot));
        else return (pool.value(slot));
    }
    struct Slot {
        EntityId id{};
        uint64_t generation = 1;
        uint32_t dense = invalid_entity_slot; // in m_live; invalid while staged
        uint32_t next_free = invalid_entity_slot;
        StageGroup stage = 0;
        StageGroup origin = 0; // the group it was published from; 0 when created visible
    };
    /// The entity exists, staged or not: what commits may target.
    bool exists(EntityHandle entity) const noexcept;
    class Borrow {
    public:
        explicit Borrow(const World& world) : m_world(world) {
            if (world.m_committing) throw std::logic_error("World access during publication");
            ++m_world.m_borrows;
        }
        ~Borrow() { --m_world.m_borrows; }
        Borrow(const Borrow&) = delete;
        Borrow& operator=(const Borrow&) = delete;
    private:
        const World& m_world;
    };
    template<Component T> auto find_pool() {
        using Ref = std::optional<std::reference_wrapper<detail::ComponentPool<T>>>;
        const auto it = m_pools.find(typeid(T));
        if (it == m_pools.end()) return Ref{};
        return Ref{static_cast<detail::ComponentPool<T>&>(*it->second)};
    }
    template<Component T> auto find_pool() const {
        using Ref = std::optional<std::reference_wrapper<const detail::ComponentPool<T>>>;
        const auto it = m_pools.find(typeid(T));
        if (it == m_pools.end()) return Ref{};
        return Ref{static_cast<const detail::ComponentPool<T>&>(*it->second)};
    }
    template<Component... Ts, class Self, class F>
    static void each_impl(Self& world, F& callback) {
        auto borrow = Borrow(world);
        auto pools = std::tuple{world.template find_pool<Ts>()...};
        std::apply([&](auto&... pool) {
            if (!(pool.has_value() && ...)) return;
            auto smallest = std::cref(static_cast<const detail::ComponentPoolBase&>(
                std::get<0>(pools)->get()));
            ((smallest = pool->get().size() < smallest.get().size()
                ? std::cref(static_cast<const detail::ComponentPoolBase&>(pool->get()))
                : smallest), ...);
            const auto any_staged = world.m_staged != 0;
            for (const auto slot : smallest.get().slots()) {
                if (any_staged && world.m_slots[slot].stage != 0) continue; // not published yet
                if ((pool->get().contains(slot) && ...))
                    std::invoke(callback, world.handle(slot), query_value<Ts>(world, pool->get(), slot)...);
            }
        }, pools);
    }
    EntityHandle handle(uint32_t slot) const noexcept {
        return {m_token, slot, m_slots[slot].generation};
    }
    void destroy(uint32_t slot) noexcept;
    void dirty_subtree(uint32_t slot) noexcept;

    const uint64_t m_token;
    uint64_t m_names_revision = 0;
    mutable size_t m_borrows = 0;
    bool m_committing = false;
    uint32_t m_free = invalid_entity_slot;
    std::vector<Slot> m_slots;
    mutable std::vector<SpatialNode> m_spatial;
    mutable std::vector<uint32_t> m_spatial_work;
    std::vector<uint32_t> m_live; // visible entities
    size_t m_staged = 0;
    std::unordered_map<StageGroup, std::vector<uint32_t>> m_stages;
    bool m_journal_on = false;
    std::vector<EntityHandle> m_journal;
    bool m_edits_on = false;
    std::vector<WorldEdit> m_edits;
    std::unordered_map<EntityId, EntityHandle, PersistentIdHash> m_ids;
    std::unordered_map<std::type_index, std::unique_ptr<detail::ComponentPoolBase>> m_pools;
};
} // namespace maya
