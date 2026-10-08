#pragma once
#include "maya/assets/asset.hpp"
#include "maya/jobs/jobs.hpp"
#include <chrono>
#include <deque>
#include <iosfwd>
#include <optional>
#include <unordered_map>
#include <variant>

namespace maya {
/// What a registry holds now. Bytes are tracked from buffer descriptors and CPU copies, not platform
/// residency.
struct AssetResidency {
    size_t entries = 0; // catalog entries
    size_t unloaded = 0, loading = 0, ready = 0, failed = 0; // entries by state
    size_t meshes = 0, materials = 0, scripts = 0, textures = 0, environments = 0, skins = 0, animations = 0; // resident versions owned by the cache
    size_t leased = 0; // resident versions also held by a lease outside the registry
    size_t mesh_gpu_bytes = 0; // vertex and index buffers of resident meshes
    size_t mesh_cpu_bytes = 0; // their picking geometry (MeshGeometry)
    size_t texture_gpu_bytes = 0; // every mip level of resident textures (texture_bytes)
    size_t environment_gpu_bytes = 0; // resident environments' backgrounds and specular cubes
};

/// How much finalizing (making device resources for prepared loads) one update may do
/// (docs/assets.md#asynchronous-loading). An update always finalizes at least one prepared load.
struct AssetLoadBudget {
    std::chrono::microseconds time{2000};
    size_t bytes = size_t{64} << 20;
};

/// The registry's loading, for instruments (docs/assets.md#asynchronous-loading).
struct AssetLoadStats {
    size_t in_flight = 0; // loads preparing on jobs or waiting to be finalized
    size_t prepared = 0; // of those, prepared and waiting for an update's budget
    uint64_t started = 0, merged = 0, finalized = 0, failed = 0, cancelled = 0, discarded = 0;
    uint64_t waited = 0; // loads finished by an explicit wait (acquire, reload, wait, wait_idle)
    uint64_t waited_in_frames = 0; // of those, inside a FrameScope with no ExplicitWait open: should be 0
    // The last update, and the longest since the registry began or reset_load_peaks():
    size_t last_finalized = 0, last_bytes = 0;
    double last_ms = 0.0, longest_ms = 0.0;
    size_t bytes_finalized = 0; // in total
    // From request to ready, over the last 256 loads finalized:
    double latency_p50_ms = 0.0, latency_p95_ms = 0.0, latency_max_ms = 0.0;
};

namespace detail {
struct AssetRequestState;
}

/// Interest in an asset's load (docs/assets.md#asynchronous-loading). Copies share it. When every request
/// for a load in flight is gone (or cancelled), the next update cancels the load. A material's request
/// includes its textures' requests, as one tree: loading until all are done, failed if any failed. Owner
/// thread only.
class AssetRequest {
public:
    AssetRequest() = default;
    explicit operator bool() const noexcept { return static_cast<bool>(m_state); }
    AssetId id() const noexcept;
    /// loading until it and everything it includes are done; then ready, or failed (also when cancelled).
    AssetState state() const noexcept;
    bool done() const noexcept { const auto s = state(); return s == AssetState::ready || s == AssetState::failed; }
    /// The first failure in the tree, or nothing.
    AssetDiagnostic diagnostic() const;
    /// Gives up this request's interest in the whole tree now.
    void cancel() const noexcept;

private:
    friend class AssetRegistry;
    explicit AssetRequest(std::shared_ptr<detail::AssetRequestState> state) noexcept : m_state(std::move(state)) {}
    std::shared_ptr<detail::AssetRequestState> m_state;
};

/// Single-owner-thread registry (docs/assets.md). Loads are split in two (AssetProvider::prepare): the
/// provider prepares on a background job, and update() finalizes on the owner thread within its budget.
/// It owns one cache lease per ready entry; evict_unused releases entries with no external lease.
class AssetRegistry {
public:
    AssetRegistry(std::filesystem::path project_root, std::unique_ptr<AssetProvider> provider);
    /// Cancels the loads in flight and waits for their jobs.
    ~AssetRegistry();
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;
    AssetRegistry(AssetRegistry&&) = delete;
    AssetRegistry& operator=(AssetRegistry&&) = delete;

