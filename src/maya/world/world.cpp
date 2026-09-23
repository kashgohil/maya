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

void WorldCommands::set_transform(EntityTarget target, TransformComponent value) {
    require_active();
    auto command = Command{Kind::set_transform, target, {}, typeid(TransformComponent), {}};
    command.transform = value;
    m_commands.push_back(std::move(command));
}

void WorldCommands::reparent(EntityTarget target, std::optional<EntityTarget> parent,
                             ReparentPolicy policy) {
    require_active();
    auto command = Command{Kind::reparent, target, {}, typeid(void), {}};
    command.parent = parent;
    command.policy = policy;
    m_commands.push_back(std::move(command));
}

std::optional<EntityHandle> World::parent(EntityHandle entity) const {
    if (m_committing || !alive(entity)) return std::nullopt;
    const auto slot = m_spatial[entity.slot].parent;
    return slot == invalid_entity_slot ? std::nullopt : std::optional{handle(slot)};
}

std::vector<EntityHandle> World::children(EntityHandle entity) const {
    auto result = std::vector<EntityHandle>{};
    if (m_committing || !alive(entity)) return result;
    for (auto child = m_spatial[entity.slot].first_child; child != invalid_entity_slot;
         child = m_spatial[child].next) result.push_back(handle(child));
    return result;
}

void World::dirty_subtree(uint32_t slot) noexcept {
    m_spatial_work.clear();
    m_spatial_work.push_back(slot); // scratch capacity prepared before publication
    while (!m_spatial_work.empty()) {
        const auto current = m_spatial_work.back();
        m_spatial_work.pop_back();
        auto& node = m_spatial[current];
        if (node.dirty) continue; // descendants already dirty
        node.dirty = true;
        for (auto child = node.first_child; child != invalid_entity_slot;
             child = m_spatial[child].next) m_spatial_work.push_back(child);
    }
}

std::optional<math::Mat4> World::world_matrix(EntityHandle entity) const {
    if (!has<TransformComponent>(entity)) return std::nullopt;
    m_spatial_work.clear();
    auto current = entity.slot;
    while (current != invalid_entity_slot && m_spatial[current].dirty) {
        m_spatial_work.push_back(current);
        current = m_spatial[current].parent;
    }
    const auto& pool = find_pool<TransformComponent>()->get();
    while (!m_spatial_work.empty()) {
        current = m_spatial_work.back();
        m_spatial_work.pop_back();
        auto& node = m_spatial[current];
        const auto& local = pool.value(current);
        const auto matrix = local_matrix(local);
        auto world = std::optional{matrix};
        node.rigid_ancestry = unit_scale(local.scale);
        if (node.parent != invalid_entity_slot) {
            const auto& ancestor = m_spatial[node.parent];
            world = ancestor.valid ? compose_affine(ancestor.world, matrix) : std::nullopt;
            node.rigid_ancestry = node.rigid_ancestry && ancestor.rigid_ancestry;
        }
        node.valid = world.has_value();
        if (world) node.world = *world;
        node.dirty = false;
    }
    const auto& node = m_spatial[entity.slot];
    return node.valid ? std::optional{node.world} : std::nullopt;
}

