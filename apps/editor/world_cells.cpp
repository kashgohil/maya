#include "world_cells.hpp"
#include "maya/assets/project.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace maya::editor {
namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

/// Metres from a point to a cell's square, across x and z.
double distance_to(const math::DVec3& point, CellIndex cell, double size) {
    const auto x0 = cell.x * size, z0 = cell.z * size;
    const auto dx = std::max({x0 - point.x, 0.0, point.x - (x0 + size)});
    const auto dz = std::max({z0 - point.z, 0.0, point.z - (z0 + size)});
    return std::sqrt(dx * dx + dz * dz);
}

/// A cell's entities with every parent before its children, otherwise in their order.
std::vector<SceneEntity> parents_first(std::vector<SceneEntity> entities) {
    auto at = std::unordered_map<EntityId, size_t, PersistentIdHash>{};
    for (size_t i = 0; i < entities.size(); ++i) at.emplace(entities[i].id, i);
    auto done = std::vector<bool>(entities.size(), false);
    auto result = std::vector<SceneEntity>{};
    result.reserve(entities.size());
    const auto emit = [&](auto&& self, size_t i) -> void {
        if (done[i]) return;
        done[i] = true; // a cycle cannot recurse forever; the World refuses it later
        if (entities[i].parent)
            if (const auto parent = at.find(*entities[i].parent); parent != at.end()) self(self, parent->second);
        result.push_back(std::move(entities[i]));
    };
    for (size_t i = 0; i < entities.size(); ++i) emit(emit, i);
    return result;
}

std::string name_of(const SceneEntity& entity) {
    for (const auto& value : entity.components)
        if (const auto* name = std::get_if<NameComponent>(&value)) return name->value.empty() ? std::string("Entity") : name->value;
    return "Entity";
}
} // namespace

std::string part_name(const WorldPart& part) { return part ? cell_name(*part) : std::string("persistent"); }

const char* editor_cell_state_name(EditorCellState state) noexcept {
    switch (state) {
    case EditorCellState::unloaded: return "unloaded";
    case EditorCellState::loading: return "loading";
    case EditorCellState::active: return "active";
    case EditorCellState::failed: return "failed";
    }
    return "unknown";
}

EditorCellsSettings project_editor_cells_settings(const ProjectSettings& project) {
    auto settings = EditorCellsSettings{};
    if (project.editor_stream_load) settings.load_radius = *project.editor_stream_load;
    if (project.stream_hysteresis) settings.hysteresis = *project.stream_hysteresis;
    return settings;
}

EditorCells::EditorCells(SceneEditor& scene, std::filesystem::path file, WorldDocument document, CellLoader loader,
                         PropertyValidationContext context, EditorCellsSettings settings)
    : m_scene(scene), m_file(std::move(file)), m_document(std::move(document)), m_loader(std::move(loader)), m_context(std::move(context)),
      m_settings(settings) {
    for (const auto& listed : m_document.cells) m_cells[listed.index];
    // The persistent part as its file holds it; anything in it that belongs to a cell makes both changed.
    {
        auto out = std::ostringstream{};
        write_scene(out, m_scene.document(), m_context);
        m_persistent_saved = out.str();
    }
    for (const auto root : m_scene.roots()) place(root);
    refresh();
    m_scene.set_change_listener(this, [this](const SceneChange& change, bool) { changed_by(change); });
    m_scene.set_change_check([this](const SceneChange& change) { return check(change); });
    // Every cell read once on a background job: where each entity is, for references and undo.
    m_index_result = std::make_shared<std::vector<std::pair<CellIndex, std::vector<std::pair<EntityId, std::string>>>>>();
    m_index_job = m_jobs.submit(JobTier::background, [cells = m_document.cells, loader = m_loader, result = m_index_result](JobContext& job) {
        for (const auto& listed : cells) {
            if (job.cancelled()) return;
            auto loaded = loader(listed, job);
            if (!loaded.document) continue;
            auto entities = std::vector<std::pair<EntityId, std::string>>{};
            entities.reserve(loaded.document->entities.size());
            for (const auto& entity : loaded.document->entities) entities.emplace_back(entity.id, name_of(entity));
            result->emplace_back(listed.index, std::move(entities));
        }
    });
    count();
}

EditorCells::~EditorCells() {
    m_jobs.cancel();
    m_jobs.wait();
    for (auto& [index, cell] : m_cells)
        if (cell.group) m_scene.discard_staged(cell.group);
    m_scene.set_change_listener(this, {});
    m_scene.set_change_check({});
}