    AssetDiagnostic register_asset(AssetRecord record);
    template<Asset T> AssetDiagnostic register_asset(AssetRef<T> ref, std::filesystem::path path) {
        return register_asset({ref.id, asset_kind<T>, std::move(path)});
    }
    std::vector<AssetRecord> records() const;
    std::optional<AssetInfo> info(AssetId id) const;
    /// Counts and bytes of what is resident; O(catalog size).
    AssetResidency residency() const noexcept;
    uint64_t token() const noexcept { return m_token; }
    /// The project directory that catalog paths are relative to.
    const std::filesystem::path& root() const noexcept { return m_root; }

    // Asynchronous loading -------------------------------------------------------------------------------
    /// Starts loading `ref` if it is not resident or loading (merging with a load in flight), and returns
    /// interest in it. Never blocks.
    template<Asset T> AssetRequest request(AssetRef<T> ref, JobTier tier = JobTier::background) {
        return request_entry(ref.id, asset_kind<T>, tier, false);
    }
    /// Loads `ref` again from its source; the current version stays in use until the new one is ready,
    /// and a failed reload keeps it. A load already in flight is superseded.
    template<Asset T> AssetRequest request_reload(AssetRef<T> ref, JobTier tier = JobTier::background) {
        return request_entry(ref.id, asset_kind<T>, tier, true);
    }
    /// The resident version, or, when there is none, starts a load the registry keeps alive and reports
    /// AssetError::loading (or the asset's failure). Never blocks: what frames use.
    template<Asset T> AssetResult<T> try_acquire(AssetRef<T> ref) {
        const auto slot = find(ref.id, asset_kind<T>);
        if (!slot.second) {
            if (usable(m_entries[slot.first].payload)) return lease<T>(slot.first, ref);
            if (auto diagnostic = start_held(slot.first)) return {{}, std::move(diagnostic)};
            return {{}, {AssetError::loading, "Asset " + m_entries[slot.first].record.path.string() + " is loading"}};
        }
        return {{}, slot.second};
    }
    /// Once a frame on the owner thread: applies finished preparations, cancels loads no one wants any
    /// more, and finalizes prepared loads in arrival order within `budget`.
    AssetLoadStats update(AssetLoadBudget budget = {});
    AssetLoadStats load_stats() const;
    void reset_load_peaks() noexcept { m_stats.longest_ms = m_stats.last_ms; }

    // Explicit waits: tests, tools, loading screens, and what must be resident at a fixed tick ----------
    /// The resident version, loading it first if needed: an explicit wait.
    template<Asset T> AssetResult<T> acquire(AssetRef<T> ref) { return load<T>(ref, false); }
    /// Loads again and waits. A failed reload preserves the previously published version and generation.
    template<Asset T> AssetResult<T> reload(AssetRef<T> ref) { return load<T>(ref, true); }
    /// Waits until the request and everything it includes are done; returns its state.
    AssetState wait(const AssetRequest& request);
    /// Waits for every load in flight.
    void wait_idle();
    /// Marks the owner thread's frame, from its update until its rendering ends: a load that waits
    /// meanwhile, with no ExplicitWait open, is counted in AssetLoadStats::waited_in_frames, the check
    /// that frames never load synchronously. (Flags, not a scope: an editor may replace its registry
    /// in the middle of a frame.)
    void begin_frame() noexcept { m_in_frame = true; }
    void end_frame() noexcept { m_in_frame = false; }
    /// Declares an intended wait inside a frame: an editor tool opening a material, a play session's
    /// scripts and clips (which must be resident at a fixed tick for replays to match).
    class ExplicitWait {
    public:
        explicit ExplicitWait(AssetRegistry& registry) noexcept : m_registry(registry) { ++m_registry.m_explicit; }
        ~ExplicitWait() { --m_registry.m_explicit; }
        ExplicitWait(const ExplicitWait&) = delete;
        ExplicitWait& operator=(const ExplicitWait&) = delete;
    private:
        AssetRegistry& m_registry;
    };

