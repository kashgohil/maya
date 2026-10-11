#pragma once
// Editing a world's cells (#1067, docs/editor.md#worlds). The editor holds a world as one SceneEditor: the
// persistent part, always there, and the cells near the editor camera or kept by hand, which come and go
// outside the history. A cell is whatever roots stand in it (their hierarchies with them); one with unsaved
// edits stays loaded until it is saved. Saving writes the persistent part and the changed cells only.

#include "scene_editor.hpp"
#include "maya/jobs/jobs.hpp"
#include "maya/streaming/scatter_streamer.hpp"
#include "maya/streaming/world_streamer.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace maya::editor {

/// Where a root and its hierarchy belong: a cell, or the persistent part (nullopt).
using WorldPart = std::optional<CellIndex>;
std::string part_name(const WorldPart& part); // "persistent" or "3_-4"

enum class EditorCellState : uint8_t { unloaded, loading, active, failed };
inline constexpr size_t editor_cell_state_count = 4;
const char* editor_cell_state_name(EditorCellState state) noexcept;
/// What was decided by hand: kept loaded (locked), kept out, or neither (the camera decides).
enum class CellHold : uint8_t { none, loaded, unloaded };

struct EditorCellsSettings {
    double load_radius = 256.0; // metres from the camera to a cell's square
    double hysteresis = 64.0;
    bool follow = true; // cells come and go with the camera
    std::chrono::microseconds frame_budget{2000}; // owner-thread time a frame spends putting cells in and taking them out
    size_t chunk_entities = 256; // most entities committed at once
};
/// The project's editor radius (editor_stream_load) and hysteresis over the defaults.
EditorCellsSettings project_editor_cells_settings(const ProjectSettings& project);

struct EditorCellStats {
    std::array<size_t, editor_cell_state_count> cells{}; // by state
    size_t changed = 0, held = 0, loads_in_flight = 0;
    double frame_ms = 0.0, longest_frame_ms = 0.0; // owner-thread work of the last update, and the longest
    double last_activation_ms = 0.0, longest_activation_ms = 0.0; // a cell's owner-thread time going in, over its frames
    size_t bytes_loaded = 0; // the loaded cells' cooked bytes
    std::string last_error;
};

/// An entity anywhere in the world, loaded or not: where it is and what it is called.
struct IndexedEntity {
    WorldPart part;
    std::string name;
};

class EditorCells {
public:
    /// The world `file`, whose persistent part `scene` already holds. Cells load through `loader` on jobs.
    EditorCells(SceneEditor& scene, std::filesystem::path file, WorldDocument document, CellLoader loader,
                PropertyValidationContext context, EditorCellsSettings settings = {});
    /// Cancels its loads and waits for them; the scene keeps what is in it.
    ~EditorCells();
    EditorCells(const EditorCells&) = delete;
    EditorCells& operator=(const EditorCells&) = delete;

    void set_source(const math::DVec3& camera) { m_source = camera; }
    EditorCellsSettings& settings() noexcept { return m_settings; }
    /// One frame: applies finished loads, puts cells in and takes them out within the budget, and brings
    /// changed cells up to date.
    const EditorCellStats& update();
    /// Everything wanted now loaded and everything unwanted gone, waiting for loads: a loading screen, tests.
    const EditorCellStats& settle();
    /// Loads a cell now, waiting for it (undo touching it, an entity moved into it, a reference followed into
    /// it). False, with stats().last_error, when it cannot.
    bool load_now(CellIndex cell);
    /// Loads every cell the change touches, in either direction: what an undo or redo needs first.
    bool load_for(const SceneChange& change);
    /// By hand: keep a cell loaded (locked), keep it out, or let the camera decide. A changed cell cannot
    /// be kept out; the reason is returned.
    std::string hold(CellIndex cell, CellHold hold);
    void release_holds();

    EditorCellState state(CellIndex cell) const;
    CellHold held(CellIndex cell) const;
    bool changed(CellIndex cell) const;
    bool persistent_changed() const noexcept { return m_persistent_changed; }
    /// Cells with unsaved edits, by index.
    std::vector<CellIndex> changed_cells() const;
    /// Cells known: the world's, and any edits have put roots in. Unloaded ones are listed too.
    std::vector<CellIndex> cells() const;
    /// The part an entity is in: from the scene for loaded ones, from the index for the rest.
    const IndexedEntity* find(EntityId id) const;
    /// Every unloaded entity the index knows in `cell` (once the world's cells have been read).
    std::vector<std::pair<EntityId, std::string>> unloaded_entities(CellIndex cell) const;
    bool index_ready() const noexcept { return m_index_ready; }
    /// A root's part, by the rules saving uses.
    WorldPart part_of_root(EntityId root) const;
    /// The roots of a part, in the scene's order.
    std::vector<EntityId> roots(const WorldPart& part) const;

