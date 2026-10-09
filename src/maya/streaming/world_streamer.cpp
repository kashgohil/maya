#include "maya/streaming/world_streamer.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/project.hpp"
#include "maya/physics/physics.hpp"
#include "maya/scene/scene_binary.hpp"
#include "maya/simulation/authored_physics.hpp"
#include "maya/world/world.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <numbers>

namespace maya {
namespace {
constexpr uint32_t cell_cook_version = 1;
constexpr size_t max_loads_in_flight = 8;
constexpr size_t chunk_entities = 128; // the most entities in one commit or removal: what one misjudged step can overrun

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

/// The components of `entity` that keep state, in schema order.
std::vector<ComponentValue> kept(const std::vector<ComponentValue>& components) {
    auto result = std::vector<ComponentValue>{};
    for (const auto& value : components)
        if (component_schema(component_id(value))->keeps_state) result.push_back(value);
    std::ranges::sort(result, {}, [](const ComponentValue& value) { return component_id(value); });
    return result;
}
bool same_components(const std::vector<ComponentValue>& a, const std::vector<ComponentValue>& b) {
    if (a.size() != b.size()) return false;
    const auto encode = [](const std::vector<ComponentValue>& components) {
        auto document = SceneDocument{};
        document.entities.push_back({EntityId{1, 1}, std::nullopt, components});
        return encode_scene_binary(document);
    };
    return encode(a) == encode(b);
}
constexpr auto no_parent = std::numeric_limits<uint32_t>::max();

/// A cell's content as activation commits it: parents before their children, and each entity's parent as
/// an index (no_parent for a root), so activation looks nothing up by ID and a parent is always committed
/// before its children, whatever order the file lists them in.
struct CellContent {
    SceneDocument document;
    std::vector<uint32_t> parents;
};

/// Arranges a loaded cell, on its load job.
CellContent arrange(SceneDocument document) {
    const auto count = uint32_t(document.entities.size());
    auto index = std::unordered_map<EntityId, uint32_t, PersistentIdHash>{};
    index.reserve(count);
    for (uint32_t i = 0; i < count; ++i) index.emplace(document.entities[i].id, i);
    auto parent = std::vector<uint32_t>(count, no_parent);
    auto first_child = std::vector<uint32_t>(count, no_parent);
    auto next_sibling = std::vector<uint32_t>(count, no_parent);
    for (auto i = count; i-- > 0;) { // backwards, so each entity's children list in file order
        const auto& id = document.entities[i].parent;
        const auto found = id ? index.find(*id) : index.end();
        if (found == index.end() || found->second == i) continue;
        parent[i] = found->second;
        next_sibling[i] = first_child[found->second];
        first_child[found->second] = i;
    }
    // Each root's tree in preorder.
    auto order = std::vector<uint32_t>{};
    order.reserve(count);
    auto placed = std::vector<uint32_t>(count, no_parent); // where each entity goes
    auto stack = std::vector<uint32_t>{};
    auto children = std::vector<uint32_t>{};
    const auto visit = [&](uint32_t root) {
        stack.push_back(root);
        while (!stack.empty()) {
            const auto entity = stack.back();
            stack.pop_back();
            if (placed[entity] != no_parent) continue;
            placed[entity] = uint32_t(order.size());
            order.push_back(entity);
            children.clear();
            for (auto child = first_child[entity]; child != no_parent; child = next_sibling[child]) children.push_back(child);
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
    };
    for (uint32_t i = 0; i < count; ++i)
        if (parent[i] == no_parent) visit(i);
    for (uint32_t i = 0; i < count; ++i)
        if (placed[i] == no_parent) { // in a cycle, which validation refuses: cut where it is entered
            parent[i] = no_parent;
            visit(i);
        }
    auto result = CellContent{};
    result.document.entities.reserve(count);
    result.parents.reserve(count);
    for (const auto entity : order) {
        result.document.entities.push_back(std::move(document.entities[entity]));
        result.parents.push_back(parent[entity] == no_parent ? no_parent : placed[parent[entity]]);
    }
    return result;
}

/// The authored cell with play's changes applied: destroyed entities left out with everything below them,
/// and changed ones' components that keep state replaced. Without changes it is the authored cell itself.
std::shared_ptr<const CellContent> with_delta(std::shared_ptr<const CellContent> authored, const CellDelta* delta) {
    if (!delta || delta->empty()) return authored;
    const auto& entities = authored->document.entities;
    auto result = std::make_shared<CellContent>();
    result->document.entities.reserve(entities.size());
    result->parents.reserve(entities.size());
    auto placed = std::vector<uint32_t>(entities.size(), no_parent);
    for (size_t i = 0; i < entities.size(); ++i) {
        const auto& entity = entities[i];
        const auto parent = authored->parents[i];
        if (delta->destroyed.contains(entity.id) || (parent != no_parent && placed[parent] == no_parent)) continue;
        auto copy = entity;
        if (const auto changed = delta->changed.find(entity.id); changed != delta->changed.end()) {
            std::erase_if(copy.components, [](const ComponentValue& value) { return component_schema(component_id(value))->keeps_state; });
            copy.components.insert(copy.components.end(), changed->second.begin(), changed->second.end());
        }
        placed[i] = uint32_t(result->document.entities.size());
        result->document.entities.push_back(std::move(copy));
        result->parents.push_back(parent == no_parent ? no_parent : placed[parent]);
    }
    return result;
}
} // namespace

const char* cell_state_name(CellState state) noexcept {
    switch (state) {
    case CellState::unloaded: return "unloaded";
    case CellState::loading: return "loading";
    case CellState::ready: return "ready";
    case CellState::activating: return "activating";
    case CellState::active: return "active";
    case CellState::deactivating: return "deactivating";
    case CellState::failed: return "failed";
    }
    return "unknown";
}

StreamingSettings project_streaming_settings(const ProjectSettings& project) {
    auto settings = StreamingSettings{};
    if (project.stream_load) settings.load_radius = *project.stream_load;
    if (project.stream_activate) settings.activate_radius = *project.stream_activate;
    if (project.stream_hysteresis) settings.hysteresis = *project.stream_hysteresis;
    return settings;
}

CellLoader cooked_cell_loader(std::filesystem::path world_folder, std::shared_ptr<CookCache> cache, PropertyValidationContext context) {
    auto lock = std::make_shared<std::mutex>(); // the cache is the loader's; loads run on several workers
    return [folder = std::move(world_folder), cache = std::move(cache), context = std::move(context), lock](const WorldCell& cell,
                                                                                                             const JobContext& job) -> CellLoad {
        const auto path = folder / cell.scene;
        auto file = std::ifstream(path, std::ios::binary);
        if (!file) return {std::nullopt, path.generic_string() + ": cannot open the cell's scene", 0};
        const auto text = std::string(std::istreambuf_iterator<char>(file), {});
        if (job.cancelled()) return {};
        if (path.extension() == ".cell") { // cooked already, in the cook cache's checked envelope
            const auto unwrapped = unwrap_cooked(std::as_bytes(std::span(text)));
            if (!unwrapped) return {std::nullopt, path.generic_string() + ": the cooked cell is damaged", 0};
            const auto bytes = std::string_view(reinterpret_cast<const char*>(unwrapped->data()), unwrapped->size());
            auto decoded = decode_scene_binary(bytes, context);
            if (!decoded) return {std::nullopt, path.generic_string() + ": " + decoded.diagnostics.front().message, 0};
            return {std::move(decoded.document), {}, bytes.size()};
        }
        auto key = CookKey{"cell", cell_cook_version, {}, "schemas " + std::to_string(scene_schema_fingerprint()) + "\n"};
        if (cache) {
            const auto guard = std::lock_guard(*lock);
            key.source = cache->source_digest(path, std::as_bytes(std::span(text)));
            if (const auto cooked = cache->read(key)) {
                const auto bytes = std::string_view(reinterpret_cast<const char*>(cooked->data()), cooked->size());
                if (auto decoded = decode_scene_binary(bytes, context)) return {std::move(decoded.document), {}, bytes.size()};
                // A damaged or foreign cook: cooked again below.
            }
        }
        auto parsed = read_scene(text, context);
        if (!parsed) return {std::nullopt, path.generic_string() + ": " + parsed.diagnostics.front().message, 0};
        if (job.cancelled()) return {};
        const auto binary = encode_scene_binary(parsed.document);
        if (cache) {
            const auto guard = std::lock_guard(*lock);
            cache->write(key, std::as_bytes(std::span(binary)));
        }
        return {std::move(parsed.document), {}, binary.size()};
    };
}

struct WorldStreamer::Cell {
    const WorldCell* info = nullptr;
    CellState state = CellState::unloaded;
    uint64_t generation = 0;
    JobHandle job;
    double distance = std::numeric_limits<double>::infinity();
    std::shared_ptr<const CellContent> authored; // while loaded
    size_t bytes = 0;
    std::optional<CellDelta> delta; // play's changes, kept while unloaded
    std::string error;
    bool pinned = false;
    // Activating: the content going in, and how far it is.
    StageGroup group = 0;
    std::shared_ptr<const CellContent> pending;
    size_t next = 0;
    std::vector<EntityHandle> handles; // what is committed, in the content's order
    std::vector<EntityId> physics; // committed entities with physics components
    // Active: its entities, the group they were published from, and those play changed.
    std::vector<EntityHandle> entities;
    StageGroup origin = 0;
    std::unordered_set<EntityId, PersistentIdHash> dirty;
};

WorldStreamer::WorldStreamer(WorldDocument world, CellLoader loader, StreamingSettings settings)
    : m_world(std::move(world)), m_loader(std::move(loader)), m_settings(settings) {
    for (const auto& info : m_world.cells) {
        auto cell = std::make_unique<Cell>();
        cell->info = &info;
        m_cells.emplace(info.index, std::move(cell));
    }
    count();
}

WorldStreamer::~WorldStreamer() { m_jobs.cancel(); }

void WorldStreamer::release(std::shared_ptr<const void>&& held) {
    // Freeing a cell's content is thousands of frees (#1064: up to 4.5 ms): a job does it, off the owner thread.
    if (held && held.use_count() == 1) m_jobs.submit(JobTier::background, [held = std::move(held)](JobContext&) {});
}

void WorldStreamer::set_sources(std::vector<math::DVec3> positions) { m_sources = std::move(positions); }

CellState WorldStreamer::state(CellIndex index) const {
    const auto found = m_cells.find(index);
    return found == m_cells.end() ? CellState::unloaded : found->second->state;
}

const CellDelta* WorldStreamer::delta(CellIndex index) const {
    const auto found = m_cells.find(index);
    return found == m_cells.end() || !found->second->delta ? nullptr : &*found->second->delta;
}

WorldStreamer::Want WorldStreamer::want(const Cell& cell) const {
    const auto d = cell.distance;
    const auto loaded = cell.state != CellState::unloaded && cell.state != CellState::failed;
    const auto active = cell.state == CellState::activating || cell.state == CellState::active;
    auto result = Want{};
    result.loaded = d <= m_settings.load_radius || (loaded && d <= m_settings.load_radius + m_settings.hysteresis);
    result.active = d <= m_settings.activate_radius || (active && d <= m_settings.activate_radius + m_settings.hysteresis);
    result.loaded |= result.active;
    return result;
}

void WorldStreamer::start_load(Cell& cell) {
    ++cell.generation;
    cell.state = CellState::loading;
    cell.error.clear();
    ++m_stats.loads_started;
    const auto index = cell.info->index;
    const auto generation = cell.generation;
    cell.job = m_jobs.submit(m_settings.tier, [this, info = *cell.info, sink = m_completions.sink(), index, generation](JobContext& job) {
        auto load = m_loader(info, job);
        if (job.cancelled()) return; // nothing to post: the load was given up
        auto content = load.document ? std::make_shared<const CellContent>(arrange(std::move(*load.document))) : nullptr;
        const auto request = (uint64_t(uint32_t(index.x)) << 32) | uint32_t(index.z);
        sink.post(request, generation, [this, index, generation, content, error = std::move(load.error), bytes = load.bytes] {
            auto& target = *m_cells.at(index);
            if (target.state != CellState::loading || target.generation != generation) {
                ++m_stats.completions_discarded;
                return;
            }
            if (!content) {
                target.state = CellState::failed;
                target.error = error.empty() ? "the load was cancelled" : error;
                m_stats.last_error = target.error;
                ++m_stats.loads_failed;
                return;
            }
            target.authored = content;
            target.bytes = bytes;
            target.state = CellState::ready;
            ++m_stats.loads_finished;
        });
    });
}

void WorldStreamer::cancel_load(Cell& cell) {
    cell.job.cancel();
    ++cell.generation; // a completion already posted is discarded when it is drained
    cell.state = CellState::unloaded;
    ++m_stats.loads_cancelled;
}

/// One frame's owner-thread work: a step starts only if what it is measured to cost fits the time left. The
/// frame's first step always starts, so streaming moves forward even when the frame began late.
struct WorldStreamer::Frame {
    std::chrono::steady_clock::time_point deadline;
    size_t entities = 0, limit = 0; // committed or removed, and the most allowed
    bool worked = false;
    double left_us() const {
        return std::chrono::duration<double, std::micro>(deadline - std::chrono::steady_clock::now()).count();
    }
    bool fits(double cost_us) const { return !worked || (entities < limit && cost_us <= left_us()); }
    /// How many entities at `cost_us` each fit, at least `least` for the frame's first step.
    size_t room(double cost_us, size_t most, size_t least) const {
        auto count = std::min(most, limit > entities ? limit - entities : 0);
        count = std::min(count, size_t(std::max(0.0, left_us()) / std::max(cost_us, 0.001)));
        return worked ? count : std::max(count, std::min(least, most));
    }
};

namespace {
constexpr size_t least_entities = 16; // a frame's first commit or removal: progress, however late it began
/// The measured costs follow the machine and the content. A dearer measure counts at once and a cheaper one
/// a tenth of the way: a step planned on too low a cost overruns the frame.
void measure(double& cost, double measured) { cost = measured > cost ? measured : cost + (measured - cost) * 0.1; }
double microseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}
bool has_physics(const SceneEntity& entity) {
    return std::ranges::any_of(entity.components, [](const ComponentValue& value) {
        return std::holds_alternative<ColliderComponent>(value) || std::holds_alternative<RigidBodyComponent>(value) ||
               std::holds_alternative<PhysicsSettingsComponent>(value);
    });
}
} // namespace

bool WorldStreamer::activate_step(Cell& cell, World& world, PhysicsWorld* physics, Frame& frame) {
    const auto fail = [&](std::string error) {
        world.discard(cell.group);
        cell.error = cell.info->scene.generic_string() + ": " + std::move(error);
        m_stats.last_error = cell.error;
        cell.state = CellState::failed;
        release(std::move(cell.authored));
        release(std::move(cell.pending));
        cell.handles = {};
        return false;
    };
    const auto& content = *cell.pending;
    const auto& pending = content.document.entities;
    while (cell.next < pending.size()) {
        // One commit of the next entities, staged: none is visible until the cell is complete.
        const auto count = frame.room(m_costs.commit_us, std::min(chunk_entities, pending.size() - cell.next), least_entities);
        if (count == 0) return false; // next frame
        const auto start = std::chrono::steady_clock::now();
        const auto end = cell.next + count;
        auto commands = world.commands();
        auto made = std::vector<PendingEntity>{};
        made.reserve(count);
        for (auto i = cell.next; i < end; ++i) {
            const auto& entity = pending[i];
            const auto created = commands.create_staged(entity.id, cell.group);
            for (const auto& value : entity.components) std::visit([&](const auto& component) { commands.add(created, component); }, value);
            if (const auto parent = content.parents[i]; parent != no_parent) { // committed already: it comes first
                if (parent >= cell.next) commands.reparent(created, made[parent - cell.next], ReparentPolicy::keep_local);
                else commands.reparent(created, cell.handles[parent], ReparentPolicy::keep_local);
            }
            if (has_physics(entity)) cell.physics.push_back(entity.id); // what can make a body, found while it is at hand
            made.push_back(created);
        }
        const auto result = world.commit(commands);
        if (!result) return fail(std::string("cannot go into the World: ") + error_name(result.error));
        for (const auto created : made) cell.handles.push_back(result.created[created.index]);
        measure(m_costs.commit_us, microseconds_since(start) / double(count));
        frame.entities += count;
        frame.worked = true;
        cell.next = end;
    }
    // Complete: published at once, with its bodies (#1060: one batch).
    if (!frame.fits(m_costs.finish_us)) return false; // next frame
    const auto start = std::chrono::steady_clock::now();
    auto published = world.publish(cell.group);
    if (physics) {
        auto authored = authored_physics(world, cell.physics);
        auto error = authored ? std::string{} : authored.error;
        if (error.empty()) error = physics->create_bodies(world, authored.bodies);
        if (!error.empty()) {
            if (world.stage(published, cell.group) == WorldError::none) world.discard(cell.group);
            return fail("its physics cannot be built: " + error);
        }
    }
    cell.entities = std::move(published);
    release(std::move(cell.pending));
    cell.handles = {};
    cell.physics = {};
    cell.dirty.clear();
    cell.origin = cell.group; // its edits name this group
    m_published[cell.group] = cell.info->index;
    cell.group = 0;
    cell.state = CellState::active;
    ++m_stats.activations;
    measure(m_costs.finish_us, microseconds_since(start));
    frame.worked = true;
    return true;
}

void WorldStreamer::begin_deactivate(Cell& cell, World& world, PhysicsWorld* physics) {
    // Play's changes first, while the entities are visible: only those the edit journal named.
    auto delta = cell.delta.value_or(CellDelta{});
    auto authored = std::unordered_map<EntityId, const SceneEntity*, PersistentIdHash>{};
    if (!cell.dirty.empty())
        for (const auto& entity : cell.authored->document.entities) authored.emplace(entity.id, &entity);
    for (const auto id : cell.dirty) {
        const auto source = authored.find(id);
        if (source == authored.end()) continue;
        const auto handle = world.find(id);
        if (!handle) {
            delta.destroyed.insert(id);
            delta.changed.erase(id);
            continue;
        }
        auto now = std::vector<ComponentValue>{};
        for (const auto& schema : component_schemas())
            if (schema.keeps_state)
                if (auto value = read_component(world, *handle, schema.id)) now.push_back(std::move(*value));
        if (same_components(now, kept(source->second->components))) delta.changed.erase(id);
        else delta.changed[id] = std::move(now);
    }
    // Then out of sight in one step, and out of physics in one batch.
    auto live = std::vector<EntityHandle>{};
    live.reserve(cell.entities.size());
    for (const auto entity : cell.entities)
        if (world.alive(entity)) live.push_back(entity);
    const auto group = m_next_group++;
    if (world.stage(live, group) != WorldError::none) {
        // A hierarchy now reaches outside the cell (play reparented across it): it stays active.
        if (!cell.pinned) {
            cell.pinned = true;
            ++m_stats.pinned;
            m_stats.last_error = cell.info->scene.generic_string() + ": kept active: play joined its entities to others' hierarchies";
        }
        return;
    }
    if (physics) physics->remove_bodies(live);
    cell.delta = delta.empty() ? std::nullopt : std::optional(std::move(delta));
    m_published.erase(cell.origin);
    cell.origin = 0;
    cell.entities.clear();
    cell.dirty.clear();
    cell.group = group;
    cell.state = CellState::deactivating;
    ++m_stats.deactivations;
}

bool WorldStreamer::deactivate_step(Cell& cell, World& world, Frame& frame) {
    for (;;) {
        const auto count = frame.room(m_costs.discard_us, chunk_entities, least_entities);
        if (count == 0) return false; // next frame
        const auto start = std::chrono::steady_clock::now();
        const auto before = world.staged(cell.group).size();
        const auto left = world.discard(cell.group, count);
        const auto removed = before - left;
        if (removed > 0) measure(m_costs.discard_us, microseconds_since(start) / double(removed));
        frame.entities += removed;
        frame.worked = true;
        if (left == 0) {
            cell.group = 0;
            cell.state = CellState::ready; // still loaded: the next update decides whether it stays
            return true;
        }
    }
}

const StreamingStats& WorldStreamer::update(World& world, PhysicsWorld* physics) {
    const auto start = std::chrono::steady_clock::now();
    world.record_edits(true);
    // Finished loads.
    m_completions.drain(std::chrono::microseconds(1000000), [this](uint64_t request, uint64_t generation) {
        const auto index = CellIndex{int32_t(uint32_t(request >> 32)), int32_t(uint32_t(request))};
        const auto found = m_cells.find(index);
        const auto current = found != m_cells.end() && found->second->generation == generation;
        if (!current) ++m_stats.completions_discarded;
        return current;
    });
    // What play changed in active cells.
    for (const auto& edit : world.take_edits())
        if (const auto owner = m_published.find(edit.origin); owner != m_published.end()) m_cells.at(owner->second)->dirty.insert(edit.id);
    // Distances from the nearest source to each cell's square.
    const auto size = m_world.cell_size;
    for (auto& [index, cell] : m_cells) {
        cell->distance = std::numeric_limits<double>::infinity();
        const auto x0 = double(index.x) * size, z0 = double(index.z) * size;
        for (const auto& source : m_sources) {
            const auto dx = std::max({x0 - source.x, 0.0, source.x - (x0 + size)});
            const auto dz = std::max({z0 - source.z, 0.0, source.z - (z0 + size)});
            cell->distance = std::min(cell->distance, std::hypot(dx, dz));
        }
    }
    auto order = std::vector<Cell*>{};
    order.reserve(m_cells.size());
    for (auto& [index, cell] : m_cells) order.push_back(cell.get());
    std::ranges::sort(order, {}, [](const Cell* cell) { return cell->distance; }); // nearest first
    // Decisions: cheap, every cell. What costs the World time waits for the work below.
    auto loads = size_t(std::ranges::count(order, CellState::loading, &Cell::state));
    for (auto* cell : order) {
        const auto want = this->want(*cell);
        switch (cell->state) {
        case CellState::failed:
            if (!want.loaded) cell->state = CellState::unloaded; // tried again when it comes back
            break;
        case CellState::unloaded:
            if (want.loaded && loads < max_loads_in_flight && m_stats.bytes_loaded < m_settings.bytes_in_flight) {
                start_load(*cell);
                ++loads;
            }
            break;
        case CellState::loading:
            if (!want.loaded) {
                cancel_load(*cell);
                --loads;
            }
            break;
        case CellState::ready:
            if (!want.loaded) {
                release(std::move(cell->authored));
                cell->bytes = 0;
                cell->state = CellState::unloaded;
            } else if (want.active) {
                cell->pending = with_delta(cell->authored, cell->delta ? &*cell->delta : nullptr);
                cell->group = m_next_group++;
                cell->handles.reserve(cell->pending->document.entities.size());
                cell->next = 0;
                cell->state = CellState::activating;
            }
            break;
        case CellState::activating:
            if (!want.active) { // what went in comes out again, in steps
                release(std::move(cell->pending));
                cell->handles = {};
                cell->physics = {};
                cell->state = CellState::deactivating;
            }
            break;
        case CellState::active:
        case CellState::deactivating:
            break;
        }
    }
    // Work, within the frame's budget: taking cells out first, farthest first, then putting the nearest in.
    // Steps are planned to a tenth short of the budget: what they are measured to cost varies a little.
    const auto planned = std::chrono::duration_cast<std::chrono::steady_clock::duration>(m_settings.frame_budget * 9 / 10);
    auto frame = Frame{start + planned, 0, m_settings.frame_entities, false};
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        auto& cell = **it;
        if (cell.state == CellState::active && !cell.pinned && !want(cell).active) {
            if (!frame.fits(m_costs.leave_us)) break;
            const auto begun = std::chrono::steady_clock::now();
            begin_deactivate(cell, world, physics);
            measure(m_costs.leave_us, microseconds_since(begun));
            frame.worked = true;
        }
        if (cell.state == CellState::deactivating && !deactivate_step(cell, world, frame)) break;
    }
    for (auto* cell : order)
        if (cell->state == CellState::activating && !activate_step(*cell, world, physics, frame)) break;
    m_stats.frame_ms = milliseconds(std::chrono::steady_clock::now() - start);
    m_stats.longest_frame_ms = std::max(m_stats.longest_frame_ms, m_stats.frame_ms);
    count();
    return m_stats;
}