std::optional<CameraMatrices> World::camera(EntityHandle entity, float aspect) const {
    if (!has<CameraComponent>(entity)) return std::nullopt;
    const auto pose = world_matrix(entity);
    if (!pose || !m_spatial[entity.slot].rigid_ancestry) return std::nullopt;
    return camera_matrices(find_pool<CameraComponent>()->get().value(entity.slot), *pose, aspect);
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

    struct SpatialEdit {
        SpatialNode node;
        std::optional<TransformComponent> local;
    };
    auto spatial = std::unordered_map<uint32_t, SpatialEdit>{};
    auto transforms = std::vector<std::optional<TransformComponent>>(commands.m_commands.size());
    auto destroyed = std::vector<std::vector<uint32_t>>(commands.m_commands.size());
    auto changed = std::vector<uint32_t>{};
    const auto transform_pool = find_pool<TransformComponent>();
    const auto edit = [&](uint32_t slot) -> SpatialEdit& {
        auto [it, inserted] = spatial.try_emplace(slot);
        if (inserted && slot < m_spatial.size()) {
            it->second.node = m_spatial[slot];
            if (transform_pool && transform_pool->get().contains(slot))
                it->second.local = transform_pool->get().value(slot);
        }
        return it->second;
    };
    const auto unlink = [&](uint32_t slot) {
        auto& node = edit(slot).node;
        if (node.previous != invalid_entity_slot) edit(node.previous).node.next = node.next;
        else if (node.parent != invalid_entity_slot) edit(node.parent).node.first_child = node.next;
        if (node.next != invalid_entity_slot) edit(node.next).node.previous = node.previous;
        node.parent = node.previous = node.next = invalid_entity_slot;
    };
    const auto staged_matrix = [&](uint32_t slot) -> std::optional<math::Mat4> {
        auto path = std::vector<uint32_t>{};
        for (auto current = slot; current != invalid_entity_slot; current = edit(current).node.parent)
            path.push_back(current);
        auto matrix = math::Mat4::identity();
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            const auto& local = edit(*it).local;
            if (!local) return std::nullopt;
            auto composed = compose_affine(matrix, local_matrix(*local));
            if (!composed) return std::nullopt;
            matrix = *composed;
        }
        return matrix;
    };
    const auto resolve_target = [&](const EntityTarget& target, uint32_t& slot) -> WorldError {
        if (const auto entity = std::get_if<EntityHandle>(&target)) {
            if (entity->world != m_token) return WorldError::wrong_world;
            if (!alive(*entity)) return WorldError::invalid_entity;
            slot = entity->slot;
        } else {
            const auto pending = std::get<PendingEntity>(target);
            if (pending.batch != commands.m_batch || pending.index >= result.created.size() ||
                result.created[pending.index].slot == invalid_entity_slot)
                return WorldError::invalid_pending_entity;
            slot = result.created[pending.index].slot;
        }
        if (const auto it = touched.find(slot); it != touched.end() && !it->second.alive)
            return WorldError::invalid_entity;
        return WorldError::none;
    };

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
            spatial[slot] = SpatialEdit{};
        } else {
            const auto error = resolve_target(command.target, slot);
            if (error != WorldError::none) return fail(error);
        }
        resolved[index] = slot;
        auto& state = touched[slot];
        if (!state.alive) return fail(WorldError::invalid_entity);
        if (command.kind == Kind::destroy) {
            unlink(slot);
            auto& subtree = destroyed[index];
            subtree.push_back(slot);
            for (size_t i = 0; i < subtree.size(); ++i) {
                const auto current = subtree[i];
                touched[current].alive = false;
                for (auto child = edit(current).node.first_child; child != invalid_entity_slot;
                     child = edit(child).node.next) subtree.push_back(child);
            }
            for (const auto current : subtree) spatial[current] = SpatialEdit{};
        } else if (command.kind == Kind::set_transform) {
            if (!edit(slot).local) return fail(WorldError::component_missing);
            transforms[index] = validated_transform(command.transform);
            if (!transforms[index]) return fail(WorldError::invalid_transform);
            edit(slot).local = transforms[index];
            changed.push_back(slot);
        } else if (command.kind == Kind::reparent) {
            if (command.policy != ReparentPolicy::keep_local &&
                command.policy != ReparentPolicy::keep_world) return fail(WorldError::invalid_policy);
            auto parent_slot = invalid_entity_slot;
            if (command.parent) {
                const auto error = resolve_target(*command.parent, parent_slot);
                if (error != WorldError::none) return fail(error);
                if (!edit(parent_slot).local) return fail(WorldError::component_missing);
            }
            if (!edit(slot).local) return fail(WorldError::component_missing);
            for (auto ancestor = parent_slot; ancestor != invalid_entity_slot;
                 ancestor = edit(ancestor).node.parent)
                if (ancestor == slot) return fail(WorldError::hierarchy_cycle);
            if (parent_slot == edit(slot).node.parent) continue;
            if (command.policy == ReparentPolicy::keep_world) {
                const auto old_world = staged_matrix(slot);
                const auto parent_world = staged_matrix(parent_slot);
                if (!old_world || !parent_world) return fail(WorldError::unrepresentable_transform);
                const auto inverse = inverse_affine(*parent_world);
                const auto local = inverse ? compose_affine(*inverse, *old_world) : std::nullopt;
                transforms[index] = local ? decompose_transform(*local) : std::nullopt;
                if (!transforms[index]) return fail(WorldError::unrepresentable_transform);
                edit(slot).local = transforms[index];
            }
            unlink(slot);
            auto& node = edit(slot).node;
            node.parent = parent_slot;
            if (parent_slot != invalid_entity_slot) {
                auto& parent = edit(parent_slot).node;
                node.next = parent.first_child;
                if (node.next != invalid_entity_slot) edit(node.next).node.previous = slot;
                parent.first_child = slot;
            }
            changed.push_back(slot);
        } else if (command.kind == Kind::add || command.kind == Kind::remove ||
                   command.kind == Kind::replace) {
            auto present = state.components.find(command.type);
            if (present == state.components.end()) {
                const auto pool = m_pools.find(command.type);
                const auto exists = pool != m_pools.end() && pool->second->contains(slot);
                present = state.components.emplace(command.type, exists).first;
            }
            if (command.kind == Kind::add) {
                if (present->second) return fail(WorldError::component_exists);
                if (command.type == typeid(TransformComponent)) {
                    const auto& value = static_cast<const WorldCommands::Addition<TransformComponent>&>(
                        *command.addition).value;
                    transforms[index] = validated_transform(value);
                    if (!transforms[index]) return fail(WorldError::invalid_transform);
                    edit(slot).local = transforms[index];
                    changed.push_back(slot);
                }
                present->second = true;
                ++additions[command.type];
                if (!m_pools.contains(command.type) && !new_pools.contains(command.type))
                    new_pools.emplace(command.type, command.addition->make_pool());
            } else if (command.kind == Kind::replace) {
                if (!present->second) return fail(WorldError::component_missing);
            } else {
                if (!present->second) return fail(WorldError::component_missing);
                if (command.type == typeid(TransformComponent)) {
                    auto& value = edit(slot);
                    if (value.node.parent != invalid_entity_slot || value.node.first_child != invalid_entity_slot)
                        return fail(WorldError::hierarchy_in_use);
                    value.local.reset();
                    changed.push_back(slot);
                }
                present->second = false;
            }
        }
    }

    // Fallible capacity preparation. Existing values may relocate, but logical state stays intact.
    detail::reserve_for(m_slots, slot_count);
    detail::reserve_for(m_spatial, slot_count);
    detail::reserve_for(m_spatial_work, slot_count);
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
    m_spatial.resize(slot_count);
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
            for (const auto descendant : destroyed[index]) destroy(descendant);
            break;
        case Kind::add:
            command.addition->publish(*m_pools.at(command.type), slot);
            break;
        case Kind::replace:
            command.addition->replace(*m_pools.at(command.type), slot);
            break;
        case Kind::remove:
            m_pools.at(command.type)->remove(slot);
            break;
        case Kind::set_transform:
        case Kind::reparent:
            break;
        }
        if (transforms[index])
            find_pool<TransformComponent>()->get().value(slot) = *transforms[index];
    }
    for (const auto& [slot, value] : spatial) m_spatial[slot] = value.node;
    for (const auto slot : changed) dirty_subtree(slot);
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
