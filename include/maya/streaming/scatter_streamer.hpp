#pragma once
// Scatter streaming (#1093, docs/procedural.md#scatter): the items of every scatter generator a World runs
// (`maya.generator`), as entities, near the places it is seen from. A cell's items are generated on a job (or
// read from the cook cache), then put into the World through a sink within a frame budget, all at once, and
// taken out when the sources move away. An item an authored entity already stands for (an override, a
// removed item, or a baked one: the same ID) is not made; an override whose item the generator no longer
// makes is an orphan, and is reported.

#include "maya/assets/cook_cache.hpp"
#include "maya/assets/generator.hpp"
#include "maya/assets/registry.hpp"
#include "maya/jobs/jobs.hpp"
#include "maya/procedural/scatter.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/streaming/world_streamer.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace maya {
class PhysicsWorld;

/// Cook version of scatter's cooked cells: raise it when their bytes change.
inline constexpr uint32_t scatter_cook_version = 1;
/// The key of a scatter cell: the generator's ID and scatter parameters, its heights' tile key, and the
/// exclusions that reach the cell.
CookKey scatter_cell_cook_key(AssetId generator, const GeneratorAsset& scatter, const GeneratorAsset* heights, TileIndex cell,
                              std::span<const ScatterExclusion> exclusions);
/// The entity of an item: its name, transform, mesh, sphere collider (when its mesh has one), and the
/// `maya.generated` marker naming its generator.
SceneEntity scatter_entity(AssetId generator, const GeneratorAsset& scatter, const ScatterItem& item, const std::string& name);

/// Where items go: the play World (WorldScatterSink), or the editor's scene, outside its history.
class ScatterSink {
public:
    virtual ~ScatterSink() = default;
    /// Commits items, invisible, into stage group `group` (any number but 0). Empty, or why not.
    virtual std::string stage(StageGroup group, std::span<const SceneEntity> items) = 0;
    /// Shows a staged group's items together (and gives them their colliders).
    virtual std::string reveal(StageGroup group) = 0;
    virtual void discard(StageGroup group) = 0;
    /// Takes items out (and their colliders).
    virtual void remove(std::span<const EntityId> items) = 0;
    /// Whether an entity with this ID is in, made by anyone.
    virtual bool exists(EntityId id) const = 0;
};
/// Items into a World, with static colliders into its physics world when there is one.
class WorldScatterSink final : public ScatterSink {
public:
    WorldScatterSink(World& world, PhysicsWorld* physics) : m_world(world), m_physics(physics) {}
    std::string stage(StageGroup group, std::span<const SceneEntity> items) override;
    std::string reveal(StageGroup group) override;
    void discard(StageGroup group) override;
    void remove(std::span<const EntityId> items) override;
    bool exists(EntityId id) const override;

private:
    World& m_world;
    PhysicsWorld* m_physics;
    std::map<StageGroup, std::vector<std::pair<EntityId, bool>>> m_staged; // and whether each has a collider
};

struct ScatterSettings {
    StreamingSettings streaming; // the activation radius and hysteresis (items stream as cells activate), and the job tier
    std::chrono::microseconds frame_budget{1000}; // owner-thread time a frame spends putting items in and taking them out
    size_t chunk_items = 256; // most items committed at once
    size_t cells_in_flight = 8; // most cells generating at once
};

struct ScatterStats {
    size_t generators = 0;
    size_t cells_loading = 0, cells_active = 0;
    size_t items = 0; // in now, made by the streamer
    size_t generated = 0, cached = 0; // cells made since it started: by generating, and read from the cook cache
    size_t regenerations = 0, cancelled = 0; // cells made again after a change; jobs superseded by a newer change
    size_t skipped = 0; // items not made, an authored entity standing for them, in the cells now in
    size_t orphans = 0; // overrides whose item is no longer made, in the cells now in
    size_t regenerating = 0; // cells whose items are of an older version, while the new ones are made
    size_t cells_waiting = 0; // cells wanted whose newest items are not yet made or not yet all in (0: settled)
    double frame_ms = 0.0, longest_frame_ms = 0.0;
    double generate_ms = 0.0, longest_generate_ms = 0.0; // the last cell's job time, and the longest
    double longest_activation_ms = 0.0; // a cell's owner-thread time going in, over its frames
    std::string last_error;
};

class ScatterStreamer {
public:
    /// Without a cache, every cell is generated.
    explicit ScatterStreamer(std::shared_ptr<CookCache> cache, ScatterSettings settings = {});
    /// Cancels its jobs and waits for them. The sink's items stay; clear() takes them out.
    ~ScatterStreamer();
    ScatterStreamer(const ScatterStreamer&) = delete;
    ScatterStreamer& operator=(const ScatterStreamer&) = delete;

    void set_sources(std::vector<math::DVec3> positions);
    /// Whether a cell's authored content is in (a world's cell, in the player's streamer or the editor's): its
    /// items wait for it, and go when it goes, so an override and its item are never in at once. Every cell by
    /// default (a plain scene).
    void set_ready(std::function<bool(CellIndex)> ready);
    /// One frame: finds the World's scatter generators and exclusions (a change regenerates their cells, the
    /// old items staying until the new ones are ready), applies finished cells, and puts items in and takes them
    /// out within the frame's budget.
    const ScatterStats& update(const World& world, AssetRegistry& assets, ScatterSink& sink);
    /// Everything wanted now, waiting for generators and cells: a loading screen.
    const ScatterStats& settle(const World& world, AssetRegistry& assets, ScatterSink& sink, size_t frames = 1000);
    /// Takes every item out: closing a scene, stopping Play.
    void clear(ScatterSink& sink);
    void wait_idle();

    /// Whether an entity is an item it made and put in (not an authored one).
    bool made(EntityId id) const;
    /// An item it made is authored now (edited into an override, or baked): it is no longer the streamer's to
    /// take out or replace.
    void release(EntityId id);
    /// An authored item is the streamer's again (the edit that made it authored was undone), if the cell it
    /// belongs to is in and makes it.
    void adopt(EntityId id);
    /// What stood for an item is gone (a removed marker deleted): its cell puts its items in again.
    void restore(EntityId id);
    /// Overrides whose item is no longer made, in the cells now in, and new ones since the last call (once each).
    const std::set<EntityId>& orphans() const;
    std::vector<std::string> take_reports();
    const ScatterStats& stats() const noexcept { return m_stats; }
    void reset_peaks() noexcept {
        m_stats.longest_frame_ms = m_stats.longest_generate_ms = m_stats.longest_activation_ms = 0.0;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    ScatterStats m_stats;
};

} // namespace maya
