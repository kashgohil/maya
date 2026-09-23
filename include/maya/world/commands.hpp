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

/// Owned staging values; may outlive the World. No publication until World::commit.
class WorldCommands {
public:
    WorldCommands(const WorldCommands&) = delete;
    WorldCommands& operator=(const WorldCommands&) = delete;
    WorldCommands(WorldCommands&& other) noexcept;
    WorldCommands& operator=(WorldCommands&& other) noexcept;

    PendingEntity create(EntityId id = EntityId::generate());
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
    void set_transform(EntityTarget target, TransformComponent value);
    /// nullopt detaches to the root. Both targets may be pending in this batch.
    void reparent(EntityTarget target, std::optional<EntityTarget> parent, ReparentPolicy policy);
    size_t size() const noexcept { return m_commands.size(); }

private:
    friend class World;
    explicit WorldCommands(uint64_t world)
        : m_world(world), m_batch(detail::next_lifetime_token()) {}
    enum class Kind { create, destroy, add, remove, set_transform, reparent };
    struct AdditionBase {
        virtual ~AdditionBase() = default;
        virtual std::unique_ptr<detail::ComponentPoolBase> make_pool() const = 0;
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