const StreamingStats& WorldStreamer::settle(World& world, PhysicsWorld* physics) {
    const auto budget = m_settings.frame_budget;
    const auto entities = m_settings.frame_entities;
    m_settings.frame_budget = std::chrono::microseconds(std::numeric_limits<int32_t>::max());
    m_settings.frame_entities = std::numeric_limits<size_t>::max();
    for (;;) {
        update(world, physics);
        const auto busy = std::ranges::any_of(m_cells, [this](const auto& item) {
            const auto& cell = *item.second;
            const auto want = this->want(cell);
            if (cell.state == CellState::loading || cell.state == CellState::activating || cell.state == CellState::deactivating) return true;
            if (cell.state == CellState::failed || cell.pinned) return false;
            return want.active ? cell.state != CellState::active : want.loaded ? cell.state != CellState::ready : cell.state != CellState::unloaded;
        });
        if (!busy) break;
        m_jobs.wait(); // the loads finish; the next update applies them
    }
    // Room for the most the sources can hold active, so streaming never grows the World's tables: growing
    // them costs a frame milliseconds. That is the cells whose squares come within reach of a source, at
    // the world's mean entities a cell, and at least half as many again as are in the World now.
    const auto reach = (m_settings.activate_radius + m_settings.hysteresis) / m_world.cell_size;
    const auto cells = std::numbers::pi * reach * reach + 4.0 * reach + 1.0;
    auto authored = size_t{0};
    for (const auto& info : m_world.cells) authored += info.entities;
    const auto mean = m_world.cells.empty() ? 0.0 : double(authored) / double(m_world.cells.size());
    const auto present = world.size() + world.staged_count();
    const auto others = present > m_stats.active_entities ? present - m_stats.active_entities : 0; // not the cells'
    const auto peak = others + size_t(cells * mean * double(std::max<size_t>(1, m_sources.size())));
    world.reserve(std::max(present * 3 / 2, peak));
    m_settings.frame_budget = budget;
    m_settings.frame_entities = entities;
    return m_stats;
}

