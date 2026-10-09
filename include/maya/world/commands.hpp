#pragma once

#include "maya/world/detail/component_pool.hpp"
#include "maya/world/components.hpp"
#include <optional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <variant>

namespace maya {
class World;
enum class ReparentPolicy { keep_local, keep_world };
using EntityTarget = std::variant<EntityHandle, PendingEntity>;
/// A group of staged entities (#1064): created into the World but invisible to everything that reads it
/// (find, alive, has, with, for_each, children, world_matrix) until World::publish makes the whole group
/// visible at once, or World::discard removes it. 0 is no group: visible as soon as it is committed.
using StageGroup = uint32_t;

/// Owned staging values; may outlive the World. No publication until World::commit.
class WorldCommands {
public:
    WorldCommands(const WorldCommands&) = delete;
    WorldCommands& operator=(const WorldCommands&) = delete;
    WorldCommands(WorldCommands&& other) noexcept;
    WorldCommands& operator=(WorldCommands&& other) noexcept;

    PendingEntity create(EntityId id = EntityId::generate());
    /// A new entity in stage group `stage` (nonzero): committed, but not visible until the group is published.
    PendingEntity create_staged(EntityId id, StageGroup stage);
    void destroy(EntityTarget target);
    template<Component T> void add(EntityTarget target, T value) {
        require_active();
        auto staged = std::make_unique<Addition<T>>(std::move(value));
        m_commands.push_back({Kind::add, target, {}, typeid(T), std::move(staged)});
    }
    template<Component T> void remove(EntityTarget target) {
        require_active();
        m_commands.push_back({Kind::remove, target, {}, typeid(T), {}});
    }
    /// Replace an existing value atomically with the rest of the command batch.
    template<Component T> void replace(EntityTarget target, T value) {
        if constexpr (std::same_as<T, TransformComponent>) {
            set_transform(target, std::move(value));
        } else {
            require_active();
            auto staged = std::make_unique<Addition<T>>(std::move(value));
            m_commands.push_back({Kind::replace, target, {}, typeid(T), std::move(staged)});
        }
    }
    void set_transform(EntityTarget target, TransformComponent value);
    /// nullopt detaches to the root. Both targets may be pending in this batch.
    void reparent(EntityTarget target, std::optional<EntityTarget> parent, ReparentPolicy policy);
    size_t size() const noexcept { return m_commands.size(); }
    /// Drops the commands staged after the first `size`, as if they were never made. Pending entities
    /// from dropped creates must not be used again. Script hosts use it to discard a failed call's work.
    void truncate(size_t size);

    enum class Kind { create, destroy, add, remove, replace, set_transform, reparent };
    /// What a staged command changes, without its value. The pending entity of a create is its target.
    struct Staged {
        Kind kind;
        EntityTarget target;
        std::type_index component; // add, remove, replace, set_transform; void otherwise
        std::optional<EntityTarget> parent; // reparent only; nullopt detaches to the root
    };
    /// The staged commands from index `first` on, in order, so a caller can check a batch before it
    /// commits (the play session refuses transform writes to physics bodies this way).
    std::vector<Staged> staged(size_t first = 0) const;
    /// The same, one at a time and without copying the batch: `visit(const Staged&)` for each.
    template<class F> void for_each_staged(size_t first, F&& visit) const {
        for (auto i = first; i < m_commands.size(); ++i) {
            const auto& command = m_commands[i];
            visit(Staged{command.kind, command.target, command.type, command.kind == Kind::reparent ? command.parent : std::nullopt});
        }
    }

private:
    friend class World;
    explicit WorldCommands(uint64_t world)
        : m_world(world), m_batch(detail::next_lifetime_token()) {}
    struct AdditionBase {
        virtual ~AdditionBase() = default;
        virtual std::unique_ptr<detail::ComponentPoolBase> make_pool() const = 0;
        virtual void replace(detail::ComponentPoolBase& pool, uint32_t slot) noexcept = 0;
        virtual void publish(detail::ComponentPoolBase& pool, uint32_t slot) noexcept = 0;
    };
    template<Component T> struct Addition final : AdditionBase {
        explicit Addition(T value) : value(std::move(value)) {}
        std::unique_ptr<detail::ComponentPoolBase> make_pool() const override {
            return std::make_unique<detail::ComponentPool<T>>();
        }
        void publish(detail::ComponentPoolBase& pool, uint32_t slot) noexcept override {
            static_cast<detail::ComponentPool<T>&>(pool).add(slot, std::move(value));
        }
        void replace(detail::ComponentPoolBase& pool, uint32_t slot) noexcept override {
            static_cast<detail::ComponentPool<T>&>(pool).value(slot) = std::move(value);
        }
        T value;
    };
    struct Command {
        Kind kind;
        EntityTarget target;
        EntityId id;
        std::type_index type{typeid(void)};
        std::unique_ptr<AdditionBase> addition;
        std::optional<EntityTarget> parent{};
        ReparentPolicy policy = ReparentPolicy::keep_local;
        TransformComponent transform{};
        StageGroup stage = 0; // create only
    };
    void require_active() const {
        if (m_world == 0) throw std::logic_error("WorldCommands is consumed or moved from");
    }
    uint64_t m_world;
    uint64_t m_batch;
    uint32_t m_created = 0;
    std::vector<Command> m_commands;
};
} // namespace maya
