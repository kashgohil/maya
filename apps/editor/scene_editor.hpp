#pragma once

#include "maya/properties/schema.hpp"
#include "maya/scene/scene_io.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace maya::editor {

/// One entity's authored state: parent and schema components, as saved in scene files.
struct EntityRecord {
    std::optional<EntityId> parent;
    std::vector<ComponentValue> components; // in schema order, at most one per ComponentId
};

/// Authored state of a scene: every entity's record, and the ordered children of every parent. Roots
/// are the children of the invalid EntityId{}; their order is editor state (the World has none).
struct SceneState {
    std::unordered_map<EntityId, EntityRecord, PersistentIdHash> entities;
    std::unordered_map<EntityId, std::vector<EntityId>, PersistentIdHash> children;
    const std::vector<EntityId>& children_of(EntityId parent) const;
};

/// A reversible change: the before and after states of just the entities, child lists, and material
/// assets it touched.
struct SceneChange {
    std::unordered_map<EntityId, std::optional<EntityRecord>, PersistentIdHash> before, after;
    std::unordered_map<EntityId, std::vector<EntityId>, PersistentIdHash> order_before, order_after;
    std::unordered_map<AssetId, MaterialAsset, PersistentIdHash> materials_before, materials_after;
    bool empty() const noexcept { return before.empty() && order_before.empty() && materials_before.empty(); }
};
/// Entities and child lists that differ between two states.
SceneChange diff(const SceneState& before, const SceneState& after);

struct EditResult {
    bool applied = false;
    std::string error; // why the edit was rejected; the World is unchanged
    explicit operator bool() const noexcept { return applied; }
};

enum class SelectMode { replace, toggle, add };
enum class Placement { inside, before, after }; // relative to a target entity

/// The editor's authoring session for one open scene: it owns the World, applies every edit as an
/// atomic World batch, records reversible history by persistent EntityId, and tracks selection and
/// unsaved changes. History never holds runtime handles, so undo and redo stay valid across deletion
/// and recreation. Open another scene with a new SceneEditor: history does not carry over.
class SceneEditor {
public:
    static constexpr size_t history_limit = 256;

    /// Roots listed in `root_order` (normally the scene file's order) come first, in that order; any
    /// others follow by EntityId.
    explicit SceneEditor(std::unique_ptr<World> world, const std::vector<EntityId>& root_order = {});
    SceneEditor(const SceneEditor&) = delete;
    SceneEditor& operator=(const SceneEditor&) = delete;

    const World& world() const noexcept { return *m_world; }
    /// The scene for saving: roots in display order, each followed depth-first by its descendants.
    SceneDocument document() const;
    World& world() noexcept { return *m_world; }
    /// Checks asset references in created and edited components, normally against the project's
    /// catalog. Without one, only empty references are accepted.
    void set_validation_context(PropertyValidationContext context) { m_context = std::move(context); }
    /// Where new EntityIds come from (EntityId::generate by default); tests make it deterministic.
    void set_id_source(std::function<EntityId()> source) { m_new_id = std::move(source); }
    /// Increments on every applied edit, undo, and redo.
    uint64_t revision() const noexcept { return m_revision; }

    // Hierarchy in display order.
    const std::vector<EntityId>& roots() const { return m_state.children_of({}); }
    const std::vector<EntityId>& children(EntityId parent) const { return m_state.children_of(parent); }
    const EntityRecord* record(EntityId id) const;
    std::string display_name(EntityId id) const;
    bool is_ancestor(EntityId ancestor, EntityId entity) const;

    // Selection, primary last. It never holds entities that no longer exist.
    const std::vector<EntityId>& selection() const noexcept { return m_selection; }
    std::optional<EntityId> primary() const;
    bool selected(EntityId id) const;
    void select(EntityId id, SelectMode mode = SelectMode::replace);
    void set_selection(std::vector<EntityId> ids);
    void clear_selection() { m_selection.clear(); }

    // Edits: each is one undo step, unless inside a group. Rejected edits change nothing.
    /// Creates an entity with a name, an identity transform, and optional extra components, as the last
    /// child of `parent` (or last root), and selects it.
    EditResult create(std::string name, std::optional<EntityId> parent = std::nullopt,
                      std::vector<ComponentValue> extra = {});
    EditResult rename(EntityId id, std::string name);
    /// Copies each selected subtree (skipping entities whose ancestor is also selected) with new IDs,
    /// placed right after its original, and selects the copies.
    EditResult duplicate_selection();
    /// Deletes each selected subtree and clears the selection.
    EditResult delete_selection();
    /// Copies a scene document's entities in with new IDs, after the existing roots, its roots moved by
    /// `offset` and renamed when their names are taken, and selects the copied roots: placing an
    /// imported model (docs/editor.md#importing-models).
    EditResult insert(const SceneDocument& document, std::string label, math::DVec3 offset = {});
    /// Moves `id` into, before, or after `target`, or to the end of the roots when `target` is empty,
    /// keeping its world pose. Rejects cycles, entities without transforms, and poses that the new
    /// parent cannot represent (e.g. shear from a rotated, nonuniformly scaled parent).
    EditResult move(EntityId id, std::optional<EntityId> target, Placement placement = Placement::inside);
    /// Replaces or adds a whole component value (validated), e.g. from an inspector or gizmo.
    EditResult set_component(EntityId id, ComponentValue value);
    /// Removes a component. A transform cannot be removed while the entity has a parent or children.
    EditResult remove_component(EntityId id, ComponentId component);
    bool group_open() const noexcept { return m_group_depth > 0; }

