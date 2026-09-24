#include "scene_editor.hpp"
#include "maya/scene/scene_io.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace maya::editor {
namespace {
using IdSet = std::unordered_set<EntityId, PersistentIdHash>;

bool same_value(const PropertyValue& a, const PropertyValue& b) {
    if (a.index() != b.index()) return false;
    return std::visit([&](const auto& left) {
        using T = std::decay_t<decltype(left)>;
        const auto& right = std::get<T>(b);
        if constexpr (std::is_same_v<T, math::Vec3>) return left.x == right.x && left.y == right.y && left.z == right.z;
        else if constexpr (std::is_same_v<T, math::Quat>)
            return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
        else return left == right;
    }, a);
}

bool same_record(const EntityRecord& a, const EntityRecord& b) {
    if (a.parent != b.parent || a.components.size() != b.components.size()) return false;
    for (size_t i = 0; i < a.components.size(); ++i)
        if (!same_component(a.components[i], b.components[i])) return false;
    return true;
}

const ComponentValue* find_component(const EntityRecord& record, ComponentId id) {
    const auto found = std::ranges::find(record.components, id, [](const ComponentValue& value) { return component_id(value); });
    return found == record.components.end() ? nullptr : &*found;
}

void put_component(EntityRecord& record, ComponentValue value) {
    const auto id = component_id(value);
    const auto found = std::ranges::find(record.components, id, [](const ComponentValue& v) { return component_id(v); });
    if (found != record.components.end()) *found = std::move(value);
    else {
        record.components.push_back(std::move(value));
        std::ranges::sort(record.components, {}, [](const ComponentValue& v) { return component_id(v); });
    }
}

void remove_component(WorldCommands& commands, EntityTarget target, ComponentId id) {
    switch (id) {
    case ComponentId::name: commands.remove<NameComponent>(target); break;
    case ComponentId::transform: commands.remove<TransformComponent>(target); break;
    case ComponentId::mesh_renderer: commands.remove<MeshRendererComponent>(target); break;
    case ComponentId::camera: commands.remove<CameraComponent>(target); break;
    case ComponentId::light: commands.remove<LightComponent>(target); break;
    }
}

std::string world_error_text(WorldError error) {
    switch (error) {
    case WorldError::hierarchy_cycle: return "an entity cannot be moved under itself or its descendants";
    case WorldError::unrepresentable_transform: return "the new parent's transform cannot represent this pose";
    case WorldError::invalid_transform: return "the transform is not valid";
    case WorldError::hierarchy_in_use: return "the entity's children need its transform";
    case WorldError::duplicate_id: return "an entity with that ID already exists";
    case WorldError::busy: return "the World is busy";
    default: return "the World rejected the change";
    }
}

/// Subtree of `root` in depth-first order.
std::vector<EntityId> subtree(const SceneState& state, EntityId root) {
    auto result = std::vector<EntityId>{};
    auto pending = std::vector<EntityId>{root};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        result.push_back(id);
        const auto& children = state.children_of(id);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

/// Selected entities that are not inside another selected entity's subtree, in selection order.
std::vector<EntityId> selection_roots(const SceneEditor& editor) {
    auto roots = std::vector<EntityId>{};
    for (const auto id : editor.selection()) {
        auto nested = false;
        for (const auto other : editor.selection())
            if (other != id && editor.is_ancestor(other, id)) nested = true;
        if (!nested) roots.push_back(id);
    }
    return roots;
}

std::string copy_name(const std::string& name, const std::unordered_set<std::string>& used) {
    // "Cube" -> "Cube (1)", "Cube (1)" -> "Cube (2)": the first free number for this base name.
    auto base = name;
    if (const auto open = name.rfind(" ("); open != std::string::npos && name.ends_with(')') &&
        open + 2 < name.size() - 1 && std::all_of(name.begin() + open + 2, name.end() - 1, [](unsigned char c) { return std::isdigit(c) != 0; }))
        base = name.substr(0, open);
    for (int n = 1;; ++n)
        if (auto candidate = base + " (" + std::to_string(n) + ")"; !used.contains(candidate)) return candidate;
}
} // namespace

const std::vector<EntityId>& SceneState::children_of(EntityId parent) const {
    static const auto none = std::vector<EntityId>{};
    const auto found = children.find(parent);
    return found == children.end() ? none : found->second;
}

bool same_component(const ComponentValue& a, const ComponentValue& b) {
    if (component_id(a) != component_id(b)) return false;
    const auto* schema = component_schema(component_id(a));
    for (const auto& property : schema->properties) {
        const auto left = read_property(a, property.id), right = read_property(b, property.id);
        if (!left || !right || !same_value(*left, *right)) return false;
    }
    return true;
}

SceneState capture_state(const World& world) {
    auto state = SceneState{};
    for (auto& entity : capture_scene(world).entities) {
        state.children[entity.parent.value_or(EntityId{})].push_back(entity.id);
        state.entities.emplace(entity.id, EntityRecord{entity.parent, std::move(entity.components)});
    }
    return state;
}

SceneChange diff(const SceneState& before, const SceneState& after) {
    auto change = SceneChange{};
    const auto note = [&](EntityId id, const EntityRecord* a, const EntityRecord* b) {
        change.before.emplace(id, a ? std::optional{*a} : std::nullopt);
        change.after.emplace(id, b ? std::optional{*b} : std::nullopt);
    };
    for (const auto& [id, record] : before.entities) {
        const auto found = after.entities.find(id);
        if (found == after.entities.end()) note(id, &record, nullptr);
        else if (!same_record(record, found->second)) note(id, &record, &found->second);
    }
    for (const auto& [id, record] : after.entities)
        if (!before.entities.contains(id)) note(id, nullptr, &record);
    auto parents = IdSet{};
    for (const auto& [parent, list] : before.children) parents.insert(parent);
    for (const auto& [parent, list] : after.children) parents.insert(parent);
    for (const auto parent : parents)
        if (before.children_of(parent) != after.children_of(parent)) {
            change.order_before.emplace(parent, before.children_of(parent));
            change.order_after.emplace(parent, after.children_of(parent));
        }
    return change;
}

SceneEditor::SceneEditor(std::unique_ptr<World> world) : m_world(std::move(world)) {
    if (!m_world) m_world = std::make_unique<World>();
    m_state = capture_state(*m_world);
}

const EntityRecord* SceneEditor::record(EntityId id) const {
    const auto found = m_state.entities.find(id);
    return found == m_state.entities.end() ? nullptr : &found->second;
}

std::string SceneEditor::display_name(EntityId id) const {
    if (const auto* entity = record(id))
        if (const auto* name = find_component(*entity, ComponentId::name))
            if (const auto& text = std::get<NameComponent>(*name).value; !text.empty()) return text;
    return "Entity";
}

bool SceneEditor::is_ancestor(EntityId ancestor, EntityId entity) const {
    for (const auto* current = record(entity); current && current->parent; current = record(*current->parent))
        if (*current->parent == ancestor) return true;
    return false;
}

std::optional<EntityId> SceneEditor::primary() const {
    return m_selection.empty() ? std::nullopt : std::optional{m_selection.back()};
}

bool SceneEditor::selected(EntityId id) const { return std::ranges::find(m_selection, id) != m_selection.end(); }

void SceneEditor::select(EntityId id, SelectMode mode) {
    if (!record(id)) return;
    const auto found = std::ranges::find(m_selection, id);
    switch (mode) {
    case SelectMode::replace: m_selection = {id}; break;
    case SelectMode::toggle:
        if (found != m_selection.end()) m_selection.erase(found);
        else m_selection.push_back(id);
        break;
    case SelectMode::add:
        if (found != m_selection.end()) m_selection.erase(found);
        m_selection.push_back(id); // becomes primary
        break;
    }
}

void SceneEditor::set_selection(std::vector<EntityId> ids) {
    m_selection = std::move(ids);
    prune_selection();
}

void SceneEditor::prune_selection() {
    auto seen = IdSet{};
    std::erase_if(m_selection, [&](EntityId id) { return !record(id) || !seen.insert(id).second; });
}

EditResult SceneEditor::apply(const SceneChange& change, bool forward) {
    const auto& records = forward ? change.after : change.before;
    const auto& orders = forward ? change.order_after : change.order_before;
    auto& world = *m_world;
    auto commands = world.commands();
    auto targets = std::unordered_map<EntityId, EntityTarget, PersistentIdHash>{};
    const auto live = [&](EntityId id) -> std::optional<EntityHandle> { return world.find(id); };
    const auto target_of = [&](EntityId id) -> EntityTarget {
        if (const auto found = targets.find(id); found != targets.end()) return found->second;
        return *live(id);
    };
    const auto current_parent = [&](EntityHandle entity) -> std::optional<EntityId> {
        const auto parent = world.parent(entity);
        return parent ? world.persistent_id(*parent) : std::nullopt;
    };
    auto destroyed = IdSet{};
    for (const auto& [id, target] : records)
        if (!target && live(id)) destroyed.insert(id);

    // 1. Detach survivors whose parent changes, and every current child of a re-ordered parent.
    auto detached = IdSet{};
    const auto detach = [&](EntityId id) {
        if (destroyed.contains(id) || detached.contains(id)) return;
        const auto handle = live(id);
        if (!handle || !current_parent(*handle)) return;
        commands.reparent(*handle, std::nullopt, ReparentPolicy::keep_local);
        detached.insert(id);
    };
    for (const auto& [id, target] : records) {
        const auto handle = live(id);
        if (target && handle && current_parent(*handle) != target->parent) detach(id);
    }
    for (const auto& [parent, list] : orders) {
        if (!parent.valid()) continue;
        if (const auto handle = live(parent))
            for (const auto child : world.children(*handle)) detach(*world.persistent_id(child));
    }
    // 2. Destroy removed entities; a removed descendant goes with its removed ancestor.
    for (const auto id : destroyed) {
        const auto parent = current_parent(*live(id));
        if (!parent || !destroyed.contains(*parent)) commands.destroy(*live(id));
    }
    // 3. Create added entities with their components.
    for (const auto& [id, target] : records) {
        if (!target || live(id)) continue;
        const auto created = commands.create(id);
        targets.emplace(id, created);
        for (auto value : target->components)
            std::visit([&](auto& component) { commands.add(created, std::move(component)); }, value);
    }
    // 4. Update components of entities that exist on both sides.
    for (const auto& [id, target] : records) {
        const auto handle = live(id);
        if (!target || !handle || targets.contains(id)) continue;
        for (const auto& schema : component_schemas()) {
            const auto now = read_component(world, *handle, schema.id);
            const auto* wanted = find_component(*target, schema.id);
            if (wanted && now && same_component(*now, *wanted)) continue;
            if (wanted) {
                std::visit([&](auto component) {
                    if (now) commands.replace(*handle, std::move(component));
                    else commands.add(*handle, std::move(component));
                }, *wanted);
            } else if (now) {
                remove_component(commands, *handle, schema.id);
            }
        }
    }
    // 5. Attach: reparenting inserts at the front, so attach each parent's children last to first.
    for (const auto& [parent, list] : orders) {
        if (!parent.valid()) continue;
        if (const auto found = records.find(parent); found != records.end() && !found->second) continue;
        for (auto child = list.rbegin(); child != list.rend(); ++child)
            commands.reparent(target_of(*child), target_of(parent), ReparentPolicy::keep_local);
    }
    const auto result = world.commit(commands);
    if (!result) return {false, world_error_text(result.error)};

    // Update the mirror for just the touched entries. Values are read back, since the World may
    // normalize them (e.g. quaternions).
    for (const auto& [id, target] : records) {
        if (!target) {
            m_state.entities.erase(id);
            continue;
        }
        auto mirrored = EntityRecord{target->parent, {}};
        const auto handle = *live(id);
        for (const auto& schema : component_schemas())
            if (auto value = read_component(world, handle, schema.id)) mirrored.components.push_back(std::move(*value));
        m_state.entities.insert_or_assign(id, std::move(mirrored));
    }
    for (const auto& [parent, list] : orders) {
        if (list.empty()) m_state.children.erase(parent);
        else m_state.children.insert_or_assign(parent, list);
    }
    ++m_revision; // callers then set the selection, which prunes missing entities
    return {true, {}};
}

void SceneEditor::record(Step step) {
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(m_position), m_history.end());
    if (m_saved && *m_saved > m_position) m_saved.reset(); // the saved state is no longer reachable
    m_history.push_back(std::move(step));
    ++m_position;
    if (m_history.size() > history_limit) {
        m_history.pop_front();
        --m_position;
        if (m_saved) {
            if (*m_saved == 0) m_saved.reset();
            else --*m_saved;
        }
    }
}

