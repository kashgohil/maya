#pragma once
// Terrain streaming (#1068, docs/procedural.md#streaming): the tiles of every terrain in a World, made from
// its height-field generator, near the places the World is seen from. A tile is generated on a job (or read
// from the cook cache) and drawn (its packed heights on the device) within the loading radius, since the
// finest levels reach 384 m, and collided with (a height field in the physics world) within the activation
// radius; a few tiles a frame within a time budget, as cells are (#1064). Beyond the tiles, a terrain is
// drawn from its overview, generated once.

#include "maya/assets/cook_cache.hpp"
#include "maya/assets/generator.hpp"
#include "maya/assets/registry.hpp"
#include "maya/jobs/jobs.hpp"
#include "maya/physics/physics.hpp"
#include "maya/procedural/height_field.hpp"
#include "maya/renderer/terrain.hpp"
#include "maya/streaming/world_streamer.hpp"
#include "maya/world/components.hpp"
#include <chrono>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace maya {

/// Cook versions of the height field's cooked forms: raise them when their bytes change.
inline constexpr uint32_t height_tile_cook_version = 1;
inline constexpr uint32_t height_overview_cook_version = 1;
/// The key of a tile's heights: the generator's noise parameters and algorithm version, and the tile; not its
/// region, so widening a region keeps the tiles already cooked.
CookKey height_tile_cook_key(const GeneratorAsset& generator, TileIndex tile);
/// The key of a generator's overview: its noise parameters, algorithm version, and region.
CookKey height_overview_cook_key(const GeneratorAsset& generator);
/// Every key the generator's output could have: its overview and each tile of its region (for pruning).
void append_generator_cook_keys(const GeneratorAsset& generator, std::vector<CookKey>& keys);
/// Whether a tile is in the generator's region.
bool in_region(const GeneratorAsset& generator, TileIndex tile) noexcept;

struct TerrainSettings {
    StreamingSettings streaming; // the activation radius, the hysteresis, and the job tier, as cells use them
    /// Tiles load and are drawn this far from a source (at least): as far as the finest levels can read
    /// them (terrain_tile_reach, 874 m), and the cells' loading radius if that is farther.
    double tile_radius = 896.0;
    bool collide = false; // height fields in the physics world (play); the editor draws only
    std::chrono::microseconds frame_budget{500}; // owner-thread time a frame spends activating and deactivating tiles
    size_t frame_tiles = 16; // most tiles activated a frame
    size_t tiles_in_flight = 16; // most tiles generating at once
};

struct TerrainStats {
    size_t terrains = 0;
    size_t loading = 0, loaded = 0, active = 0; // tiles by state (active ones are loaded too)
    size_t drawn = 0; // loaded tiles whose heights are on the device
    size_t generated = 0, cached = 0; // tiles made since the streamer started: by generating, and read from the cook cache
    size_t activated = 0, deactivated = 0, uploaded = 0; // in the last update (a tile joining a physics world it had none of is activated)
    size_t height_fields = 0;
    size_t cpu_bytes = 0, gpu_bytes = 0; // held now: heights and normals, and device buffers
    double update_ms = 0.0, longest_update_ms = 0.0;
    double generate_ms = 0.0, longest_generate_ms = 0.0; // the last generated tile's job time, and the longest
};

class TerrainStreamer {
public:
    /// Without a device, tiles are not drawn (tests, servers); without a cache, every tile is generated.
    TerrainStreamer(GraphicsDevice* device, std::shared_ptr<CookCache> cache, TerrainSettings settings = {});
    /// Cancels its jobs and waits for them. Call clear() first to take its height fields out of a physics
    /// world that outlives it.
    ~TerrainStreamer();
    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    /// Where the terrain is seen from: the camera, the player.
    void set_sources(std::vector<math::DVec3> positions);
    /// One frame on the owner thread: finds the World's terrains (a changed component or generator starts
    /// them again), applies finished tiles, starts the ones now wanted, and activates and deactivates within
    /// the frame's budget. `physics` takes the height fields when the settings collide.
    const TerrainStats& update(const World& world, AssetRegistry& assets, PhysicsWorld* physics);
    /// Takes every tile out, and their height fields out of `physics`: what closing a scene or stopping Play does.
    void clear(PhysicsWorld* physics);
    std::span<const TerrainView> terrains() const noexcept { return m_views; }
    /// The height of the loaded terrain at a point, between its samples (none where no tile is loaded).
    std::optional<float> height_at(double x, double z) const;
    /// Waits for every tile job in flight (tests and loading screens).
    void wait_idle();
    /// A loading screen: updates, waiting for generators and tiles, until nothing more would change around
    /// the sources now (or `frames` updates have passed).
    const TerrainStats& settle(const World& world, AssetRegistry& assets, PhysicsWorld* physics, size_t frames = 1000);
    const TerrainStats& stats() const noexcept { return m_stats; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<TerrainView> m_views;
    TerrainStats m_stats;
};

} // namespace maya
