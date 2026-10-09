#pragma once

#include "maya/jobs/jobs.hpp"
#include "maya/scene/world_io.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace maya {
struct ProjectSettings;
class CookCache;
class PhysicsWorld;
class World;

// Streaming a world's cells (#1064, docs/world.md#streaming): cells near the sources load on background
// jobs, activate into the World within a per-frame budget, and deactivate and unload when the sources move
// away, keeping what play changed. The persistent part is loaded once, as a scene.

/// Where a cell is in its life. Failed cells are retried once they leave the load radius and come back.
enum class CellState : uint8_t { unloaded, loading, ready, activating, active, deactivating, failed };
inline constexpr size_t cell_state_count = 7;
const char* cell_state_name(CellState state) noexcept;

struct StreamingSettings {
    double load_radius = 640.0; // metres from a source to a cell's square: start loading
    double activate_radius = 384.0; // ... and put it into the World
    double hysteresis = 64.0; // beyond a radius by this much before the cell goes back down
    /// Owner-thread time a frame spends committing cells into the World and taking them out (#1060: 1 ms).
    std::chrono::microseconds frame_budget{1000};
    size_t frame_entities = 4096; // most entities committed or removed a frame
    size_t bytes_in_flight = size_t{64} << 20; // most cooked cell bytes loading or waiting to activate
    JobTier tier = JobTier::background;
};

/// The project's streaming radii (ProjectSettings::stream_*) over the defaults.
StreamingSettings project_streaming_settings(const ProjectSettings& project);

/// What loading a cell produces, off the owner thread.
struct CellLoad {
    std::optional<SceneDocument> document;
    std::string error; // why not, naming the cell's file
    size_t bytes = 0; // the cooked cell's size: what it holds while loaded
};
/// Loads a cell's content on a job. It must be thread-safe and check `job.cancelled()` between steps.
using CellLoader = std::function<CellLoad(const WorldCell& cell, const JobContext& job)>;
/// Reads cells' scene text beside the world file and cooks it to packed binary through `cache` (when
/// given): a later load decodes the cooked cell instead of parsing (#1060: 14× faster). A cell already
/// cooked (`.cell`, as packages hold them) is decoded directly. `context` validates
/// asset references; it is called from jobs, so it must only read the catalog.
CellLoader cooked_cell_loader(std::filesystem::path world_folder, std::shared_ptr<CookCache> cache, PropertyValidationContext context);

/// Play's changes to one cell, kept while it is unloaded (docs/world.md#state-across-unloads).
struct CellDelta {
    std::unordered_set<EntityId, PersistentIdHash> destroyed;
    /// The entity's components that keep state (ComponentDescriptor::keeps_state), as they were.
    std::unordered_map<EntityId, std::vector<ComponentValue>, PersistentIdHash> changed;
    bool empty() const noexcept { return destroyed.empty() && changed.empty(); }
};

struct StreamingStats {
    std::array<size_t, cell_state_count> cells{}; // by CellState
    uint64_t loads_started = 0, loads_finished = 0, loads_failed = 0, loads_cancelled = 0;
    uint64_t completions_discarded = 0; // loads that finished after being cancelled
    uint64_t activations = 0, deactivations = 0, pinned = 0; // pinned: kept active (a hierarchy left the cell)
    double frame_ms = 0.0; // the last update's owner-thread work
    double longest_frame_ms = 0.0; // since reset_peaks
    size_t bytes_in_flight = 0; // loading
    size_t bytes_loaded = 0; // ready, activating, or active
    size_t active_entities = 0;
    size_t deltas = 0; // cells holding play's changes while unloaded
    std::string last_error;
};

class WorldStreamer {
public:
    WorldStreamer(WorldDocument world, CellLoader loader, StreamingSettings settings = {});
    /// Cancels its loads and waits for their jobs. The World it fed is not touched.
    ~WorldStreamer();
    WorldStreamer(const WorldStreamer&) = delete;
    WorldStreamer& operator=(const WorldStreamer&) = delete;

    /// Where the world is seen from: the camera, the player. Cells near any of them stream in.
    void set_sources(std::vector<math::DVec3> positions);
    /// One frame of streaming into `world` (and `physics`, when the world has one), on the owner thread
    /// between ticks: applies finished loads, decides what each cell should be, and commits within the
    /// frame budget.
    const StreamingStats& update(World& world, PhysicsWorld* physics);
    /// Brings every cell to what the sources want now, waiting for loads and ignoring the frame budget:
    /// a loading screen.
    const StreamingStats& settle(World& world, PhysicsWorld* physics);
    /// Takes every cell out of `world` now, keeping play's changes, and stops: the world is closing.
    void unload_all(World& world, PhysicsWorld* physics);

    CellState state(CellIndex cell) const;
    const CellDelta* delta(CellIndex cell) const;
    const StreamingStats& stats() const noexcept { return m_stats; }
    void reset_peaks() noexcept { m_stats.longest_frame_ms = 0.0; }
    const WorldDocument& world() const noexcept { return m_world; }
    const StreamingSettings& settings() const noexcept { return m_settings; }

private:
    struct Cell;
    struct Frame;
    struct Want {
        bool loaded = false, active = false;
    };
    Want want(const Cell& cell) const;
    void start_load(Cell& cell);
    void cancel_load(Cell& cell);
    bool activate_step(Cell& cell, World& world, PhysicsWorld* physics, Frame& frame);
    void begin_deactivate(Cell& cell, World& world, PhysicsWorld* physics);
    bool deactivate_step(Cell& cell, World& world, Frame& frame);
    void count();
    void release(std::shared_ptr<const void>&& held);

    WorldDocument m_world;
    CellLoader m_loader;
    StreamingSettings m_settings;
    std::vector<math::DVec3> m_sources;
    std::map<CellIndex, std::unique_ptr<Cell>> m_cells;
    std::unordered_map<StageGroup, CellIndex> m_published; // active cells, by the group they were published from
    StageGroup m_next_group = 1;
    /// What owner-thread steps cost, measured as they run (microseconds), so a step starts only if it fits.
    struct Costs {
        double commit_us = 4.0; // an entity committed, staged
        double discard_us = 0.5; // an entity removed
        double finish_us = 500.0; // publishing a cell, with its bodies
        double leave_us = 500.0; // taking a cell out of sight and out of physics
    } m_costs;
    StreamingStats m_stats;
    CompletionQueue m_completions;
    JobScope m_jobs; // last: its jobs end before anything they use
};

} // namespace maya
