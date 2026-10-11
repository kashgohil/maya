#include "maya/streaming/terrain_streamer.hpp"
#include "maya/procedural/scatter.hpp"
#include "maya/core/sha256.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <string>

namespace maya {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
std::string number(float value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr;
    return {text, end};
}
/// What a tile's heights depend on: the kind's algorithm version and its noise parameters, exactly.
Sha256Digest noise_digest(const GeneratorAsset& g) {
    const auto text = "heightfield.noise " + std::to_string(g.version) + "\nseed " + std::to_string(g.seed) + "\noctaves " +
                      std::to_string(g.octaves) + "\nwavelength " + number(g.wavelength) + "\namplitude " + number(g.amplitude) +
                      "\nwarp " + number(g.warp) + "\nbase " + number(g.base) + "\n";
    return sha256(std::as_bytes(std::span(text.data(), text.size())));
}
/// Whether two terrain components make the same heights (their layers are read when drawn).
bool same_heights(const TerrainComponent& a, const TerrainComponent& b) noexcept { return a.heights == b.heights; }
/// Metres from a point to a tile's square, in x and z.
double distance_to(const math::DVec3& point, TileIndex tile) {
    const auto x0 = double(tile.x) * tile_size, z0 = double(tile.z) * tile_size;
    const auto dx = std::max({x0 - point.x, 0.0, point.x - (x0 + tile_size)});
    const auto dz = std::max({z0 - point.z, 0.0, point.z - (z0 + tile_size)});
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

CookKey height_tile_cook_key(const GeneratorAsset& generator, TileIndex tile) {
    return {"height-tile", height_tile_cook_version, noise_digest(generator),
            "tile " + std::to_string(tile.x) + " " + std::to_string(tile.z) + "\n"};
}
CookKey height_overview_cook_key(const GeneratorAsset& generator) {
    return {"height-overview", height_overview_cook_version, noise_digest(generator),
            "region " + std::to_string(generator.region_min_x) + " " + std::to_string(generator.region_min_z) + " " +
                std::to_string(generator.region_max_x) + " " + std::to_string(generator.region_max_z) + "\n"};
}
void append_generator_cook_keys(const GeneratorAsset& generator, std::vector<CookKey>& keys) {
    if (generator.kind != GeneratorKind::heightfield_noise) return; // only heights have tiles and an overview
    keys.push_back(height_overview_cook_key(generator));
    for (auto z = generator.region_min_z; z <= generator.region_max_z; ++z)
        for (auto x = generator.region_min_x; x <= generator.region_max_x; ++x) keys.push_back(height_tile_cook_key(generator, {x, z}));
}
bool in_region(const GeneratorAsset& generator, TileIndex tile) noexcept {
    return tile.x >= generator.region_min_x && tile.x <= generator.region_max_x && tile.z >= generator.region_min_z &&
           tile.z <= generator.region_max_z;
}

struct TerrainStreamer::Impl {
    enum class State { loading, loaded, active };
    struct Result { // what a tile's job hands back
        std::shared_ptr<const HeightTile> tile;
        std::shared_ptr<const PreparedHeightField> field;
        bool cached = false;
        double ms = 0.0;
    };
    struct Tile {
        State state = State::loading;
        JobHandle job;
        std::shared_ptr<Result> result;
        std::shared_ptr<const HeightTile> heights;
        std::shared_ptr<const PreparedHeightField> field;
        std::shared_ptr<const HeightBuffer> buffer;
        uint64_t physics_field = 0;
    };
    struct Terrain {
        EntityHandle entity;
        EntityId id;
        TerrainComponent component;
        std::shared_ptr<const GeneratorAsset> generator;
        uint64_t generation = 0; // the generator version's, to notice a reload or an edit
        std::map<TileIndex, Tile> tiles;
        JobHandle overview_job;
        std::shared_ptr<std::shared_ptr<const HeightOverview>> overview_result;
        std::shared_ptr<const HeightOverview> overview;
        std::shared_ptr<const HeightBuffer> overview_buffer;
        /// What was drawn before its generator changed (#1093): drawn until the new tiles are ready.
        std::optional<TerrainView> previous;
    };

    GraphicsDevice* device;
    std::shared_ptr<CookCache> cache;
    TerrainSettings settings;
    JobScope jobs;
    std::vector<math::DVec3> sources;
    std::map<std::pair<uint32_t, uint64_t>, Terrain> terrains; // by entity slot and generation
    PhysicsWorld* physics = nullptr; // the one holding the height fields
    uint64_t world = 0;

    Impl(GraphicsDevice* d, std::shared_ptr<CookCache> c, TerrainSettings s) : device(d), cache(std::move(c)), settings(s) {}

    /// A tile's job: read from the cook cache or generate (and write it there), then build its height field.
    void start(Terrain& terrain, TileIndex index, Tile& tile) {
        tile.result = std::make_shared<Result>();
        const auto noise = heightfield_noise(*terrain.generator);
        const auto key = height_tile_cook_key(*terrain.generator, index);
        // Shapes only when a physics world will take them (Play); the editor's authored scene draws only.
        tile.job = jobs.submit(settings.streaming.tier, [result = tile.result, noise, key, index, cache = cache,
                                                          collide = settings.collide && physics](JobContext& context) {
            const auto clock = Clock::now();
            auto heights = std::optional<HeightTile>{};
            if (cache)
                if (const auto bytes = cache->read(key)) heights = decode_height_tile(*bytes);
            result->cached = heights.has_value();
            if (!heights) {
                heights = generate_height_tile(noise, index, &context);
                if (!heights) return; // cancelled
                if (cache) cache->write(key, encode_height_tile(*heights));
            }
            if (context.cancelled()) return;
            auto shared = std::make_shared<const HeightTile>(std::move(*heights));
            if (collide) {
                const auto origin = math::DVec3{double(index.x) * tile_size, 0.0, double(index.z) * tile_size};
                result->field = PhysicsWorld::prepare_height_field({origin, tile_samples, 1.0f, shared->heights});
            }
            result->tile = std::move(shared);
            result->ms = milliseconds_since(clock);
        });
    }
    void start_overview(Terrain& terrain) {
        terrain.overview_result = std::make_shared<std::shared_ptr<const HeightOverview>>();
        const auto generator = *terrain.generator;
        terrain.overview_job = jobs.submit(settings.streaming.tier, [result = terrain.overview_result, generator, cache = cache](JobContext& context) {
            const auto key = height_overview_cook_key(generator);
            auto overview = std::optional<HeightOverview>{};
            if (cache)
                if (const auto bytes = cache->read(key)) overview = decode_height_overview(*bytes);
            if (!overview) {
                overview = generate_height_overview(heightfield_noise(generator), {generator.region_min_x, generator.region_min_z},
                                                    {generator.region_max_x, generator.region_max_z}, &context);
                if (!overview) return;
                if (cache) cache->write(key, encode_height_overview(*overview));
            }
            *result = std::make_shared<const HeightOverview>(std::move(*overview));
        });
    }
    /// Takes a terrain's tiles out: their jobs cancelled, their height fields removed in one batch.
    void drop(Terrain& terrain) {
        auto fields = std::vector<uint64_t>{};
        for (auto& [index, tile] : terrain.tiles) {
            if (tile.job) tile.job.cancel();
            if (tile.physics_field) fields.push_back(tile.physics_field);
        }
        if (terrain.overview_job) terrain.overview_job.cancel();
        if (physics && !fields.empty()) physics->remove_height_fields(fields);
        terrain.tiles.clear();
    }
    double nearest(TileIndex index) const {
        auto best = std::numeric_limits<double>::infinity();
        for (const auto& source : sources) best = std::min(best, distance_to(source, index));
        return best;
    }
};

TerrainStreamer::TerrainStreamer(GraphicsDevice* device, std::shared_ptr<CookCache> cache, TerrainSettings settings)
    : m_impl(std::make_unique<Impl>(device, std::move(cache), settings)) {}

TerrainStreamer::~TerrainStreamer() {
    m_impl->jobs.cancel();
    m_impl->jobs.wait();
}

void TerrainStreamer::set_sources(std::vector<math::DVec3> positions) { m_impl->sources = std::move(positions); }

void TerrainStreamer::wait_idle() { m_impl->jobs.wait(); }

const TerrainStats& TerrainStreamer::settle(const World& world, AssetRegistry& assets, PhysicsWorld* physics, size_t frames) {
    const auto budget = m_impl->settings.frame_budget;
    m_impl->settings.frame_budget = std::chrono::hours(1); // a loading screen spends what it takes
    for (size_t frame = 0; frame < frames; ++frame) {
        assets.update();
        assets.wait_idle();
        wait_idle();
        const auto& stats = update(world, assets, physics);
        const auto pending_overview = std::ranges::any_of(m_views, [](const TerrainView& view) { return !view.overview; });
        if (stats.loading == 0 && stats.activated == 0 && stats.uploaded == 0 && stats.deactivated == 0 && !pending_overview &&
            m_views.size() == stats.terrains && frame > 0)
            break;
    }
    m_impl->settings.frame_budget = budget;
    return m_stats;
}

void TerrainStreamer::clear(PhysicsWorld* physics) {
    auto& impl = *m_impl;
    if (physics) impl.physics = physics;
    for (auto& [key, terrain] : impl.terrains) impl.drop(terrain);
    impl.terrains.clear();
    m_views.clear();
    m_stats.loading = m_stats.loaded = m_stats.active = m_stats.height_fields = 0;
    m_stats.cpu_bytes = m_stats.gpu_bytes = 0;
    m_stats.terrains = 0;
}

const TerrainStats& TerrainStreamer::update(const World& world, AssetRegistry& assets, PhysicsWorld* physics) {
    auto& impl = *m_impl;
    const auto clock = Clock::now();
    if (impl.world != world.token()) { // another World: nothing carries over
        clear(impl.physics);
        impl.world = world.token();
    }
    if (impl.physics && physics != impl.physics) { // another physics world: its height fields went with it
        for (auto& [key, terrain] : impl.terrains)
            for (auto& [index, tile] : terrain.tiles) tile.physics_field = 0;
    }
    impl.physics = physics;
    const auto collide = impl.settings.collide && physics;

    // The World's terrains, and their generators as resident now (a load is started, never waited for).
    auto seen = std::set<std::pair<uint32_t, uint64_t>>{};
    world.for_each<TerrainComponent>([&](EntityHandle entity, const TerrainComponent& component) {
        const auto key = std::pair{entity.slot, entity.generation};
        seen.insert(key);
        auto& terrain = impl.terrains[key];
        const auto lease = component.heights.valid() ? assets.try_acquire(component.heights) : AssetResult<GeneratorAsset>{};
        const auto generation = lease ? lease.lease.handle().generation : 0;
        const auto changed = terrain.entity != entity || terrain.generation != generation ||
                             !same_heights(terrain.component, component);
        if (changed) {
            // The terrain as it was keeps being drawn (not collided with) until the new one is ready.
            auto previous = std::optional<TerrainView>{};
            if (terrain.generator && terrain.overview_buffer && terrain.entity == entity) {
                previous = TerrainView{terrain.entity, terrain.id, terrain.component, *terrain.generator, terrain.overview, terrain.overview_buffer, {}};
                for (const auto& [index, tile] : terrain.tiles)
                    if (tile.state != Impl::State::loading && tile.buffer) previous->tiles.push_back({index, tile.heights, tile.buffer});
            } else if (terrain.previous && terrain.entity == entity) {
                previous = std::move(terrain.previous); // changed again before the last change was ready
            }
            impl.drop(terrain);
            terrain = {};
            terrain.previous = std::move(previous);
            terrain.entity = entity;
            terrain.id = world.persistent_id(entity).value_or(EntityId{});
            terrain.component = component;
            terrain.generation = generation;
            terrain.generator = lease ? std::make_shared<const GeneratorAsset>(lease.lease.value()) : nullptr;
            if (terrain.generator) impl.start_overview(terrain);
        }
        terrain.component = component; // its layers, drawn as they are now
    });
    for (auto it = impl.terrains.begin(); it != impl.terrains.end();)
        if (!seen.contains(it->first)) {
            impl.drop(it->second);
            it = impl.terrains.erase(it);
        } else {
            ++it;
        }

    const auto& radii = impl.settings.streaming;
    const auto load_radius = std::max(radii.load_radius, impl.settings.tile_radius);
    auto in_flight = size_t{0};
    for (const auto& [key, terrain] : impl.terrains)
        for (const auto& [index, tile] : terrain.tiles) in_flight += tile.state == Impl::State::loading && tile.job;
    auto activated = size_t{0}, deactivated = size_t{0};
    auto removed = std::vector<uint64_t>{};
    for (auto& [key, terrain] : impl.terrains) {
        if (!terrain.generator) continue;
        const auto& generator = *terrain.generator;
        // The overview, once generated.
        if (!terrain.overview && terrain.overview_job && terrain.overview_job.done() && *terrain.overview_result) {
            terrain.overview = *terrain.overview_result;
            if (impl.device) {
                const auto packed = pack_heights(terrain.overview->heights, terrain.overview->normals);
                terrain.overview_buffer = std::make_shared<const HeightBuffer>(*impl.device, packed, "terrain overview");
            }
        }
        // Finished tiles.
        for (auto& [index, tile] : terrain.tiles)
            if (tile.state == Impl::State::loading && tile.job && tile.job.done()) {
                --in_flight;
                if (!tile.result->tile) { // cancelled or failed: tried again if still wanted
                    tile.job = {};
                    tile.result = {};
                    continue;
                }
                tile.heights = tile.result->tile;
                tile.field = tile.result->field;
                tile.state = Impl::State::loaded;
                ++(tile.result->cached ? m_stats.cached : m_stats.generated);
                if (!tile.result->cached) {
                    m_stats.generate_ms = tile.result->ms;
                    m_stats.longest_generate_ms = std::max(m_stats.longest_generate_ms, tile.result->ms);
                }
                tile.job = {};
                tile.result = {};
            }
        // What each source wants: tiles of the region within the loading radius, nearest first.
        auto wanted = std::vector<std::pair<double, TileIndex>>{};
        for (const auto& source : impl.sources) {
            const auto reach = load_radius + radii.hysteresis;
            const auto low_x = std::max(generator.region_min_x, int32_t(std::floor((source.x - reach) / tile_size)));
            const auto high_x = std::min(generator.region_max_x, int32_t(std::floor((source.x + reach) / tile_size)));
            const auto low_z = std::max(generator.region_min_z, int32_t(std::floor((source.z - reach) / tile_size)));
            const auto high_z = std::min(generator.region_max_z, int32_t(std::floor((source.z + reach) / tile_size)));
            for (auto z = low_z; z <= high_z; ++z)
                for (auto x = low_x; x <= high_x; ++x)
                    if (const auto distance = impl.nearest({x, z}); distance <= load_radius) wanted.push_back({distance, {x, z}});
        }
        std::ranges::sort(wanted);
        for (const auto& [distance, index] : wanted) {
            auto& tile = terrain.tiles[index];
            if (tile.state == Impl::State::loading && !tile.job) {
                if (in_flight >= impl.settings.tiles_in_flight) continue;
                impl.start(terrain, index, tile);
                ++in_flight;
            }
        }
        // Beyond the radii and their hysteresis: deactivate, unload.
        for (auto it = terrain.tiles.begin(); it != terrain.tiles.end();) {
            auto& [index, tile] = *it;
            const auto distance = impl.nearest(index);
            if (distance > load_radius + radii.hysteresis) {
                if (tile.job) tile.job.cancel();
                if (tile.physics_field) removed.push_back(tile.physics_field);
                if (tile.state == Impl::State::active) ++deactivated;
                it = terrain.tiles.erase(it);
                continue;
            }
            if (tile.state == Impl::State::active && distance > radii.activate_radius + radii.hysteresis) {
                if (tile.physics_field) removed.push_back(tile.physics_field);
                tile.physics_field = 0;
                tile.state = Impl::State::loaded; // still drawn: its buffer stays while it is loaded
                ++deactivated;
            }
            ++it;
        }
    }
    if (physics && !removed.empty()) physics->remove_height_fields(removed);

    // Nearest first, within the frame's budget: loaded tiles get their buffer (drawn within the loading
    // radius, since the finest levels reach 384 m), and those within the activation radius are activated
    // (collided with).
    auto ready = std::vector<std::tuple<double, Impl::Terrain*, TileIndex>>{};
    for (auto& [key, terrain] : impl.terrains)
        for (auto& [index, tile] : terrain.tiles) {
            if (tile.state == Impl::State::loading) continue;
            const auto distance = impl.nearest(index);
            const auto upload = impl.device && !tile.buffer;
            const auto activate = distance <= radii.activate_radius &&
                                  (tile.state == Impl::State::loaded || (collide && !tile.physics_field));
            if (upload || activate) ready.push_back({distance, &terrain, index});
        }
    std::ranges::sort(ready, {}, [](const auto& entry) { return std::get<0>(entry); });
    const auto budget_start = Clock::now();
    auto worked = size_t{0}, uploaded = size_t{0};
    for (const auto& [distance, terrain, index] : ready) {
        if (worked >= impl.settings.frame_tiles || Clock::now() - budget_start >= impl.settings.frame_budget) break;
        ++worked;
        auto& tile = terrain->tiles.at(index);
        if (impl.device && !tile.buffer) {
            const auto packed = pack_heights(tile.heights->heights, tile.heights->normals);
            tile.buffer = std::make_shared<const HeightBuffer>(*impl.device, packed,
                                                               "terrain tile " + std::to_string(index.x) + "_" + std::to_string(index.z));
            ++uploaded;
        }
        if (distance > radii.activate_radius) continue;
        if (collide && !tile.field) { // loaded before a physics world came: built now, within the budget
            const auto origin = math::DVec3{double(index.x) * tile_size, 0.0, double(index.z) * tile_size};
            tile.field = PhysicsWorld::prepare_height_field({origin, tile_samples, 1.0f, tile.heights->heights});
        }
        auto joined = false; // a tile already active without physics joins it now: an activation too
        if (collide && tile.field && !tile.physics_field && world.alive(terrain->entity)) {
            tile.physics_field = physics->add_height_field(world, terrain->entity, tile.field);
            joined = true;
        }
        if (tile.state != Impl::State::active || joined) ++activated;
        tile.state = Impl::State::active;
    }

    // What the renderer and picking read, and the counts.
    m_views.clear();
    m_stats.terrains = impl.terrains.size();
    m_stats.loading = m_stats.loaded = m_stats.active = 0;
    m_stats.cpu_bytes = m_stats.gpu_bytes = 0;
    m_stats.height_fields = 0;
    m_stats.drawn = 0;
    m_stats.regenerating = 0;
    for (auto& [key, terrain] : impl.terrains) {
        if (terrain.previous) { // until the new terrain is ready: its overview, and every wanted tile loaded and on the device
            const auto ready = terrain.generator && terrain.overview && (!impl.device || terrain.overview_buffer) &&
                               std::ranges::none_of(terrain.tiles, [&](const auto& entry) {
                                   return entry.second.state == Impl::State::loading || (impl.device && !entry.second.buffer);
                               });
            if (ready) terrain.previous.reset();
        }
        if (terrain.previous) {
            auto shown = *terrain.previous;
            shown.component = terrain.component; // its layers as they are now
            m_views.push_back(std::move(shown));
            ++m_stats.regenerating;
            for (const auto& [index, tile] : terrain.tiles) m_stats.loading += tile.state == Impl::State::loading;
            continue;
        }
        if (!terrain.generator) continue;
        auto view = TerrainView{terrain.entity, terrain.id, terrain.component, *terrain.generator, terrain.overview, terrain.overview_buffer, {}};
        if (terrain.overview) m_stats.cpu_bytes += terrain.overview->heights.size() * 6;
        if (terrain.overview_buffer) m_stats.gpu_bytes += terrain.overview_buffer->bytes();
        for (const auto& [index, tile] : terrain.tiles) {
            switch (tile.state) {
            case Impl::State::loading: ++m_stats.loading; break;
            case Impl::State::loaded: ++m_stats.loaded; break;
            case Impl::State::active: ++m_stats.loaded; ++m_stats.active; break;
            }
            if (tile.heights) m_stats.cpu_bytes += tile.heights->heights.size() * (sizeof(float) + sizeof(uint16_t));
            if (tile.buffer) m_stats.gpu_bytes += tile.buffer->bytes();
            m_stats.height_fields += tile.physics_field != 0;
            m_stats.drawn += tile.buffer != nullptr;
            if (tile.state != Impl::State::loading) view.tiles.push_back({index, tile.heights, tile.buffer});
        }
        m_views.push_back(std::move(view));
    }
    m_stats.activated = activated;
    m_stats.uploaded = uploaded;
    m_stats.deactivated = deactivated;
    m_stats.update_ms = milliseconds_since(clock);
    m_stats.longest_update_ms = std::max(m_stats.longest_update_ms, m_stats.update_ms);
    return m_stats;
}

std::optional<float> TerrainStreamer::height_at(double x, double z) const {
    const auto index = TileIndex{int32_t(std::floor(x / tile_size)), int32_t(std::floor(z / tile_size))};
    for (const auto& [key, terrain] : m_impl->terrains) {
        const auto found = terrain.tiles.find(index);
        if (found == terrain.tiles.end() || !found->second.heights) continue;
        // On the triangles the terrain draws (scatter stands its items on the same ones).
        return tile_height_at(*found->second.heights, x - double(index.x) * tile_size, z - double(index.z) * tile_size);
    }
    return std::nullopt;
}

} // namespace maya