void WorldStreamer::unload_all(World& world, PhysicsWorld* physics) {
    m_sources.clear();
    const auto entities = m_settings.frame_entities;
    m_settings.frame_entities = std::numeric_limits<size_t>::max();
    for (auto& [index, cell] : m_cells) {
        if (cell->state == CellState::loading) cancel_load(*cell);
        if (cell->state == CellState::activating) {
            world.discard(cell->group);
            cell->pending.reset();
            cell->handles = {};
            cell->physics = {};
            cell->state = CellState::ready;
        }
        if (cell->state == CellState::active) begin_deactivate(*cell, world, physics);
        if (cell->state == CellState::deactivating) {
            world.discard(cell->group);
            cell->group = 0;
        }
        if (!cell->pinned) {
            cell->authored.reset();
            cell->bytes = 0;
            cell->state = CellState::unloaded;
        }
    }
    m_settings.frame_entities = entities;
    m_jobs.cancel();
    count();
}

void WorldStreamer::count() {
    m_stats.cells = {};
    m_stats.bytes_in_flight = m_stats.bytes_loaded = m_stats.active_entities = m_stats.deltas = 0;
    for (const auto& [index, cell] : m_cells) {
        ++m_stats.cells[size_t(cell->state)];
        if (cell->state == CellState::loading) m_stats.bytes_in_flight += size_t(cell->info->entities) * 256; // an estimate until loaded
        m_stats.bytes_loaded += cell->bytes;
        m_stats.active_entities += cell->entities.size();
        m_stats.deltas += cell->delta.has_value();
    }
}

} // namespace maya