EditorCells::Cell& EditorCells::cell(CellIndex index) {
    const auto [found, added] = m_cells.try_emplace(index);
    if (added) found->second.state = EditorCellState::active; // new to the world: nothing on disk to load
    return found->second;
}

bool EditorCells::wanted(CellIndex index, const Cell& cell) const {
    if (cell.changed) return true;
    if (cell.hold == CellHold::loaded) return true;
    if (cell.hold == CellHold::unloaded) return false;
    const auto in = cell.state == EditorCellState::active || cell.state == EditorCellState::loading;
    if (!m_settings.follow || !m_source) return in;
    const auto distance = distance_to(*m_source, index, m_document.cell_size);
    return distance <= m_settings.load_radius || (in && distance <= m_settings.load_radius + m_settings.hysteresis);
}

void EditorCells::start(CellIndex index, Cell& cell, JobTier tier) {
    const auto* listed = m_document.cell(index);
    if (!listed) {
        cell.state = EditorCellState::active;
        return;
    }
    cell.state = EditorCellState::loading;
    cell.error.clear();
    cell.result = std::make_shared<CellLoad>();
    cell.job = m_jobs.submit(tier, [loader = m_loader, listed = *listed, result = cell.result](JobContext& job) { *result = loader(listed, job); });
}

void EditorCells::fail(CellIndex index, Cell& cell, std::string error) {
    if (cell.group) m_scene.discard_staged(cell.group);
    cell.group = 0;
    cell.pending.clear();
    cell.next = 0;
    cell.result.reset();
    cell.job = {};
    cell.state = EditorCellState::failed;
    cell.error = "Cell " + cell_name(index) + ": " + std::move(error);
    m_stats.last_error = cell.error;
}

bool EditorCells::stage_step(CellIndex index, Cell& cell, Clock::time_point deadline) {
    if (!cell.group) cell.group = m_next_group++;
    while (cell.next < cell.pending.size()) {
        const auto count = std::min(m_settings.chunk_entities, cell.pending.size() - cell.next);
        // Only what fits: each entity's measured cost, so a chunk started late does not overrun the frame. The
        // frame's first step always goes, so a slow machine (or build) still makes progress.
        if (m_frame_worked && Clock::now() + std::chrono::duration<double, std::micro>(m_stage_us * double(count)) > deadline)
            return false;
        const auto start = Clock::now();
        const auto staged = m_scene.stage_entities(cell.group, std::span(cell.pending).subspan(cell.next, count));
        if (!staged) {
            fail(index, cell, staged.error);
            return true;
        }
        cell.next += count;
        m_frame_worked = true;
        const auto took = ms_since(start);
        cell.activation_ms += took;
        m_stage_us = 0.8 * m_stage_us + 0.2 * (took * 1000.0 / double(count));
    }
    // Then shown, placed, and its saved text taken, in one step: when it fits too.
    const auto entities = double(std::max<size_t>(cell.pending.size(), 1));
    if (m_frame_worked && Clock::now() + std::chrono::duration<double, std::micro>(m_finish_us * entities) > deadline) return false;
    const auto start = Clock::now();
    finish_load(index, cell);
    m_frame_worked = true;
    const auto took = ms_since(start);
    cell.activation_ms += took;
    m_finish_us = 0.8 * m_finish_us + 0.2 * (took * 1000.0 / entities);
    m_stats.last_activation_ms = cell.activation_ms;
    m_stats.longest_activation_ms = std::max(m_stats.longest_activation_ms, cell.activation_ms);
    return true;
}

void EditorCells::finish_load(CellIndex index, Cell& cell) {
    if (cell.group) {
        if (const auto revealed = m_scene.reveal(cell.group); !revealed) {
            fail(index, cell, revealed.error);
            return;
        }
        cell.group = 0;
    }
    cell.state = EditorCellState::active;
    auto misplaced = false;
    auto from_file = std::set<EntityId>{};
    for (const auto& entity : cell.pending)
        if (!entity.parent) {
            place(entity.id);
            from_file.insert(entity.id);
            misplaced = misplaced || part_of_root(entity.id) != WorldPart(index);
        }
    cell.pending.clear();
    cell.next = 0;
    // What saving would leave as it is: the file's roots alone, in the scene's order. Roots an edit put here
    // while it was unloaded, or the file's roots that stand elsewhere, make it changed.
    auto here = roots(index);
    auto saved = here;
    std::erase_if(saved, [&](EntityId root) { return !from_file.contains(root); });
    cell.saved = misplaced ? std::string("\x01 misplaced") : roots_text(saved);
    cell.changed = misplaced || saved.size() != here.size(); // no text to compare: only what came from elsewhere differs
    m_touched.erase(WorldPart(index));
}