    /// Groups the edits made until the matching end_group into one undo step labelled `label`.
    /// Groups nest; only the outermost one records.
    void begin_group(std::string label);
    void end_group();

    bool can_undo() const noexcept { return m_position > 0 && m_group_depth == 0 && !locked(); }
    bool can_redo() const noexcept { return m_position < m_history.size() && m_group_depth == 0 && !locked(); }
    std::string undo_label() const { return can_undo() ? m_history[m_position - 1].label : std::string{}; }
    std::string redo_label() const { return can_redo() ? m_history[m_position].label : std::string{}; }
    EditResult undo();
    EditResult redo();
    size_t history_size() const noexcept { return m_history.size(); }

    /// While locked (e.g. while the scene plays), every edit, undo, and redo is refused with `reason`.
    /// Selection still works.
    void lock(std::string reason) { m_lock_reason = reason.empty() ? std::string("The scene is locked") : std::move(reason); }
    void unlock() noexcept { m_lock_reason.clear(); }
    bool locked() const noexcept { return !m_lock_reason.empty(); }

    /// Unsaved changes: the history position differs from the one at the last save or open, or a
    /// material differs from its file.
    bool dirty() const noexcept;
    void mark_saved() noexcept { m_saved = m_position; }

    // Material assets (docs/editor.md#materials), edited in the same history as the scene. The editor
    // holds a value for each material it has opened; edits publish it, so views show it at once, and
    // saving writes it to the material's file.
    /// Starts editing a material whose file holds `saved`, unless it is already open.
    void open_material(AssetId id, MaterialAsset saved);
    /// The material's edited value, or null if it is not open.
    const MaterialAsset* material(AssetId id) const;
    /// Replaces an open material's value (validated by the caller) as one undo step, or part of a group.
    EditResult set_material(AssetId id, MaterialAsset value, std::string label);
    /// The material's file now holds `saved` (it was reloaded or written). An unedited material takes the
    /// file's value; an edited one keeps its edit, which is published again, and stays unsaved.
    void material_file_changed(AssetId id, MaterialAsset saved);
    /// Open materials whose value differs from their file, by ID.
    std::vector<AssetId> dirty_materials() const;
    /// Called with each material value that edits, undo, and redo make current, e.g. to publish it.
    void set_material_publisher(std::function<void(AssetId, const MaterialAsset&)> publish) { m_publish_material = std::move(publish); }

private:
    struct Step {
        std::string label;
        SceneChange change;
        std::vector<EntityId> selection_before, selection_after;
    };
    EditResult commit(std::string label, SceneChange change, std::vector<EntityId> selection_after);
    EditResult apply(const SceneChange& change, bool forward);
    void record(Step step);
    void prune_selection();

    std::unique_ptr<World> m_world;
    std::function<EntityId()> m_new_id = [] { return EntityId::generate(); };
    PropertyValidationContext m_context;
    std::string m_lock_reason; // empty when unlocked
    SceneState m_state; // mirrors the World's authored data, plus root order
    std::vector<EntityId> m_selection;
    std::deque<Step> m_history;
    size_t m_position = 0; // steps before this index are applied
    std::optional<size_t> m_saved = 0; // nullopt: the saved position was discarded
    uint64_t m_revision = 0;
    int m_group_depth = 0;
    std::string m_group_label;
    SceneChange m_group_change; // merged changes of the open group
    std::vector<EntityId> m_group_selection;
    std::unordered_map<AssetId, MaterialAsset, PersistentIdHash> m_materials; // open materials, as edited
    std::unordered_map<AssetId, MaterialAsset, PersistentIdHash> m_material_files; // what their files hold
    std::function<void(AssetId, const MaterialAsset&)> m_publish_material;
};

/// Reads a World's authored state (schema components and hierarchy). Roots are ordered by EntityId.
SceneState capture_state(const World& world);
bool same_component(const ComponentValue& a, const ComponentValue& b);

} // namespace maya::editor