namespace {
/// Collects a change against the current state, one touched entity or child list at a time. Later
/// writes to the same entry replace earlier ones; "before" always keeps the original value.
class ChangeBuilder {
public:
    explicit ChangeBuilder(const SceneState& state) : m_state(state) {}
    const EntityRecord* record(EntityId id) const {
        if (const auto found = m_change.after.find(id); found != m_change.after.end())
            return found->second ? &*found->second : nullptr;
        const auto found = m_state.entities.find(id);
        return found == m_state.entities.end() ? nullptr : &found->second;
    }
    const std::vector<EntityId>& children(EntityId parent) const {
        if (const auto found = m_change.order_after.find(parent); found != m_change.order_after.end()) return found->second;
        return m_state.children_of(parent);
    }
    void set(EntityId id, std::optional<EntityRecord> after) {
        if (!m_change.before.contains(id)) {
            const auto found = m_state.entities.find(id);
            m_change.before.emplace(id, found == m_state.entities.end() ? std::nullopt : std::optional{found->second});
        }
        m_change.after.insert_or_assign(id, std::move(after));
    }
    void order(EntityId parent, std::vector<EntityId> after) {
        if (!m_change.order_before.contains(parent)) m_change.order_before.emplace(parent, m_state.children_of(parent));
        m_change.order_after.insert_or_assign(parent, std::move(after));
    }
    SceneChange take() { return std::move(m_change); }

private:
    const SceneState& m_state;
    SceneChange m_change;
};

/// Drops entries whose before and after are equal.
void drop_unchanged(SceneChange& change) {
    for (auto it = change.after.begin(); it != change.after.end();) {
        const auto& before = change.before.at(it->first);
        const auto same = (!before && !it->second) || (before && it->second && same_record(*before, *it->second));
        if (same) {
            change.before.erase(it->first);
            it = change.after.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = change.order_after.begin(); it != change.order_after.end();) {
        if (change.order_before.at(it->first) == it->second) {
            change.order_before.erase(it->first);
            it = change.order_after.erase(it);
        } else {
            ++it;
        }
    }
}
} // namespace

EditResult SceneEditor::commit(std::string label, SceneChange change, std::vector<EntityId> selection_after) {
    drop_unchanged(change);
    if (change.empty()) return {false, "Nothing to change"};
    const auto selection_before = m_selection;
    if (auto result = apply(change, true); !result) return result;
    set_selection(std::move(selection_after));
    if (m_group_depth > 0) {
        // Merge into the group: keep each entry's first "before" and latest "after".
        for (auto& [id, after] : change.after) {
            m_group_change.before.try_emplace(id, change.before.at(id));
            m_group_change.after.insert_or_assign(id, std::move(after));
        }
        for (auto& [parent, after] : change.order_after) {
            m_group_change.order_before.try_emplace(parent, change.order_before.at(parent));
            m_group_change.order_after.insert_or_assign(parent, std::move(after));
        }
    } else {
        record({std::move(label), std::move(change), selection_before, m_selection});
    }
    return {true, {}};
}

EditResult SceneEditor::create(std::string name, std::optional<EntityId> parent, std::vector<ComponentValue> extra) {
    if (parent && !record(*parent)) return {false, "The parent no longer exists"};
    if (parent && !find_component(*record(*parent), ComponentId::transform)) return {false, "The parent has no transform"};
    auto entity = EntityRecord{parent, {}};
    auto label = "Create " + name;
    put_component(entity, NameComponent{std::move(name)});
    put_component(entity, TransformComponent{});
    for (auto& value : extra) {
        if (auto error = validate_component(value); !error) return {false, std::string(error.message)};
        put_component(entity, std::move(value));
    }
    const auto id = m_new_id();
    auto change = ChangeBuilder(m_state);
    change.set(id, std::move(entity));
    auto siblings = change.children(parent.value_or(EntityId{}));
    siblings.push_back(id);
    change.order(parent.value_or(EntityId{}), std::move(siblings));
    return commit(std::move(label), change.take(), {id});
}

EditResult SceneEditor::rename(EntityId id, std::string name) {
    const auto* entity = record(id);
    if (!entity) return {false, "The entity no longer exists"};
    auto value = ComponentValue{NameComponent{std::move(name)}};
    if (auto error = validate_component(value); !error) return {false, std::string(error.message)};
    auto renamed = *entity;
    put_component(renamed, std::move(value));
    auto change = ChangeBuilder(m_state);
    change.set(id, std::move(renamed));
    return commit("Rename", change.take(), m_selection);
}

EditResult SceneEditor::set_component(EntityId id, ComponentValue value) {
    const auto* entity = record(id);
    if (!entity) return {false, "The entity no longer exists"};
    if (auto error = validate_component(value); !error) return {false, std::string(error.message)};
    const auto label = std::string("Edit ") + std::string(component_schema(component_id(value))->label);
    auto edited = *entity;
    put_component(edited, std::move(value));
    auto change = ChangeBuilder(m_state);
    change.set(id, std::move(edited));
    return commit(label, change.take(), m_selection);
}

EditResult SceneEditor::duplicate_selection() {
    const auto originals = selection_roots(*this);
    if (originals.empty()) return {false, "Nothing is selected"};
    auto change = ChangeBuilder(m_state);
    auto copies = std::vector<EntityId>{};
    auto names = std::unordered_set<std::string>{};
    for (const auto& [id, record] : m_state.entities)
        if (const auto* value = find_component(record, ComponentId::name)) names.insert(std::get<NameComponent>(*value).value);
    for (const auto original : originals) {
        auto remap = std::unordered_map<EntityId, EntityId, PersistentIdHash>{};
        const auto members = subtree(m_state, original);
        for (const auto id : members) remap.emplace(id, m_new_id());
        for (const auto id : members) {
            auto copy = m_state.entities.at(id);
            if (copy.parent && remap.contains(*copy.parent)) copy.parent = remap.at(*copy.parent);
            if (id == original)
                if (const auto* name = find_component(copy, ComponentId::name)) {
                    const auto fresh = copy_name(std::get<NameComponent>(*name).value, names);
                    names.insert(fresh);
                    put_component(copy, NameComponent{fresh});
                }
            change.set(remap.at(id), std::move(copy));
            // Children keep their original order inside the copy.
            if (const auto& kids = m_state.children_of(id); !kids.empty()) {
                auto mapped = std::vector<EntityId>{};
                for (const auto kid : kids) mapped.push_back(remap.at(kid));
                change.order(remap.at(id), std::move(mapped));
            }
        }
        // The copy of the root goes right after its original.
        const auto parent = m_state.entities.at(original).parent.value_or(EntityId{});
        auto siblings = change.children(parent);
        siblings.insert(std::ranges::find(siblings, original) + 1, remap.at(original));
        change.order(parent, std::move(siblings));
        copies.push_back(remap.at(original));
    }
    return commit(copies.size() == 1 ? "Duplicate " + display_name(originals.front()) : "Duplicate", change.take(), copies);
}

EditResult SceneEditor::delete_selection() {
    const auto doomed = selection_roots(*this);
    if (doomed.empty()) return {false, "Nothing is selected"};
    auto change = ChangeBuilder(m_state);
    for (const auto root : doomed) {
        for (const auto id : subtree(m_state, root)) {
            change.set(id, std::nullopt);
            if (!m_state.children_of(id).empty()) change.order(id, {});
        }
        const auto parent = m_state.entities.at(root).parent.value_or(EntityId{});
        auto siblings = change.children(parent);
        std::erase(siblings, root);
        change.order(parent, std::move(siblings));
    }
    return commit(doomed.size() == 1 ? "Delete " + display_name(doomed.front()) : "Delete", change.take(), {});
}

EditResult SceneEditor::move(EntityId id, std::optional<EntityId> anchor, Placement placement) {
    const auto* entity = record(id);
    if (!entity) return {false, "The entity no longer exists"};
    auto parent = std::optional<EntityId>{};
    if (anchor) {
        const auto* other = record(*anchor);
        if (!other) return {false, "The target no longer exists"};
        parent = placement == Placement::inside ? anchor : other->parent;
        if (*anchor == id && placement == Placement::inside) return {false, "An entity cannot be its own parent"};
    }
    if (parent && (*parent == id || is_ancestor(id, *parent)))
        return {false, "An entity cannot be moved under itself or its descendants"};
    if (parent && !find_component(*record(*parent), ComponentId::transform)) return {false, "The new parent has no transform"};
    const auto has_transform = find_component(*entity, ComponentId::transform) != nullptr;
    if (!has_transform && parent) return {false, "Only entities with a transform can have a parent"};

    auto moved = *entity;
    if (has_transform && parent != entity->parent) {
        // Keep the world pose: the new local transform is inverse(new parent world) x current world.
        const auto world_pose = m_world->world_matrix(*m_world->find(id));
        if (!world_pose) return {false, "The entity's current pose is not valid"};
        auto local = std::optional{*world_pose};
        if (parent) {
            const auto parent_pose = m_world->world_matrix(*m_world->find(*parent));
            const auto inverse = parent_pose ? inverse_affine(*parent_pose) : std::nullopt;
            local = inverse ? compose_affine(*inverse, *world_pose) : std::nullopt;
        }
        const auto transform = local ? decompose_transform(*local) : std::nullopt;
        if (!transform) return {false, "The new parent's transform cannot represent this entity's pose (it would need shear)"};
        put_component(moved, *transform);
    }
    moved.parent = parent;
    auto change = ChangeBuilder(m_state);
    change.set(id, std::move(moved));
    const auto old_key = entity->parent.value_or(EntityId{}), new_key = parent.value_or(EntityId{});
    auto old_siblings = change.children(old_key);
    std::erase(old_siblings, id);
    change.order(old_key, std::move(old_siblings));
    auto siblings = change.children(new_key);
    auto position = siblings.end();
    if (anchor && placement != Placement::inside) {
        position = std::ranges::find(siblings, *anchor);
        if (placement == Placement::after && position != siblings.end()) ++position;
    }
    siblings.insert(position, id);
    change.order(new_key, std::move(siblings));
    return commit((parent == entity->parent ? "Reorder " : "Move ") + display_name(id), change.take(), m_selection);
}

void SceneEditor::begin_group(std::string label) {
    if (m_group_depth++ == 0) {
        m_group_label = std::move(label);
        m_group_change = {};
        m_group_selection = m_selection;
    }
}

void SceneEditor::end_group() {
    if (m_group_depth == 0 || --m_group_depth > 0) return;
    auto change = std::move(m_group_change);
    m_group_change = {};
    drop_unchanged(change);
    if (!change.empty()) record({std::move(m_group_label), std::move(change), std::move(m_group_selection), m_selection});
}

EditResult SceneEditor::undo() {
    if (!can_undo()) return {false, "Nothing to undo"};
    const auto& step = m_history[m_position - 1];
    if (auto result = apply(step.change, false); !result) return result;
    --m_position;
    set_selection(step.selection_before);
    return {true, {}};
}

EditResult SceneEditor::redo() {
    if (!can_redo()) return {false, "Nothing to redo"};
    const auto& step = m_history[m_position];
    if (auto result = apply(step.change, true); !result) return result;
    ++m_position;
    set_selection(step.selection_after);
    return {true, {}};
}

} // namespace maya::editor