void EditorCells::unload(CellIndex index, Cell& cell) {
    const auto found = m_part_roots.find(index);
    if (found != m_part_roots.end()) {
        const auto roots = std::vector<EntityId>(found->second.begin(), found->second.end());
        if (const auto removed = m_scene.unload(roots); !removed) {
            m_stats.last_error = "Cell " + cell_name(index) + ": " + removed.error;
            return;
        }
        for (const auto root : roots) m_root_part.erase(root);
        m_part_roots.erase(found);
    }
    m_touched.erase(WorldPart(index));
    cell.state = EditorCellState::unloaded;
    cell.bytes = 0;
    cell.activation_ms = 0.0;
}

const EditorCellStats& EditorCells::update() {
    const auto began = Clock::now();
    const auto deadline = began + m_settings.frame_budget;
    m_frame_worked = false;
    // The index, once read: entities not loaded are known from it. A big world's is merged over several
    // frames, a few cells at a time, so no frame takes it all.
    if (m_index_job && m_index_job.done()) {
        auto& read = *m_index_result;
        const auto merge_until = began + std::chrono::microseconds(500);
        while (m_index_merged < read.size() && Clock::now() < merge_until) {
            auto& [index, entities] = read[m_index_merged++];
            for (auto& [id, name] : entities) m_index.try_emplace(id, IndexedEntity{index, std::move(name)});
        }
        if (m_index_merged == read.size()) {
            // What is left of it (a big world's hundreds of thousands of names) is freed on a job, not here.
            m_jobs.submit(JobTier::background, [left = std::move(read)](JobContext&) {});
            read.clear();
            m_index_job = {};
            m_index_ready = true;
        }
    }
    // Cells edits put roots into while unloaded, now: saving must have their other content.
    for (const auto index : std::exchange(m_load_now, {})) load_now(index);
    refresh();
    for (auto& [index, cell] : m_cells) {
        const auto want = wanted(index, cell);
        switch (cell.state) {
        case EditorCellState::unloaded:
            if (want) start(index, cell, JobTier::background);
            break;
        case EditorCellState::failed:
            if (!want) cell.state = EditorCellState::unloaded; // retried when wanted again
            break;
        case EditorCellState::loading:
            if (!want) {
                if (cell.job) cell.job.cancel();
                if (cell.group) m_scene.discard_staged(cell.group);
                cell = Cell{.hold = cell.hold, .saved = std::move(cell.saved)};
                break;
            }
            if (cell.job && cell.job.done()) {
                auto result = std::move(cell.result);
                cell.job = {};
                if (!result || !result->document) {
                    fail(index, cell, result && !result->error.empty() ? result->error : "it did not load");
                    break;
                }
                cell.bytes = result->bytes;
                cell.pending = parents_first(std::move(result->document->entities));
                cell.activation_ms = 0.0;
            }
            break;
        case EditorCellState::active:
            if (!want && !cell.changed) {
                const auto entities = m_part_roots.contains(WorldPart(index)) ? m_part_roots.at(WorldPart(index)).size() * 2 : 0; // about
                if (m_frame_worked && Clock::now() + std::chrono::duration<double, std::micro>(m_unload_us * double(entities)) > deadline)
                    break; // a later frame
                const auto start = Clock::now();
                const auto before = m_scene.world().size();
                unload(index, cell);
                m_frame_worked = true;
                if (const auto removed = before - m_scene.world().size(); removed > 0)
                    m_unload_us = 0.8 * m_unload_us + 0.2 * (ms_since(start) * 1000.0 / double(removed));
            }
            break;
        }
    }
    // Loaded cells go in, nearest first, within the frame's budget.
    auto staging = std::vector<std::pair<double, CellIndex>>{};
    for (const auto& [index, cell] : m_cells)
        if (cell.state == EditorCellState::loading && !cell.job)
            staging.emplace_back(m_source ? distance_to(*m_source, index, m_document.cell_size) : 0.0, index);
    std::ranges::sort(staging);
    for (const auto& [distance, index] : staging)
        if (!stage_step(index, m_cells.at(index), deadline)) break;
    // Cells nothing holds any more, and that never were on disk, are forgotten.
    std::erase_if(m_cells, [&](const auto& entry) {
        const auto& [index, cell] = entry;
        return !m_document.cell(index) && !cell.changed && cell.hold == CellHold::none && !m_part_roots.contains(WorldPart(index));
    });
    m_stats.frame_ms = ms_since(began);
    m_stats.longest_frame_ms = std::max(m_stats.longest_frame_ms, m_stats.frame_ms);
    count();
    return m_stats;
}

