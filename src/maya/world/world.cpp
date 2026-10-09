#include "maya/world/world.hpp"
#include <algorithm>
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

PendingEntity WorldCommands::create_staged(EntityId id, StageGroup stage) {
    if (stage == 0) throw std::invalid_argument("A stage group is nonzero");
    const auto pending = create(id);
    m_commands.back().stage = stage;
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

void WorldCommands::truncate(size_t size) {
    require_active();
    if (size >= m_commands.size()) return;
    m_commands.erase(m_commands.begin() + std::ptrdiff_t(size), m_commands.end());
    m_created = uint32_t(std::ranges::count(m_commands, Kind::create, &Command::kind));
}

std::vector<WorldCommands::Staged> WorldCommands::staged(size_t first) const {
    auto result = std::vector<Staged>{};
    result.reserve(m_commands.size() > first ? m_commands.size() - first : 0);
    for (auto i = first; i < m_commands.size(); ++i) {
        const auto& command = m_commands[i];
        result.push_back({command.kind, command.target, command.type,
                          command.kind == Kind::reparent ? command.parent : std::nullopt});
    }
    return result;
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

std::optional<math::Affine> World::world_matrix(EntityHandle entity) const {
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
        const auto matrix = local_pose(local);
        auto world = std::optional{matrix};
        node.rigid_ancestry = unit_scale(local.scale);
        if (node.parent != invalid_entity_slot) {
            const auto& ancestor = m_spatial[node.parent];
            world = ancestor.valid ? compose_pose(ancestor.world, matrix) : std::nullopt;
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

bool World::exists(EntityHandle entity) const noexcept {
    return entity.world == m_token && entity.slot < m_slots.size() &&
        entity.generation != 0 && m_slots[entity.slot].generation == entity.generation &&
        m_slots[entity.slot].id.valid();
}

bool World::alive(EntityHandle entity) const noexcept {
    return exists(entity) && m_slots[entity.slot].stage == 0;
}

std::optional<EntityId> World::persistent_id(EntityHandle entity) const noexcept {
    if (!alive(entity)) return std::nullopt;
    return m_slots[entity.slot].id;
}

std::optional<EntityHandle> World::find(EntityId id) const {
    const auto it = m_ids.find(id);
    if (it == m_ids.end() || m_slots[it->second.slot].stage != 0) return std::nullopt; // staged: not yet visible
    return it->second;
}

std::vector<EntityHandle> World::staged(StageGroup group) const {
    auto result = std::vector<EntityHandle>{};
    if (const auto found = m_stages.find(group); found != m_stages.end())
        for (const auto slot : found->second) result.push_back(handle(slot));
    return result;
}

std::vector<EntityHandle> World::publish(StageGroup group) {
    if (m_committing || m_borrows != 0) throw std::logic_error("World::publish during publication or a borrow");
    const auto found = m_stages.find(group);
    if (found == m_stages.end()) return {};
    auto result = std::vector<EntityHandle>{};
    result.reserve(found->second.size());
    detail::reserve_for(m_live, m_live.size() + found->second.size());
    // Nothing allocates from here: the group becomes visible in one step.
    for (const auto slot : found->second) {
        auto& entry = m_slots[slot];
        entry.stage = 0;
        entry.dense = static_cast<uint32_t>(m_live.size());
        m_live.push_back(slot);
        result.push_back(handle(slot));
    }
    m_staged -= found->second.size();
    m_stages.erase(found);
    ++m_names_revision;
    return result;
}

WorldError World::stage(std::span<const EntityHandle> entities, StageGroup group) {
    if (m_committing || m_borrows != 0) return WorldError::busy;
    if (group == 0) return WorldError::invalid_policy;
    auto& staged = m_stages[group]; // allocations first: nothing changes after a failure
    staged.reserve(staged.size() + entities.size());
    auto marked = std::vector<uint32_t>{};
    marked.reserve(entities.size());
    // Each entity is marked with the group as it is checked, so the hierarchy check needs no set; a refusal
    // takes the marks off again.
    const auto refuse = [&](WorldError error) {
        for (const auto slot : marked) m_slots[slot].stage = 0;
        if (staged.empty()) m_stages.erase(group);
        return error;
    };
    for (const auto entity : entities) {
        if (exists(entity) && m_slots[entity.slot].stage == group) continue; // listed twice
        if (!alive(entity)) return refuse(WorldError::invalid_entity);
        m_slots[entity.slot].stage = group;
        marked.push_back(entity.slot);
    }
    for (const auto slot : marked) { // whole hierarchies only
        const auto& node = m_spatial[slot];
        if (node.parent != invalid_entity_slot && m_slots[node.parent].stage != group) return refuse(WorldError::stage_mismatch);
        for (auto child = node.first_child; child != invalid_entity_slot; child = m_spatial[child].next)
            if (m_slots[child].stage != group) return refuse(WorldError::stage_mismatch);
    }
    for (const auto slot : marked) {
        auto& entry = m_slots[slot];
        const auto moved_slot = m_live.back();
        m_live[entry.dense] = moved_slot;
        m_slots[moved_slot].dense = entry.dense;
        m_live.pop_back();
        entry.dense = invalid_entity_slot;
        staged.push_back(slot);
        ++m_staged;
    }
    ++m_names_revision;
    return WorldError::none;
}

void World::reserve(size_t entities) {
    if (m_committing || m_borrows != 0) throw std::logic_error("World::reserve during publication or a borrow");
    detail::reserve_for(m_slots, entities);
    detail::reserve_for(m_spatial, entities);
    detail::reserve_for(m_spatial_work, entities);
    detail::reserve_for(m_live, entities);
    if (double(entities) > double(m_ids.bucket_count()) * m_ids.max_load_factor()) m_ids.reserve(entities);
    // Each component's storage in proportion to the entities that have it now.
    const auto present = std::max<size_t>(1, m_live.size() + m_staged);
    for (auto& [type, pool] : m_pools)
        pool->reserve(entities, size_t(double(pool->size()) * double(entities) / double(present)));
}

size_t World::discard(StageGroup group, size_t limit) {
    if (m_committing || m_borrows != 0) throw std::logic_error("World::discard during publication or a borrow");
    const auto found = m_stages.find(group);
    if (found == m_stages.end()) return 0;
    auto& slots = found->second;
    const auto count = std::min(limit, slots.size());
    for (size_t i = 0; i < count; ++i) {
        const auto slot = slots.back();
        slots.pop_back();
        --m_staged;
        m_slots[slot].stage = 0; // out of its group already; destroy() treats it as hidden
        m_slots[slot].dense = invalid_entity_slot;
        m_spatial[slot] = SpatialNode{}; // hidden relatives never read it again
        m_ids.erase(m_slots[slot].id);
        m_slots[slot].id = {};
        auto& entity = m_slots[slot];
        if (entity.generation != std::numeric_limits<uint64_t>::max()) {
            ++entity.generation;
            entity.next_free = m_free;
            m_free = slot;
        }
        for (auto& [type, pool] : m_pools) pool->remove(slot);
    }
    const auto left = slots.size();
    if (left == 0) m_stages.erase(found);
    return left;
}

WorldCommitResult World::commit(WorldCommands& commands) {
    if (m_committing || m_borrows != 0) return {WorldError::busy, 0, {}, {}};
    if (commands.m_world != m_token) return {WorldError::wrong_world, 0, {}, {}};
    struct PublicationGuard {
        bool& active;
        explicit PublicationGuard(bool& flag) : active(flag) { active = true; }
        ~PublicationGuard() { active = false; }
    } guard(m_committing);

    using Kind = WorldCommands::Kind;
    struct VirtualEntity {
        bool alive = true;
        std::vector<std::pair<std::type_index, bool>> components; // an entity has few: a list, not a table
    };
    // The batch's scratch tables are sized once: growing them entity by entity cost most of a big commit (#1064).
    auto touched = std::unordered_map<uint32_t, VirtualEntity>{};
    touched.reserve(commands.m_commands.size());
    auto additions = std::unordered_map<std::type_index, size_t>{};
    auto new_pools = decltype(m_pools){};
    auto new_ids = decltype(m_ids){};
    new_ids.reserve(commands.m_created);
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
    spatial.reserve(commands.m_commands.size());
    auto transforms = std::vector<std::optional<TransformComponent>>(commands.m_commands.size());
    auto destroyed = std::vector<std::vector<uint32_t>>(commands.m_commands.size());
    auto changed = std::vector<uint32_t>{};
    auto new_stages = std::unordered_map<uint32_t, StageGroup>{}; // created slots' groups
    auto staging = std::unordered_map<StageGroup, size_t>{}; // entities created into each group
    auto destroyed_count = size_t{0};
    const auto stage_of = [&](uint32_t slot) {
        if (const auto found = new_stages.find(slot); found != new_stages.end()) return found->second;
        return slot < m_slots.size() ? m_slots[slot].stage : StageGroup{0};
    };
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
    const auto staged_matrix = [&](uint32_t slot) -> std::optional<math::Affine> {
        auto path = std::vector<uint32_t>{};
        for (auto current = slot; current != invalid_entity_slot; current = edit(current).node.parent)
            path.push_back(current);
        auto matrix = math::Affine{};
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            const auto& local = edit(*it).local;
            if (!local) return std::nullopt;
            auto composed = compose_pose(matrix, local_pose(*local));
            if (!composed) return std::nullopt;
            matrix = *composed;
        }
        return matrix;
    };
    const auto resolve_target = [&](const EntityTarget& target, uint32_t& slot) -> WorldError {
        if (const auto entity = std::get_if<EntityHandle>(&target)) {
            if (entity->world != m_token) return WorldError::wrong_world;
            if (!exists(*entity)) return WorldError::invalid_entity; // staged entities too

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
        const auto fail = [index](WorldError error) { return WorldCommitResult{error, index, {}, {}}; };
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
            if (command.stage != 0) {
                new_stages[slot] = command.stage;
                ++staging[command.stage];
            }
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
            destroyed_count += subtree.size();
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
            // A staged entity's hierarchy stays inside its group: a cell's content never hangs from the rest.
            if (parent_slot != invalid_entity_slot && stage_of(parent_slot) != stage_of(slot)) return fail(WorldError::stage_mismatch);
            for (auto ancestor = parent_slot; ancestor != invalid_entity_slot;
                 ancestor = edit(ancestor).node.parent)
                if (ancestor == slot) return fail(WorldError::hierarchy_cycle);
            if (parent_slot == edit(slot).node.parent) continue;
            if (command.policy == ReparentPolicy::keep_world) {
                const auto old_world = staged_matrix(slot);
                const auto parent_world = staged_matrix(parent_slot);
                if (!old_world || !parent_world) return fail(WorldError::unrepresentable_transform);
                const auto inverse = inverse_pose(*parent_world);
                const auto local = inverse ? compose_pose(*inverse, *old_world) : std::nullopt;
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
            auto present = std::ranges::find(state.components, command.type, &std::pair<std::type_index, bool>::first);
            if (present == state.components.end()) {
                const auto pool = m_pools.find(command.type);
                const auto exists = pool != m_pools.end() && pool->second->contains(slot);
                if (state.components.empty()) state.components.reserve(4);
                state.components.emplace_back(command.type, exists);
                present = std::prev(state.components.end());
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
    result.destroyed.reserve(destroyed_count);
    if (m_journal_on) detail::reserve_for(m_journal, m_journal.size() + changed.size());
    if (m_edits_on) detail::reserve_for(m_edits, m_edits.size() + commands.m_commands.size() + destroyed_count);
    for (const auto& [group, count] : staging) {
        auto& slots = m_stages[group];
        slots.reserve(slots.size() + count);
    }
    const auto prepare_map = [](auto& map, size_t incoming) {
        const auto required = map.size() + incoming;
        // Grown geometrically, as reserve_for grows vectors: an exact reserve rehashed every entity each
        // time a commit passed the last size (#1064: 256-entity cell chunks took 10 ms in a 60,000-entity World).
        if (static_cast<double>(required) >
            static_cast<double>(map.bucket_count()) * map.max_load_factor())
            map.reserve(std::max(required, map.size() + map.size() / 2));
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
    auto renamed = false; // names or hierarchy changed: names_revision()
    for (size_t index = 0; index < commands.m_commands.size(); ++index) {
        auto& command = commands.m_commands[index];
        const auto slot = resolved[index];
        renamed |= command.kind == Kind::create || command.kind == Kind::destroy || command.kind == Kind::reparent ||
                   (command.kind != Kind::set_transform && command.type == typeid(NameComponent));
        switch (command.kind) {
        case Kind::create:
            m_slots[slot].id = command.id;
            m_slots[slot].next_free = invalid_entity_slot;
            m_slots[slot].stage = command.stage;
            m_slots[slot].origin = command.stage;
            if (command.stage != 0) { // staged: not visible until its group is published
                m_slots[slot].dense = invalid_entity_slot;
                m_stages[command.stage].push_back(slot);
                ++m_staged;
            } else {
                m_slots[slot].dense = static_cast<uint32_t>(m_live.size());
                m_live.push_back(slot);
            }
            break;
        case Kind::destroy:
            for (const auto descendant : destroyed[index]) {
                result.destroyed.push_back(handle(descendant));
                if (m_edits_on && m_slots[descendant].stage == 0 && m_slots[descendant].origin != 0)
                    m_edits.push_back({m_slots[descendant].id, m_slots[descendant].origin});
                destroy(descendant);
            }
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
        if (m_edits_on && command.kind != Kind::create && command.kind != Kind::destroy && m_slots[slot].id.valid() &&
            m_slots[slot].stage == 0 && m_slots[slot].origin != 0)
            m_edits.push_back({m_slots[slot].id, m_slots[slot].origin});
    }
    for (const auto& [slot, value] : spatial) m_spatial[slot] = value.node;
    for (const auto slot : changed) dirty_subtree(slot);
    if (m_journal_on)
        for (const auto slot : changed)
            if (m_slots[slot].id.valid()) m_journal.push_back(handle(slot));
    if (renamed) ++m_names_revision;
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
    if (entity.stage != 0) { // a staged entity leaves its group, not the visible list
        if (const auto found = m_stages.find(entity.stage); found != m_stages.end()) {
            auto& slots = found->second;
            if (const auto at = std::ranges::find(slots, slot); at != slots.end()) {
                *at = slots.back();
                slots.pop_back();
                --m_staged;
            }
        }
        entity.stage = 0;
    } else {
        const auto moved_slot = m_live.back();
        m_live[entity.dense] = moved_slot;
        m_slots[moved_slot].dense = entity.dense;
        m_live.pop_back();
    }
    entity.dense = invalid_entity_slot;
    // Retire exhausted slots. Never allow an ancient handle to become valid after wrap.
    if (entity.generation != std::numeric_limits<uint64_t>::max()) {
        ++entity.generation;
        entity.next_free = m_free;
        m_free = slot;
    }
    for (auto& [type, pool] : m_pools) pool->remove(slot);
}

const char* error_name(WorldError error) noexcept {
    switch (error) {
    case WorldError::none: return "none";
    case WorldError::busy: return "busy";
    case WorldError::wrong_world: return "wrong world";
    case WorldError::invalid_entity: return "invalid entity";
    case WorldError::invalid_id: return "invalid ID";
    case WorldError::duplicate_id: return "duplicate ID";
    case WorldError::invalid_pending_entity: return "invalid pending entity";
    case WorldError::component_exists: return "component exists";
    case WorldError::component_missing: return "component missing";
    case WorldError::capacity_exhausted: return "capacity exhausted";
    case WorldError::invalid_transform: return "invalid transform";
    case WorldError::hierarchy_cycle: return "hierarchy cycle";
    case WorldError::hierarchy_in_use: return "hierarchy in use";
    case WorldError::unrepresentable_transform: return "unrepresentable transform";
    case WorldError::invalid_policy: return "invalid policy";
    case WorldError::stage_mismatch: return "stage mismatch";
    }
    return "unknown";
}

} // namespace maya
