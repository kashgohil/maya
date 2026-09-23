#pragma once

#include "maya/world/commands.hpp"
#include "maya/world/spatial.hpp"
#include <functional>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace maya {
enum class WorldError {
    none, busy, wrong_world, invalid_entity, invalid_id, duplicate_id,
    invalid_pending_entity, component_exists, component_missing, capacity_exhausted,
    invalid_transform, hierarchy_cycle, hierarchy_in_use, unrepresentable_transform, invalid_policy
};

struct WorldCommitResult {
    WorldError error = WorldError::none;
    size_t command_index = 0; // failing command, or command count on success
    std::vector<EntityHandle> created; // indexed by PendingEntity::index, only on success
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
    size_t size() const noexcept { return m_live.size(); }
    bool alive(EntityHandle entity) const noexcept;
    std::optional<EntityId> persistent_id(EntityHandle entity) const noexcept;
    std::optional<EntityHandle> find(EntityId id) const;
    WorldCommands commands() const { return WorldCommands(m_token); }
    /// Failure leaves logical state unchanged; allocation failure throws before publication.
    /// Success consumes commands. Commit cannot run inside with/for_each callbacks.
    WorldCommitResult commit(WorldCommands& commands);

    /// Root/invalid/no-transform returns nullopt; use has/alive to distinguish.
    std::optional<EntityHandle> parent(EntityHandle entity) const;
    std::vector<EntityHandle> children(EntityHandle entity) const;
    /// Returns a copy. Lazily updates dirty ancestors; invalid/overflowing poses fail.
    std::optional<math::Mat4> world_matrix(EntityHandle entity) const;
    std::optional<CameraMatrices> camera(EntityHandle entity, float aspect) const;

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
        math::Mat4 world{};
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
        uint32_t dense = invalid_entity_slot;
        uint32_t next_free = invalid_entity_slot;
    };
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
            for (const auto slot : smallest.get().slots()) {
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
    mutable size_t m_borrows = 0;
    bool m_committing = false;
    uint32_t m_free = invalid_entity_slot;
    std::vector<Slot> m_slots;
    mutable std::vector<SpatialNode> m_spatial;
    mutable std::vector<uint32_t> m_spatial_work;
    std::vector<uint32_t> m_live;
    std::unordered_map<EntityId, EntityHandle, PersistentIdHash> m_ids;
    std::unordered_map<std::type_index, std::unique_ptr<detail::ComponentPoolBase>> m_pools;
};
} // namespace maya