const EditorCellStats& EditorCells::settle() {
    for (int frame = 0; frame < 10000; ++frame) {
        const auto budget = m_settings.frame_budget;
        m_settings.frame_budget = std::chrono::hours(1);
        m_jobs.wait();
        update();
        m_settings.frame_budget = budget;
        const auto busy = std::ranges::any_of(m_cells, [&](const auto& entry) {
            const auto& [index, cell] = entry;
            const auto want = wanted(index, cell);
            return cell.state == EditorCellState::loading || (want && cell.state == EditorCellState::unloaded) ||
                   (!want && cell.state == EditorCellState::active && !cell.changed);
        });
        if (!busy && m_load_now.empty()) break;
    }
    return m_stats;
}

bool EditorCells::load_now(CellIndex index) {
    auto& entry = cell(index);
    if (entry.state == EditorCellState::active) return true;
    if (entry.state == EditorCellState::failed) entry.state = EditorCellState::unloaded;
    if (entry.state == EditorCellState::unloaded) start(index, entry, JobTier::frame); // waited for: the frame tier
    if (entry.state == EditorCellState::active) return true; // nothing on disk
    if (entry.job) {
        entry.job.wait();
        auto result = std::move(entry.result);
        entry.job = {};
        if (!result || !result->document) {
            fail(index, entry, result && !result->error.empty() ? result->error : "it did not load");
            return false;
        }
        entry.bytes = result->bytes;
        entry.pending = parents_first(std::move(result->document->entities));
        entry.activation_ms = 0.0;
    }
    stage_step(index, entry, Clock::time_point::max());
    count();
    return entry.state == EditorCellState::active;
}

bool EditorCells::load_for(const SceneChange& change) {
    auto cells = std::set<CellIndex>{};
    const auto note = [&](EntityId id, const std::optional<EntityRecord>& record) {
        if (const auto* known = find(id); known && known->part) cells.insert(*known->part);
        if (!record) return;
        if (record->parent) {
            if (const auto* parent = find(*record->parent); parent && parent->part) cells.insert(*parent->part);
        } else if (const auto* transform = find_component(*record, ComponentId::transform);
                   transform && !std::ranges::any_of(record->components, persistent_component)) {
            cells.insert(cell_of(std::get<TransformComponent>(*transform).translation, m_document.cell_size));
        }
    };
    for (const auto& [id, record] : change.before) note(id, record);
    for (const auto& [id, record] : change.after) note(id, record);
    auto loaded = true;
    for (const auto index : cells)
        if (state(index) != EditorCellState::active) loaded = load_now(index) && loaded;
    return loaded;
}

std::string EditorCells::hold(CellIndex index, CellHold hold) {
    auto& entry = m_cells.try_emplace(index).first->second;
    if (hold == CellHold::unloaded && entry.changed)
        return "Cell " + cell_name(index) + " has unsaved changes: save or undo them before unloading it";
    entry.hold = hold;
    return {};
}

void EditorCells::release_holds() {
    for (auto& [index, cell] : m_cells) cell.hold = CellHold::none;
}

EditorCellState EditorCells::state(CellIndex index) const {
    const auto found = m_cells.find(index);
    return found == m_cells.end() ? EditorCellState::unloaded : found->second.state;
}

CellHold EditorCells::held(CellIndex index) const {
    const auto found = m_cells.find(index);
    return found == m_cells.end() ? CellHold::none : found->second.hold;
}

bool EditorCells::changed(CellIndex index) const {
    const auto found = m_cells.find(index);
    return found != m_cells.end() && found->second.changed;
}

std::vector<CellIndex> EditorCells::changed_cells() const {
    auto result = std::vector<CellIndex>{};
    for (const auto& [index, cell] : m_cells)
        if (cell.changed) result.push_back(index);
    return result;
}

std::vector<CellIndex> EditorCells::cells() const {
    auto result = std::vector<CellIndex>{};
    for (const auto& [index, cell] : m_cells) result.push_back(index);
    return result;
}

const IndexedEntity* EditorCells::find(EntityId id) const {
    const auto found = m_index.find(id);
    return found == m_index.end() ? nullptr : &found->second;
}

