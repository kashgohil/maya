#include "maya/world/world.hpp"
#include <limits>
#include <utility>

namespace maya {
WorldCommands::WorldCommands(WorldCommands&& other) noexcept
    : m_world(std::exchange(other.m_world, 0)), m_batch(std::exchange(other.m_batch, 0)),
      m_created(std::exchange(other.m_created, 0)), m_commands(std::move(other.m_commands)) {}

WorldCommands& WorldCommands::operator=(WorldCommands&& other) noexcept {
    if (this != &other) {
        m_world = std::exchange(other.m_world, 0);
        m_batch = std::exchange(other.m_batch, 0);
        m_created = std::exchange(other.m_created, 0);
        m_commands = std::move(other.m_commands);
    }
    return *this;
}

PendingEntity WorldCommands::create(EntityId id) {
    require_active();
    if (m_created == invalid_entity_slot) throw std::length_error("Too many pending entities");
    const auto pending = PendingEntity{m_batch, m_created};
    m_commands.push_back({Kind::create, pending, id, typeid(void), {}});
    ++m_created;
    return pending;
}

void WorldCommands::destroy(EntityTarget target) {
    require_active();
    m_commands.push_back({Kind::destroy, target, {}, typeid(void), {}});
}

World::World() : m_token(detail::next_lifetime_token()) {}

World::~World() {
    m_committing = true;
    // All entity identities disappear before resource-owning components are destroyed.
    m_ids.clear();
    m_live.clear();
    for (auto& slot : m_slots) slot.id = {};
    m_pools.clear();
}

bool World::alive(EntityHandle entity) const noexcept {
    return entity.world == m_token && entity.slot < m_slots.size() &&
        entity.generation != 0 && m_slots[entity.slot].generation == entity.generation &&
        m_slots[entity.slot].id.valid();
}

std::optional<EntityId> World::persistent_id(EntityHandle entity) const noexcept {
    if (!alive(entity)) return std::nullopt;
    return m_slots[entity.slot].id;
}

std::optional<EntityHandle> World::find(EntityId id) const {
    const auto it = m_ids.find(id);
    if (it == m_ids.end()) return std::nullopt;
    return it->second;
}

WorldCommitResult World::commit(WorldCommands& commands) {
    if (m_committing || m_borrows != 0) return {WorldError::busy, 0, {}};
    if (commands.m_world != m_token) return {WorldError::wrong_world, 0, {}};
    struct PublicationGuard {
        bool& active;
        explicit PublicationGuard(bool& flag) : active(flag) { active = true; }
        ~PublicationGuard() { active = false; }
    } guard(m_committing);

    using Kind = WorldCommands::Kind;
    struct VirtualEntity {
        bool alive = true;
        std::unordered_map<std::type_index, bool> components;
    };
    auto touched = std::unordered_map<uint32_t, VirtualEntity>{};
    auto additions = std::unordered_map<std::type_index, size_t>{};
    auto new_pools = decltype(m_pools){};
    auto new_ids = decltype(m_ids){};
    auto resolved = std::vector<uint32_t>(commands.m_commands.size());
    auto result = WorldCommitResult{};
    result.created.resize(commands.m_created);
    auto next_free = m_free;
    auto slot_count = m_slots.size();

    // Simulate the batch in enqueue order without modifying live identity or components.
    for (size_t index = 0; index < commands.m_commands.size(); ++index) {
        const auto& command = commands.m_commands[index];
        const auto fail = [index](WorldError error) { return WorldCommitResult{error, index, {}}; };
        auto slot = invalid_entity_slot;
        if (command.kind == Kind::create) {
            if (!command.id.valid()) return fail(WorldError::invalid_id);
            // Restoring a deleted ID is allowed in a later commit, not twice in one batch.
            if (m_ids.contains(command.id) || new_ids.contains(command.id))
                return fail(WorldError::duplicate_id);
            if (next_free != invalid_entity_slot) {
                slot = next_free;
                next_free = m_slots[slot].next_free;
            } else {
                if (slot_count >= invalid_entity_slot) return fail(WorldError::capacity_exhausted);
                slot = static_cast<uint32_t>(slot_count++);
            }
            const auto generation = slot < m_slots.size() ? m_slots[slot].generation : uint64_t{1};
            const auto entity = EntityHandle{m_token, slot, generation};
            result.created[std::get<PendingEntity>(command.target).index] = entity;
            new_ids.emplace(command.id, entity);
        } else {
            if (std::holds_alternative<EntityHandle>(command.target)) {
                const auto entity = std::get<EntityHandle>(command.target);
                if (entity.world != m_token) return fail(WorldError::wrong_world);
                if (!alive(entity)) return fail(WorldError::invalid_entity);
                slot = entity.slot;
            } else {
                const auto pending = std::get<PendingEntity>(command.target);
                if (pending.batch != commands.m_batch || pending.index >= result.created.size() ||
                    result.created[pending.index].slot == invalid_entity_slot)
                    return fail(WorldError::invalid_pending_entity);
                slot = result.created[pending.index].slot;
            }
        }
        resolved[index] = slot;
        auto& state = touched[slot];
        if (!state.alive) return fail(WorldError::invalid_entity);
        if (command.kind == Kind::destroy) {
            state.alive = false;
        } else if (command.kind == Kind::add || command.kind == Kind::remove) {
            auto present = state.components.find(command.type);
            if (present == state.components.end()) {
                const auto pool = m_pools.find(command.type);
                const auto exists = pool != m_pools.end() && pool->second->contains(slot);
                present = state.components.emplace(command.type, exists).first;
            }
            if (command.kind == Kind::add) {
                if (present->second) return fail(WorldError::component_exists);
                present->second = true;
                ++additions[command.type];
                if (!m_pools.contains(command.type) && !new_pools.contains(command.type))
                    new_pools.emplace(command.type, command.addition->make_pool());
            } else {
                if (!present->second) return fail(WorldError::component_missing);
                present->second = false;
            }
        }
    }

    // Fallible capacity preparation. Existing values may relocate, but logical state stays intact.
    detail::reserve_for(m_slots, slot_count);
    detail::reserve_for(m_live, m_live.size() + result.created.size());
    const auto prepare_map = [](auto& map, size_t incoming) {
        const auto required = map.size() + incoming;
        if (static_cast<double>(required) >
            static_cast<double>(map.bucket_count()) * map.max_load_factor())
            map.reserve(required);
    };
    prepare_map(m_ids, new_ids.size());
    prepare_map(m_pools, new_pools.size());
    for (const auto& [type, count] : additions) {
        const auto existing = m_pools.find(type);
        auto& pool = existing == m_pools.end() ? *new_pools.at(type) : *existing->second;
        pool.prepare(slot_count, count);
    }

    // No allocation or throwing component operations after this point. Node transfer reuses
    // prepared allocations, with destination hash-table capacity already reserved above.
    m_slots.resize(slot_count);
    m_ids.merge(new_ids);
    m_pools.merge(new_pools);
    m_free = next_free;
    for (size_t index = 0; index < commands.m_commands.size(); ++index) {
        auto& command = commands.m_commands[index];
        const auto slot = resolved[index];
        switch (command.kind) {
        case Kind::create:
            m_slots[slot].id = command.id;
            m_slots[slot].dense = static_cast<uint32_t>(m_live.size());
            m_slots[slot].next_free = invalid_entity_slot;
            m_live.push_back(slot);
            break;
        case Kind::destroy:
            destroy(slot);
            break;
        case Kind::add:
            command.addition->publish(*m_pools.at(command.type), slot);
            break;
        case Kind::remove:
            m_pools.at(command.type)->remove(slot);
            break;
        }
    }
    result.command_index = commands.m_commands.size();
    commands.m_commands.clear();
    commands.m_world = 0;
    commands.m_batch = 0;
    commands.m_created = 0;
    return result;
}

void World::destroy(uint32_t slot) noexcept {
    auto& entity = m_slots[slot];
    m_ids.erase(entity.id);
    entity.id = {};
    const auto moved_slot = m_live.back();
    m_live[entity.dense] = moved_slot;
    m_slots[moved_slot].dense = entity.dense;
    m_live.pop_back();
    entity.dense = invalid_entity_slot;
    // Retire exhausted slots. Never allow an ancient handle to become valid after wrap.
    if (entity.generation != std::numeric_limits<uint64_t>::max()) {
        ++entity.generation;
        entity.next_free = m_free;
        m_free = slot;
    }
    for (auto& [type, pool] : m_pools) pool->remove(slot);
}
} // namespace maya
