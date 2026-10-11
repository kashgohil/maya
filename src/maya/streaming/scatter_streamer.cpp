#include "maya/streaming/scatter_streamer.hpp"
#include "maya/core/sha256.hpp"
#include "maya/physics/physics.hpp"
#include "maya/properties/schema.hpp"
#include "maya/simulation/authored_physics.hpp"
#include "maya/streaming/terrain_streamer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <unordered_set>

namespace maya {
namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
template <class Tag> std::string id_text(PersistentId<Tag> id) {
    char text[40];
    std::snprintf(text, sizeof(text), "%llx:%llx", static_cast<unsigned long long>(id.high), static_cast<unsigned long long>(id.low));
    return text;
}

/// What a scatter cell's items depend on, as text: the rules, the generator's ID, and what each mesh is.
std::string scatter_text(AssetId id, const GeneratorAsset& g) {
    auto out = std::ostringstream{};
    out.precision(9);
    out << "scatter " << g.version << "\nid " << id.high << ' ' << id.low << "\nseed " << g.seed << "\nspacing " << g.spacing << "\ndensity "
        << g.density << "\nslope " << g.max_slope << "\nheights " << g.min_height << ' ' << g.max_height << "\nscale " << g.min_scale << ' '
        << g.max_scale << "\nyaw " << g.random_yaw << "\nbase " << g.base << "\nweights";
    for (const auto weight : scatter_rules(g).weights) out << ' ' << weight;
    out << '\n';
    return out.str();
}
/// An exclusion as text, exactly: its shape, size, and world pose.
std::string exclusion_text(const ScatterExclusion& exclusion) {
    auto out = std::ostringstream{};
    out.precision(17);
    out << (exclusion.shape == ScatterExclusion::Shape::box ? "box " : "sphere ") << exclusion.half_extents.x << ' ' << exclusion.half_extents.y
        << ' ' << exclusion.half_extents.z << ' ' << exclusion.radius << ' ' << exclusion.pose.translation.x << ' ' << exclusion.pose.translation.y
        << ' ' << exclusion.pose.translation.z;
    for (const auto element : exclusion.pose.linear.elements) out << ' ' << element;
    out << '\n';
    return out.str();
}
/// Whether an exclusion can reach a cell: its bounding sphere against the cell's square.
bool reaches(const ScatterExclusion& exclusion, TileIndex cell) {
    auto reach = 0.0f;
    if (exclusion.shape == ScatterExclusion::Shape::sphere) {
        for (int c = 0; c < 3; ++c) reach = std::max(reach, exclusion.pose.axis(c).length());
        reach *= exclusion.radius;
    } else {
        reach = (exclusion.pose.axis(0) * exclusion.half_extents.x).length() + (exclusion.pose.axis(1) * exclusion.half_extents.y).length() +
                (exclusion.pose.axis(2) * exclusion.half_extents.z).length();
    }
    const auto x0 = double(cell.x) * tile_size, z0 = double(cell.z) * tile_size;
    const auto& p = exclusion.pose.translation;
    const auto dx = std::max({x0 - p.x, 0.0, p.x - (x0 + tile_size)}), dz = std::max({z0 - p.z, 0.0, p.z - (z0 + tile_size)});
    return std::sqrt(dx * dx + dz * dz) <= double(reach);
}
std::vector<ScatterExclusion> reaching(std::span<const ScatterExclusion> exclusions, TileIndex cell) {
    auto result = std::vector<ScatterExclusion>{};
    for (const auto& exclusion : exclusions)
        if (reaches(exclusion, cell)) result.push_back(exclusion);
    return result;
}
double distance_to(const math::DVec3& point, TileIndex cell) {
    const auto x0 = double(cell.x) * tile_size, z0 = double(cell.z) * tile_size;
    const auto dx = std::max({x0 - point.x, 0.0, point.x - (x0 + tile_size)}), dz = std::max({z0 - point.z, 0.0, point.z - (z0 + tile_size)});
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

CookKey scatter_cell_cook_key(AssetId generator, const GeneratorAsset& scatter, const GeneratorAsset* heights, TileIndex cell,
                              std::span<const ScatterExclusion> exclusions) {
    auto text = scatter_text(generator, scatter);
    if (heights) text += "tile " + sha256_text(height_tile_cook_key(*heights, cell).digest()) + '\n';
    for (const auto& exclusion : exclusions)
        if (reaches(exclusion, cell)) text += exclusion_text(exclusion);
    return {"scatter-cell", scatter_cook_version, sha256(std::as_bytes(std::span(text.data(), text.size()))),
            "cell " + std::to_string(cell.x) + " " + std::to_string(cell.z) + "\n"};
}

SceneEntity scatter_entity(AssetId generator, const GeneratorAsset& scatter, const ScatterItem& item, const std::string& name) {
    const AssetRef<MeshAsset> meshes[] = {scatter.mesh0, scatter.mesh1, scatter.mesh2, scatter.mesh3};
    const AssetRef<MaterialAsset> materials[] = {scatter.material0, scatter.material1, scatter.material2, scatter.material3};
    const float colliders[] = {scatter.collider0, scatter.collider1, scatter.collider2, scatter.collider3};
    auto entity = SceneEntity{item.id, std::nullopt, {}};
    entity.components.push_back(NameComponent{name});
    entity.components.push_back(TransformComponent{item.position, math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, item.yaw), math::Vec3(item.scale)});
    auto renderer = MeshRendererComponent{};
    renderer.mesh = meshes[item.variant];
    renderer.material = materials[item.variant];
    entity.components.push_back(renderer);
    if (colliders[item.variant] > 0.0f) {
        auto collider = ColliderComponent{};
        collider.shape = ColliderShape::sphere;
        collider.radius = colliders[item.variant];
        entity.components.push_back(collider);
    }
    entity.components.push_back(GeneratedComponent{AssetRef<GeneratorAsset>{generator}, false});
    return entity;
}

std::string WorldScatterSink::stage(StageGroup group, std::span<const SceneEntity> items) {
    auto commands = m_world.commands();
    for (const auto& item : items) {
        const auto created = commands.create_staged(item.id, group);
        for (const auto& value : item.components) std::visit([&](const auto& component) { commands.add(created, component); }, value);
    }
    if (const auto result = m_world.commit(commands); !result) return "the items cannot go into the World";
    auto& staged = m_staged[group];
    for (const auto& item : items)
        staged.emplace_back(item.id, std::ranges::any_of(item.components, [](const ComponentValue& v) { return std::holds_alternative<ColliderComponent>(v); }));
    return {};
}

std::string WorldScatterSink::reveal(StageGroup group) {
    m_world.publish(group);
    const auto staged = m_staged.find(group);
    if (staged == m_staged.end()) return {};
    auto colliding = std::vector<EntityId>{};
    for (const auto& [id, collider] : staged->second)
        if (collider) colliding.push_back(id);
    m_staged.erase(staged);
    if (!m_physics || colliding.empty()) return {};
    auto authored = authored_physics(m_world, colliding);
    if (!authored) return authored.error;
    return m_physics->create_bodies(m_world, authored.bodies);
}

void WorldScatterSink::discard(StageGroup group) {
    m_staged.erase(group);
    m_world.discard(group);
}

void WorldScatterSink::remove(std::span<const EntityId> items) {
    auto handles = std::vector<EntityHandle>{};
    handles.reserve(items.size());
    for (const auto id : items)
        if (const auto handle = m_world.find(id)) handles.push_back(*handle);
    if (handles.empty()) return;
    if (m_physics) m_physics->remove_bodies(handles);
    auto commands = m_world.commands();
    for (const auto handle : handles) commands.destroy(handle);
    m_world.commit(commands);
}

bool WorldScatterSink::exists(EntityId id) const { return m_world.find(id).has_value(); }

struct ScatterStreamer::Impl {
    struct Result { // what a cell's job hands back
        std::optional<std::vector<ScatterItem>> items;
        bool cached = false;
        double ms = 0.0;
    };
    struct Cell {
        JobHandle job;
        std::shared_ptr<Result> result;
        uint64_t job_version = 0; // the generator version the job makes
        std::optional<std::vector<ScatterItem>> items; // the newest made, waiting to go in or in
        uint64_t items_version = 0;
        std::vector<EntityId> live; // the items in the sink now
        uint64_t live_version = 0;
        std::vector<SceneEntity> pending; // going in, over frames
        size_t next = 0;
        StageGroup group = 0;
        double activation_ms = 0.0;
        size_t skipped = 0, orphans = 0;
    };
    struct Generator {
        AssetId id;
        EntityId owner;
        std::shared_ptr<const GeneratorAsset> asset;
        uint64_t generation = 0, heights_generation = 0;
        std::shared_ptr<const GeneratorAsset> heights;
        std::vector<ScatterExclusion> exclusions;
        std::string exclusion_key; // all of them, as text: a change regenerates
        uint64_t version = 0; // increments with every change of the asset, its heights, or its exclusions
        std::string name;
        std::map<TileIndex, Cell> cells;
        bool seen = false;
    };