std::vector<std::pair<EntityId, std::string>> EditorCells::unloaded_entities(CellIndex index) const {
    auto result = std::vector<std::pair<EntityId, std::string>>{};
    for (const auto& [id, entity] : m_index)
        if (entity.part == WorldPart(index) && !m_scene.record(id)) result.emplace_back(id, entity.name);
    std::ranges::sort(result, {}, [](const auto& entry) -> const std::string& { return entry.second; });
    return result;
}

EntityId EditorCells::root_of(EntityId id) const {
    for (const auto* record = m_scene.record(id); record && record->parent; record = m_scene.record(id)) id = *record->parent;
    return id;
}

WorldPart EditorCells::part_of_root(EntityId root) const {
    const auto* record = m_scene.record(root);
    if (!record) return std::nullopt;
    auto pending = std::vector<EntityId>{root};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (const auto* entity = m_scene.record(id); entity && std::ranges::any_of(entity->components, persistent_component)) return std::nullopt;
        const auto& children = m_scene.children(id);
        pending.insert(pending.end(), children.begin(), children.end());
    }
    const auto* transform = find_component(*record, ComponentId::transform);
    if (!transform) return std::nullopt;
    return cell_of(std::get<TransformComponent>(*transform).translation, m_document.cell_size);
}

std::vector<EntityId> EditorCells::roots(const WorldPart& part) const {
    auto result = std::vector<EntityId>{};
    for (const auto root : m_scene.roots())
        if (const auto found = m_root_part.find(root); found != m_root_part.end() && found->second == part) result.push_back(root);
    return result;
}

void EditorCells::place(EntityId root) {
    if (m_scene.transient(root)) { // a generated item: in no part until edited (an override)
        if (const auto found = m_root_part.find(root); found != m_root_part.end()) {
            m_part_roots[found->second].erase(root);
            m_touched.insert(found->second);
            m_root_part.erase(found);
        }
        return;
    }
    const auto part = part_of_root(root);
    if (const auto found = m_root_part.find(root); found != m_root_part.end()) {
        if (found->second != part) {
            m_part_roots[found->second].erase(root);
            m_touched.insert(found->second);
        }
        found->second = part;
    } else {
        m_root_part.emplace(root, part);
    }
    m_part_roots[part].insert(root);
    m_touched.insert(part);
    // Its hierarchy goes where it goes, in the index too.
    auto pending = std::vector<EntityId>{root};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        m_index.insert_or_assign(id, IndexedEntity{part, m_scene.display_name(id)});
        const auto& children = m_scene.children(id);
        pending.insert(pending.end(), children.begin(), children.end());
    }
    if (part) {
        auto& entry = cell(*part);
        if (entry.state != EditorCellState::active) m_load_now.insert(*part); // its other content, before anything is saved
    }
}

void EditorCells::changed_by(const SceneChange& change) {
    auto roots = std::set<EntityId>{};
    for (const auto& [id, after] : change.after) {
        if (!m_scene.record(id)) { // gone, with whatever it held
            if (const auto found = m_index.find(id); found != m_index.end()) {
                m_touched.insert(found->second.part);
                m_index.erase(found);
            }
            if (const auto found = m_root_part.find(id); found != m_root_part.end()) {
                m_part_roots[found->second].erase(id);
                m_root_part.erase(found);
            }
            continue;
        }
        if (m_scene.record(id)->parent)
            if (const auto found = m_root_part.find(id); found != m_root_part.end()) { // a root no longer
                m_touched.insert(found->second);
                m_part_roots[found->second].erase(id);
                m_root_part.erase(found);
            }
        if (const auto found = m_index.find(id); found != m_index.end()) m_touched.insert(found->second.part);
        roots.insert(root_of(id));
    }
    for (const auto root : roots) place(root);
}

void EditorCells::refresh() {
    for (const auto& part : std::exchange(m_touched, {})) {
        if (!part) {
            m_persistent_changed = part_text(part) != m_persistent_saved;
            continue;
        }
        auto& entry = cell(*part);
        if (entry.state != EditorCellState::active) continue; // loaded first (m_load_now), then compared
        if (entry.saved.empty() && !m_document.cell(*part)) { // new to the world: changed while anything stands in it
            entry.changed = m_part_roots.contains(part) && !m_part_roots.at(part).empty();
            continue;
        }
        entry.changed = part_text(part) != entry.saved;
    }
}

SceneDocument EditorCells::part_document(const WorldPart& part) const { return roots_document(roots(part)); }