    template<Asset T> AssetResult<T> resolve(AssetHandle<T> handle) const {
        if (handle.registry != m_token || handle.slot >= m_entries.size())
            return {{}, {AssetError::stale_handle, "Asset handle belongs to another registry or invalid slot"}};
        const auto& entry = m_entries[handle.slot];
        if (entry.record.kind != asset_kind<T>)
            return {{}, {AssetError::wrong_type, "Asset handle type does not match the catalog entry"}};
        if (entry.generation != handle.generation || !usable(entry.payload))
            return {{}, {AssetError::stale_handle, "Asset version was evicted, replaced, or its device session ended"}};
        return {AssetLease<T>{std::get<std::shared_ptr<const T>>(entry.payload),handle,{entry.record.id}}, {}};
    }
    /// Releases resident versions no one else holds; entries with a load in flight are kept.
    size_t evict_unused();
    /// Publishes `value` as the material's next version, as a successful reload would, without reading
    /// its file: an editor's unsaved edit (docs/editor.md#materials). Later acquisitions and extractions
    /// see it; leases of the previous version keep it; a load of it in flight is cancelled. Fails for an
    /// ID that is not a cataloged material.
    AssetDiagnostic publish(AssetRef<MaterialAsset> ref, MaterialAsset value);

private:
    using Payload = AssetValue;
    struct PendingLoad {
        uint64_t generation = 0; // this load's request generation; completions of others are discarded
        bool reload = false;
        bool held = false; // the registry keeps it alive (try_acquire and explicit waits)
        JobTier tier = JobTier::background;
        AssetState previous = AssetState::unloaded; // restored if the load is cancelled
        JobHandle job;
        std::shared_ptr<PreparedAsset> prepared; // set when the job's completion is applied
        std::chrono::steady_clock::time_point start;
        std::vector<std::weak_ptr<detail::AssetRequestState>> requests;
    };
    struct Entry {
        AssetRecord record;
        AssetState state = AssetState::unloaded;
        uint64_t generation = 0;
        Payload payload{};
        AssetDiagnostic diagnostic{};
        std::optional<PendingLoad> pending;
    };
    template<Asset T> AssetResult<T> lease(uint32_t slot, AssetRef<T> ref) const {
        const auto& entry = m_entries[slot];
        return {AssetLease<T>{std::get<std::shared_ptr<const T>>(entry.payload), {m_token, slot, entry.generation}, ref}, {}};
    }
    template<Asset T> AssetResult<T> load(AssetRef<T> ref, bool reload) {
        const auto slot = find(ref.id, asset_kind<T>);
        if (slot.second) return {{}, slot.second};
        if (auto diagnostic = load_and_wait(slot.first, reload)) return {{}, std::move(diagnostic)};
        return lease<T>(slot.first, ref);
    }
    static bool usable(const Payload& payload) noexcept;
    /// The entry's slot, or why there is none (not registered, another kind, or a load during finalize).
    std::pair<uint32_t, AssetDiagnostic> find(AssetId id, AssetKind kind) const;
    AssetRequest request_entry(AssetId id, AssetKind kind, JobTier tier, bool reload);
    AssetDiagnostic start_held(uint32_t slot, JobTier tier = JobTier::background);
    /// Starts (or joins) the entry's load; fails at once when its file cannot be resolved.
    AssetDiagnostic start_load(uint32_t slot, bool reload, bool held, JobTier tier);
    AssetDiagnostic load_and_wait(uint32_t slot, bool reload);
    AssetDiagnostic wait_slot(uint32_t slot);
    void apply_completions();
    void finalize(uint32_t slot);
    void cancel_load(uint32_t slot);
    void attach(Entry& entry, const std::shared_ptr<detail::AssetRequestState>& request, JobTier tier);
    void settle(Entry& entry, PendingLoad& load);
    std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& path) const;
    const uint64_t m_token;
    const std::filesystem::path m_root;
    std::unique_ptr<AssetProvider> m_provider;
    bool m_loading = false; // finalizing: a provider may not load meanwhile
    std::vector<Entry> m_entries;
    std::unordered_map<AssetId,uint32_t,PersistentIdHash> m_ids;
    std::unordered_map<std::string,AssetId> m_paths;
    std::vector<uint32_t> m_in_flight; // slots with a load in flight
    std::deque<uint32_t> m_prepared; // slots whose loads are prepared, in arrival order
    uint64_t m_next_generation = 0;
    AssetLoadStats m_stats;
    std::vector<double> m_latencies; // the last 256, as a ring
    size_t m_latency_next = 0;
    bool m_in_frame = false;
    int m_explicit = 0;
    CompletionQueue m_completions;
    JobScope m_jobs; // last: destroyed first, so no job outlives what it uses
};

struct AssetCatalogResult {
    std::vector<AssetRecord> records;
    AssetDiagnostic diagnostic;
    explicit operator bool() const noexcept { return !diagnostic; }
};
/// Versioned catalog of IDs, types, and quoted project-relative source paths; no runtime state.
AssetCatalogResult read_asset_catalog(std::istream& input);
void write_asset_catalog(std::ostream& output, const std::vector<AssetRecord>& records);
} // namespace maya