    std::shared_ptr<CookCache> cache;
    ScatterSettings settings;
    JobScope jobs;
    std::vector<math::DVec3> sources;
    std::function<bool(CellIndex)> ready = [](CellIndex) { return true; };
    std::map<AssetId, Generator> generators;
    std::unordered_set<EntityId, PersistentIdHash> made;
    std::set<EntityId> orphans;
    std::vector<std::string> reports;
    std::set<std::string> reported;
    StageGroup next_group = 0x5ca7'0000;
    double stage_us = 2.0, finish_us = 2.0, remove_us = 1.0; // measured cost per item, as frames plan
    bool frame_worked = false;
    ScatterStats* stats = nullptr;

    void report(std::string text) {
        if (reported.insert(text).second) reports.push_back(text);
        stats->last_error = std::move(text);
    }
    bool fits(Clock::time_point deadline, double cost_us) const {
        return !frame_worked || Clock::now() + std::chrono::duration<double, std::micro>(cost_us) <= deadline;
    }
    void start(Generator& generator, TileIndex index, Cell& cell) {
        cell.result = std::make_shared<Result>();
        cell.job_version = generator.version;
        const auto exclusions = reaching(generator.exclusions, index);
        const auto key = scatter_cell_cook_key(generator.id, *generator.asset, generator.heights.get(), index, exclusions);
        const auto rules = scatter_rules(*generator.asset);
        const auto id = std::array<uint64_t, 2>{generator.id.high, generator.id.low};
        auto heights = generator.heights && in_region(*generator.heights, index) ? generator.heights : nullptr;
        cell.job = jobs.submit(settings.streaming.tier, [result = cell.result, key, rules, id, index, exclusions, heights, cache = cache](JobContext& context) {
            const auto clock = Clock::now();
            if (cache)
                if (const auto bytes = cache->read(key))
                    if (auto items = decode_scatter_items(*bytes)) {
                        result->items = std::move(items);
                        result->cached = true;
                        result->ms = ms_since(clock);
                        return;
                    }
            // The cell's heights, as the terrain reads them: from the cook cache, or generated (and kept there).
            auto tile = std::optional<HeightTile>{};
            if (heights) {
                const auto tile_key = height_tile_cook_key(*heights, index);
                if (cache)
                    if (const auto bytes = cache->read(tile_key)) tile = decode_height_tile(*bytes);
                if (!tile) {
                    tile = generate_height_tile(heightfield_noise(*heights), index, &context);
                    if (!tile) return; // cancelled
                    if (cache) cache->write(tile_key, encode_height_tile(*tile));
                }
            }
            auto items = scatter_cell(id, rules, index, tile ? &*tile : nullptr, exclusions, &context);
            if (!items) return; // cancelled
            if (cache) cache->write(key, encode_scatter_items(*items));
            result->items = std::move(items);
            result->ms = ms_since(clock);
        });
    }
    void take_out(Cell& cell, ScatterSink& sink) {
        if (cell.group) sink.discard(cell.group);
        cell.group = 0;
        cell.pending.clear();
        cell.next = 0;
        if (!cell.live.empty()) {
            const auto clock = Clock::now();
            sink.remove(cell.live);
            remove_us = 0.8 * remove_us + 0.2 * (ms_since(clock) * 1000.0 / double(cell.live.size()));
            frame_worked = true;
            for (const auto id : cell.live) made.erase(id);
        }
        cell.live.clear();
        cell.live_version = 0;
    }
    void drop(Generator& generator, ScatterSink& sink) {
        for (auto& [index, cell] : generator.cells) {
            if (cell.job) cell.job.cancel();
            take_out(cell, sink);
        }
        generator.cells.clear();
    }
};

ScatterStreamer::ScatterStreamer(std::shared_ptr<CookCache> cache, ScatterSettings settings) : m_impl(std::make_unique<Impl>()) {
    m_impl->cache = std::move(cache);
    m_impl->settings = settings;
    m_impl->stats = &m_stats;
}

ScatterStreamer::~ScatterStreamer() {
    m_impl->jobs.cancel();
    m_impl->jobs.wait();
}

void ScatterStreamer::set_sources(std::vector<math::DVec3> positions) { m_impl->sources = std::move(positions); }
void ScatterStreamer::set_ready(std::function<bool(CellIndex)> ready) { m_impl->ready = ready ? std::move(ready) : [](CellIndex) { return true; }; }
void ScatterStreamer::wait_idle() { m_impl->jobs.wait(); }
bool ScatterStreamer::made(EntityId id) const { return m_impl->made.contains(id); }

void ScatterStreamer::release(EntityId id) {
    if (!m_impl->made.erase(id)) return;
    for (auto& [key, generator] : m_impl->generators)
        for (auto& [index, cell] : generator.cells)
            if (const auto found = std::ranges::find(cell.live, id); found != cell.live.end()) {
                cell.live.erase(found);
                ++cell.skipped;
                return;
            }
}

void ScatterStreamer::restore(EntityId id) {
    for (auto& [key, generator] : m_impl->generators)
        for (auto& [index, cell] : generator.cells)
            if (cell.items && std::ranges::any_of(*cell.items, [&](const ScatterItem& item) { return item.id == id; })) {
                cell.live_version = 0; // the next update takes its items out and puts them in again, this one too
                return;
            }
}

void ScatterStreamer::adopt(EntityId id) {
    if (m_impl->made.contains(id)) return;
    for (auto& [key, generator] : m_impl->generators)
        for (auto& [index, cell] : generator.cells)
            if (cell.items && cell.live_version == cell.items_version && !cell.group &&
                std::ranges::any_of(*cell.items, [&](const ScatterItem& item) { return item.id == id; })) {
                cell.live.push_back(id);
                m_impl->made.insert(id);
                if (cell.skipped > 0) --cell.skipped;
                return;
            }
}
const std::set<EntityId>& ScatterStreamer::orphans() const { return m_impl->orphans; }
std::vector<std::string> ScatterStreamer::take_reports() { return std::exchange(m_impl->reports, {}); }

void ScatterStreamer::clear(ScatterSink& sink) {
    auto& impl = *m_impl;
    for (auto& [id, generator] : impl.generators) impl.drop(generator, sink);
    impl.generators.clear();
    impl.made.clear();
    impl.orphans.clear();
    m_stats.items = m_stats.cells_active = m_stats.cells_loading = 0;
}

const ScatterStats& ScatterStreamer::update(const World& world, AssetRegistry& assets, ScatterSink& sink) {
    auto& impl = *m_impl;
    const auto began = Clock::now();
    const auto deadline = began + impl.settings.frame_budget;
    impl.frame_worked = false;
    // The World's scatter generators, each once, with their heights and the exclusions that clear them.
    for (auto& [id, generator] : impl.generators) generator.seen = false;
    auto exclusions = std::vector<std::pair<AssetId, ScatterExclusion>>{}; // per generator; an invalid ID clears every one
    world.for_each<ScatterExclusionComponent>([&](EntityHandle entity, const ScatterExclusionComponent& component) {
        const auto pose = world.world_matrix(entity);
        if (!pose) return;
        auto exclusion = ScatterExclusion{};
        exclusion.shape = component.shape == ExclusionShape::sphere ? ScatterExclusion::Shape::sphere : ScatterExclusion::Shape::box;
        exclusion.pose = *pose;
        exclusion.half_extents = component.half_extents;
        exclusion.radius = component.radius;
        exclusions.emplace_back(component.generator.id, exclusion);
    });
    world.for_each<GeneratorComponent>([&](EntityHandle entity, const GeneratorComponent& component) {
        if (!component.generator.valid()) return;
        const auto owner = world.persistent_id(entity).value_or(EntityId{});
        const auto lease = assets.try_acquire(component.generator);
        if (!lease) {
            if (lease.diagnostic.code != AssetError::loading) impl.report("Generator " + id_text(component.generator.id) + ": " + lease.diagnostic.message);
            if (const auto found = impl.generators.find(component.generator.id); found != impl.generators.end()) found->second.seen = true;
            return; // its items stay while it loads again
        }
        const auto& asset = lease.lease.value();
        if (asset.kind != GeneratorKind::scatter) return; // a generator component for heights draws nothing by itself
        auto [found, added] = impl.generators.try_emplace(component.generator.id);
        auto& generator = found->second;
        if (!added && generator.seen && generator.owner != owner) {
            impl.report("Two entities run the generator " + id_text(component.generator.id) + "; only the first scatters its items");
            return;
        }
        generator.seen = true;
        generator.id = component.generator.id;
        generator.owner = owner;
        auto heights = std::shared_ptr<const GeneratorAsset>{};
        auto heights_generation = uint64_t{0};
        if (asset.heights.valid()) {
            const auto heights_lease = assets.try_acquire(asset.heights);
            if (!heights_lease) {
                if (heights_lease.diagnostic.code != AssetError::loading)
                    impl.report("Generator " + id_text(component.generator.id) + "'s heights: " + heights_lease.diagnostic.message);
                return; // waits for its heights
            }
            if (heights_lease.lease.value().kind != GeneratorKind::heightfield_noise) {
                impl.report("Generator " + id_text(component.generator.id) + "'s heights name a generator that makes no heights");
                return;
            }
            heights = std::make_shared<const GeneratorAsset>(heights_lease.lease.value());
            heights_generation = heights_lease.lease.handle().generation;
        }
        auto relevant = std::vector<ScatterExclusion>{};
        auto key = std::string{};
        for (const auto& [target, exclusion] : exclusions)
            if (!target.valid() || target == generator.id) {
                relevant.push_back(exclusion);
                key += exclusion_text(exclusion);
            }
        const auto generation = lease.lease.handle().generation;
        if (added || generation != generator.generation || heights_generation != generator.heights_generation || key != generator.exclusion_key) {
            // A change: every cell is made again; what is in stays until its new items are ready.
            generator.asset = std::make_shared<const GeneratorAsset>(asset);
            generator.generation = generation;
            generator.heights = heights;
            generator.heights_generation = heights_generation;
            generator.exclusions = std::move(relevant);
            generator.exclusion_key = std::move(key);
            ++generator.version;
            generator.name = [&] {
                const auto info = assets.info(generator.id);
                return info ? info->record.path.stem().string() : std::string("Scatter");
            }();
            for (auto& [index, cell] : generator.cells)
                if (cell.job && !cell.job.done()) {
                    cell.job.cancel(); // superseded
                    ++m_stats.cancelled;
                }
        }
    });
    for (auto it = impl.generators.begin(); it != impl.generators.end();)
        if (!it->second.seen) {
            impl.drop(it->second, sink);
            it = impl.generators.erase(it);
        } else {
            ++it;
        }

    // Each generator's cells: those its region has within the activation radius of a source (and the hysteresis
    // for those in); made on jobs, nearest first, a few at a time.
    const auto& radii = impl.settings.streaming;
    auto in_flight = size_t{0};
    for (auto& [id, generator] : impl.generators)
        for (auto& [index, cell] : generator.cells) in_flight += cell.job && !cell.job.done();
    for (auto& [id, generator] : impl.generators) {
        if (!generator.asset) continue; // its heights are still loading
        const auto& asset = *generator.asset;
        const auto nearest = [&](TileIndex index) {
            auto best = std::numeric_limits<double>::max();
            for (const auto& source : impl.sources) best = std::min(best, distance_to(source, index));
            return best;
        };
        auto wanted = std::vector<std::pair<double, TileIndex>>{};
        for (const auto& source : impl.sources) {
            const auto reach = radii.activate_radius + radii.hysteresis;
            const auto low_x = std::max(asset.region_min_x, int32_t(std::floor((source.x - reach) / tile_size)));
            const auto high_x = std::min(asset.region_max_x, int32_t(std::floor((source.x + reach) / tile_size)));
            const auto low_z = std::max(asset.region_min_z, int32_t(std::floor((source.z - reach) / tile_size)));
            const auto high_z = std::min(asset.region_max_z, int32_t(std::floor((source.z + reach) / tile_size)));
            for (auto z = low_z; z <= high_z; ++z)
                for (auto x = low_x; x <= high_x; ++x) {
                    const auto index = TileIndex{x, z};
                    const auto distance = nearest(index);
                    if (distance <= radii.activate_radius || (generator.cells.contains(index) && distance <= reach))
                        wanted.emplace_back(distance, index);
                }
        }
        std::ranges::sort(wanted);
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        auto keep = std::set<TileIndex>{};
        for (const auto& [distance, index] : wanted) {
            keep.insert(index);
            auto& cell = generator.cells[index];
            // Finished jobs first: the newest version's items wait to go in.
            if (cell.job && cell.job.done()) {
                auto result = std::move(cell.result);
                const auto version = cell.job_version;
                cell.job = {};
                if (result && result->items && version == generator.version) {
                    if (cell.items_version) ++m_stats.regenerations;
                    cell.items = std::move(result->items);
                    cell.items_version = version;
                    ++(result->cached ? m_stats.cached : m_stats.generated);
                    m_stats.generate_ms = result->ms;
                    m_stats.longest_generate_ms = std::max(m_stats.longest_generate_ms, result->ms);
                }
            }
            if (!cell.job && cell.items_version != generator.version && in_flight < impl.settings.cells_in_flight) {
                impl.start(generator, index, cell);
                ++in_flight;
            }
        }
        for (auto it = generator.cells.begin(); it != generator.cells.end();) {
            if (keep.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.job) it->second.job.cancel();
            if (!it->second.live.empty() && !impl.fits(deadline, impl.remove_us * double(it->second.live.size()))) { // a later frame
                ++it;
                continue;
            }
            impl.take_out(it->second, sink);
            it = generator.cells.erase(it);
        }
    }

    // Into the sink, nearest first, within the budget: a cell whose authored content is in, with items newer than
    // what it shows. The old items go first (the same IDs), then the new ones over frames, shown at once.
    auto order = std::vector<std::tuple<double, AssetId, TileIndex>>{};
    for (auto& [id, generator] : impl.generators)
        for (auto& [index, cell] : generator.cells) {
            const auto ready = impl.ready(CellIndex{index.x, index.z});
            if (!ready) {
                if (!cell.live.empty() || cell.group) impl.take_out(cell, sink); // it goes with its cell
                continue;
            }
            // Going in over frames, or items newer than those shown.
            if (cell.group || (cell.items && cell.live_version != cell.items_version)) {
                auto best = std::numeric_limits<double>::max();
                for (const auto& source : impl.sources) best = std::min(best, distance_to(source, index));
                order.emplace_back(best, id, index);
            }
        }
    std::ranges::sort(order);
    for (const auto& [distance, id, index] : order) {
        auto& generator = impl.generators.at(id);
        auto& cell = generator.cells.at(index);
        if (cell.pending.empty() && !cell.group) {
            if (!cell.live.empty()) {
                if (!impl.fits(deadline, impl.remove_us * double(cell.live.size()))) break;
                impl.take_out(cell, sink);
            }
            // What goes in: the items no authored entity stands for. An authored override of this generator in
            // this cell whose item is not made is an orphan.
            auto ids = std::unordered_set<EntityId, PersistentIdHash>{};
            cell.skipped = 0;
            for (const auto& item : *cell.items) {
                ids.insert(item.id);
                if (sink.exists(item.id) && !impl.made.contains(item.id)) {
                    ++cell.skipped;
                    continue;
                }
                cell.pending.push_back(scatter_entity(generator.id, *generator.asset, item, generator.name));
            }
            cell.orphans = 0;
            world.for_each<GeneratedComponent>([&](EntityHandle entity, const GeneratedComponent& marker) {
                if (marker.generator.id != generator.id) return;
                const auto entity_id = world.persistent_id(entity);
                if (!entity_id || impl.made.contains(*entity_id) || ids.contains(*entity_id)) return;
                const auto pose = world.world_matrix(entity);
                if (!pose || cell_of(pose->translation) != CellIndex{index.x, index.z}) return;
                ++cell.orphans;
                if (impl.orphans.insert(*entity_id).second)
                    impl.report("An override of " + generator.name + "'s item " + id_text(*entity_id) + " in cell " +
                                cell_name({index.x, index.z}) + " stands for an item the generator no longer makes: it is kept as it is");
            });
            cell.next = 0;
            cell.activation_ms = 0.0;
            cell.live_version = cell.items_version;
            if (cell.pending.empty()) continue; // nothing to show: done
            cell.group = impl.next_group++;
        }
        auto stopped = false;
        while (cell.next < cell.pending.size()) {
            const auto count = std::min(impl.settings.chunk_items, cell.pending.size() - cell.next);
            if (!impl.fits(deadline, impl.stage_us * double(count))) {
                stopped = true;
                break;
            }
            const auto clock = Clock::now();
            if (auto error = sink.stage(cell.group, std::span(cell.pending).subspan(cell.next, count)); !error.empty()) {
                impl.report(generator.name + " in cell " + cell_name({index.x, index.z}) + ": " + error);
                sink.discard(cell.group);
                cell.group = 0;
                cell.pending.clear();
                stopped = true;
                break;
            }
            const auto took = ms_since(clock);
            cell.activation_ms += took;
            impl.stage_us = 0.8 * impl.stage_us + 0.2 * (took * 1000.0 / double(count));
            impl.frame_worked = true;
            cell.next += count;
        }
        if (stopped || cell.pending.empty()) {
            if (stopped) break;
            continue;
        }
        if (!impl.fits(deadline, impl.finish_us * double(cell.pending.size()))) break;
        const auto clock = Clock::now();
        if (auto error = sink.reveal(cell.group); !error.empty()) impl.report(generator.name + " in cell " + cell_name({index.x, index.z}) + ": " + error);
        const auto took = ms_since(clock);
        impl.finish_us = 0.8 * impl.finish_us + 0.2 * (took * 1000.0 / double(cell.pending.size()));
        impl.frame_worked = true;
        cell.activation_ms += took;
        m_stats.longest_activation_ms = std::max(m_stats.longest_activation_ms, cell.activation_ms);
        for (const auto& entity : cell.pending) {
            cell.live.push_back(entity.id);
            impl.made.insert(entity.id);
        }
        cell.pending.clear();
        cell.group = 0;
        cell.next = 0;
    }

    // The counts.
    m_stats.generators = impl.generators.size();
    m_stats.cells_loading = m_stats.cells_active = m_stats.items = m_stats.skipped = m_stats.orphans = m_stats.regenerating = 0;
    m_stats.cells_waiting = 0;
    for (const auto& [id, generator] : impl.generators)
        for (const auto& [index, cell] : generator.cells) {
            if (cell.job && !cell.job.done()) ++m_stats.cells_loading;
            if (!cell.live.empty()) ++m_stats.cells_active;
            if (!cell.live.empty() && cell.live_version != generator.version) ++m_stats.regenerating;
            if (cell.items_version != generator.version || cell.group ||
                (impl.ready(CellIndex{index.x, index.z}) && cell.live_version != cell.items_version))
                ++m_stats.cells_waiting;
            m_stats.items += cell.live.size();
            m_stats.skipped += cell.skipped;
            m_stats.orphans += cell.orphans;
        }
    m_stats.frame_ms = ms_since(began);
    m_stats.longest_frame_ms = std::max(m_stats.longest_frame_ms, m_stats.frame_ms);
    return m_stats;
}

const ScatterStats& ScatterStreamer::settle(const World& world, AssetRegistry& assets, ScatterSink& sink, size_t frames) {
    const auto budget = m_impl->settings.frame_budget;
    m_impl->settings.frame_budget = std::chrono::hours(1); // a loading screen spends what it takes
    for (size_t frame = 0; frame < frames; ++frame) {
        assets.update();
        assets.wait_idle();
        wait_idle();
        update(world, assets, sink);
        auto busy = false;
        for (const auto& [id, generator] : m_impl->generators) {
            busy = busy || !generator.asset; // its heights are still loading
            for (const auto& [index, cell] : generator.cells)
                busy = busy || cell.job || cell.group || !cell.pending.empty() || cell.items_version != generator.version ||
                       (m_impl->ready(CellIndex{index.x, index.z}) && cell.live_version != cell.items_version);
        }
        if (!busy && frame > 0) break;
    }
    m_impl->settings.frame_budget = budget;
    return m_stats;
}

} // namespace maya