SceneDocument EditorCells::roots_document(const std::vector<EntityId>& roots_in_order) const {
    auto document = SceneDocument{};
    auto pending = std::vector<EntityId>(roots_in_order.rbegin(), roots_in_order.rend());
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto* record = m_scene.record(id);
        document.entities.push_back({id, record->parent, record->components});
        const auto& children = m_scene.children(id);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return document;
}

std::string EditorCells::part_text(const WorldPart& part) const { return roots_text(roots(part)); }

std::string EditorCells::roots_text(const std::vector<EntityId>& roots_in_order) const {
    auto out = std::ostringstream{};
    write_scene(out, roots_document(roots_in_order), m_context);
    return out.str();
}

std::string EditorCells::check(const SceneChange& change) const {
    auto roots = std::set<EntityId>{};
    for (const auto& [id, after] : change.after)
        if (after && m_scene.record(id)) roots.insert(root_of(id));
    const auto& world = m_scene.world();
    for (const auto root : roots) {
        const auto part = part_of_root(root);
        if (!part) continue; // persistent: wherever it is
        auto pending = std::vector<EntityId>(m_scene.children(root).begin(), m_scene.children(root).end());
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            const auto& children = m_scene.children(id);
            pending.insert(pending.end(), children.begin(), children.end());
            const auto handle = world.find(id);
            const auto pose = handle ? world.world_matrix(*handle) : std::nullopt;
            if (!pose) continue;
            if (const auto at = cell_of(pose->translation, m_document.cell_size); at != *part)
                return "'" + m_scene.display_name(id) + "' would be in cell " + cell_name(at) + ", but its root '" + m_scene.display_name(root) +
                       "' is in cell " + cell_name(*part) + ": a hierarchy stays in one cell";
        }
    }
    return {};
}

SceneDiagnostics EditorCells::save() {
    for (const auto index : std::exchange(m_load_now, {})) load_now(index);
    refresh();
    const auto persistent = part_document(std::nullopt);
    auto parts = std::map<CellIndex, SceneDocument>{};
    for (const auto& [index, cell] : m_cells)
        if (cell.changed) parts.emplace(index, part_document(index));
    if (auto diagnostics = save_world_parts(m_file, m_document, &persistent, parts, m_context); !diagnostics.empty()) return diagnostics;
    m_persistent_saved = part_text(std::nullopt);
    m_persistent_changed = false;
    for (const auto& [index, document] : parts) {
        auto& entry = m_cells.at(index);
        entry.changed = false;
        entry.saved = m_document.cell(index) ? part_text(index) : std::string{};
    }
    count();
    return {};
}

EditorCells::PlayWorld EditorCells::play_world(CellLoader disk) const {
    auto play = PlayWorld{m_document, part_document(std::nullopt), {}};
    auto edited = std::make_shared<std::map<CellIndex, SceneDocument>>();
    for (const auto& [index, cell] : m_cells)
        if (cell.changed) edited->emplace(index, part_document(index));
    for (const auto& [index, document] : *edited) {
        auto found = std::ranges::lower_bound(play.document.cells, index, {}, &WorldCell::index);
        const auto listed = found != play.document.cells.end() && found->index == index;
        if (document.entities.empty()) {
            if (listed) play.document.cells.erase(found);
        } else if (listed) {
            found->entities = uint32_t(document.entities.size());
        } else {
            play.document.cells.insert(found, {index, std::filesystem::path("edited") / (cell_name(index) + ".scene"), uint32_t(document.entities.size())});
        }
    }
    play.loader = [edited, disk = std::move(disk)](const WorldCell& cell, const JobContext& job) -> CellLoad {
        if (const auto found = edited->find(cell.index); found != edited->end())
            return {found->second, {}, found->second.entities.size() * 256}; // an estimate: it is never cooked
        return disk(cell, job);
    };
    return play;
}

void EditorCells::count() {
    m_stats.cells = {};
    m_stats.changed = m_stats.held = m_stats.loads_in_flight = 0;
    m_stats.bytes_loaded = 0;
    for (const auto& [index, cell] : m_cells) {
        ++m_stats.cells[size_t(cell.state)];
        m_stats.changed += cell.changed;
        m_stats.held += cell.hold != CellHold::none;
        m_stats.loads_in_flight += cell.job && !cell.job.done();
        if (cell.state == EditorCellState::active) m_stats.bytes_loaded += cell.bytes;
    }
}

} // namespace maya::editor