    /// Writes the persistent part and every changed cell (docs/editor.md#worlds), then the world file.
    SceneDiagnostics save();
    /// The world as the editor has it now, for Play: the world file with changed cells' counts, the
    /// persistent part, and a loader that gives changed cells from the editor and the rest from disk.
    struct PlayWorld {
        WorldDocument document;
        SceneDocument persistent;
        CellLoader loader;
    };
    PlayWorld play_world(CellLoader disk) const;

    const WorldDocument& document() const noexcept { return m_document; }
    const std::filesystem::path& file() const noexcept { return m_file; }
    const EditorCellStats& stats() const noexcept { return m_stats; }
    void reset_peaks() noexcept {
        m_stats.longest_frame_ms = 0.0;
        m_stats.longest_activation_ms = 0.0;
    }
    std::optional<math::DVec3> source() const noexcept { return m_source; }

private:
    struct Cell {
        EditorCellState state = EditorCellState::unloaded;
        CellHold hold = CellHold::none;
        bool changed = false;
        JobHandle job;
        std::shared_ptr<CellLoad> result;
        std::vector<SceneEntity> pending; // parents first
        size_t next = 0;
        StageGroup group = 0;
        double activation_ms = 0.0;
        size_t bytes = 0;
        std::string saved; // its text as saved (or loaded); empty for a cell not on disk
        std::string error;
    };
    Cell& cell(CellIndex index);
    bool wanted(CellIndex index, const Cell& cell) const;
    void start(CellIndex index, Cell& cell, JobTier tier);
    bool stage_step(CellIndex index, Cell& cell, std::chrono::steady_clock::time_point deadline);
    void finish_load(CellIndex index, Cell& cell);
    void unload(CellIndex index, Cell& cell);
    void fail(CellIndex index, Cell& cell, std::string error);
    void changed_by(const SceneChange& change);
    void place(EntityId root);
    void refresh();
    std::string part_text(const WorldPart& part) const;
    SceneDocument part_document(const WorldPart& part) const;
    SceneDocument roots_document(const std::vector<EntityId>& roots_in_order) const;
    std::string roots_text(const std::vector<EntityId>& roots_in_order) const;
    std::string check(const SceneChange& change) const;
    EntityId root_of(EntityId id) const;
    void count();

    SceneEditor& m_scene;
    std::filesystem::path m_file;
    WorldDocument m_document;
    CellLoader m_loader;
    PropertyValidationContext m_context;
    EditorCellsSettings m_settings;
    std::optional<math::DVec3> m_source;
    std::map<CellIndex, Cell> m_cells;
    std::unordered_map<EntityId, WorldPart, PersistentIdHash> m_root_part; // loaded roots
    std::map<WorldPart, std::set<EntityId>> m_part_roots;
    std::unordered_map<EntityId, IndexedEntity, PersistentIdHash> m_index; // every entity known, loaded or not
    std::set<WorldPart> m_touched; // parts whose text may have changed since the last refresh
    std::set<CellIndex> m_load_now; // cells an edit put a root into while unloaded
    std::string m_persistent_saved;
    bool m_persistent_changed = false;
    bool m_index_ready = false;
    JobHandle m_index_job;
    size_t m_index_merged = 0; // cells of the read index merged so far
    std::shared_ptr<std::vector<std::pair<CellIndex, std::vector<std::pair<EntityId, std::string>>>>> m_index_result;
    StageGroup m_next_group = 0x6d617961; // apart from any other owner's
    double m_stage_us = 1.0, m_finish_us = 1.0, m_unload_us = 0.5; // measured cost per entity staged, finished, and unloaded, as frames plan
    bool m_frame_worked = false; // this update has done a step: further ones must fit the budget
    EditorCellStats m_stats;
    JobScope m_jobs;
};

/// Scatter's items into the editor's scene (#1093, docs/editor.md#generated-content): transient, outside the
/// history, so they are drawn and picked but never saved, until an edit makes one an override.
class EditorScatterSink final : public ScatterSink {
public:
    explicit EditorScatterSink(SceneEditor& scene) : m_scene(scene) {}
    std::string stage(StageGroup group, std::span<const SceneEntity> items) override {
        const auto staged = m_scene.stage_entities(group, items, true);
        return staged ? std::string{} : staged.error;
    }
    std::string reveal(StageGroup group) override {
        const auto revealed = m_scene.reveal(group);
        return revealed ? std::string{} : revealed.error;
    }
    void discard(StageGroup group) override { m_scene.discard_staged(group); }
    void remove(std::span<const EntityId> items) override {
        auto transient = std::vector<EntityId>{};
        for (const auto id : items)
            if (m_scene.transient(id)) transient.push_back(id); // an edited one is authored now: not the streamer's
        m_scene.unload(transient);
    }
    bool exists(EntityId id) const override { return m_scene.record(id) != nullptr; }

private:
    SceneEditor& m_scene;
};

} // namespace maya::editor
