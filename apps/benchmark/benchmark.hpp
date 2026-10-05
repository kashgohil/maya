#pragma once

#include "maya/core/build_info.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/system_info.hpp"
#include "maya/metrics/metrics.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/scene/scene_io.hpp"
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace maya::benchmark {

enum class Workload {
    instances, // I1: one mesh and material shared by `count` generated entities, a fraction spinning
    scene, // a project's saved scene, as the player runs it (the fast regression case)
    load_cycles, // L1: load the generated scene from its file, play and render, unload; repeatedly
    play_cycles, // L1: start and stop play sessions from one authored scene; repeatedly
    physics, // P1: the physics stress scene, ticked back to back with no views
    import, // glTF files imported into a new project, then loaded cold (cooking) and warm (cook cache)
};

/// A versioned benchmark description (docs/performance.md#manifests). Paths are relative to the
/// manifest's folder.
struct Manifest {
    std::string name;
    Workload workload = Workload::instances;
    std::filesystem::path project; // resolved
    std::filesystem::path scene; // content-relative; scene workload (default: the startup scene)
    AssetId mesh{}; // generated workloads: the shared mesh and material, by catalog ID
    AssetId material{};
    uint32_t count = 1000;
    double rotating = 0.1; // fraction of generated entities with maya.spin, chosen by seed
    float spacing = 1.5f; // metres between grid cells
    uint64_t seed = 990;
    math::Vec3 camera_position{0.0f, 30.0f, 60.0f}; // generated workloads look from here...
    math::Vec3 camera_target{0.0f, 0.0f, 0.0f}; // ...at this point
    uint32_t width = 1920, height = 1080;
    uint32_t upload_mib = 64; // per-frame upload memory; instances need 256 bytes each
    uint32_t warmup = 300; // frames before sampling
    uint32_t samples = 3000; // sampled frames per run
    uint32_t runs = 3;
    uint32_t cycles = 100; // cycle workloads
    uint32_t ticks = 120; // frames per cycle
    uint32_t slope_from = 11; // cycle workloads: the first cycle of the footprint slope's fit
    bool overhead = true; // also a matched run without CPU scopes or GPU pass timing, to measure their cost
    bool present = false; // present every frame to a window and record display pacing (#1026)
    // Physics (P1): `count` is the dynamic bodies; warmup and samples are ticks.
    uint32_t obstacles = 500;
    uint32_t scripted = 500;
    uint32_t sensors = 50;
    uint32_t rays = 1000, overlaps = 100, casts = 20; // queries per tick
    std::vector<int> workers{-1, 0}; // physics worker threads per configuration; -1 is the default
    // Import: glTF files (with the files they name) under `content`, e.g. the R1 content.
    std::filesystem::path content; // resolved
    std::vector<std::filesystem::path> models; // relative to `content`
};
struct ManifestResult {
    Manifest manifest;
    std::string error; // with its line; empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};
ManifestResult read_manifest(std::istream& input, const std::filesystem::path& folder);
ManifestResult load_manifest(const std::filesystem::path& file);

/// One run's per-frame samples, in milliseconds. Without instrumentation only `frame` is kept.
struct RunSamples {
    bool instrumented = true;
    std::vector<double> frame, simulation, wait, extract, encode, submit;
    std::vector<std::optional<double>> gpu; // per sampled frame; nullopt when the GPU did not report it
    /// Per pass label, per sampled frame: the GPU time of that frame's passes with the label, summed;
    /// nullopt when the frame had none or was not timed.
    std::map<std::string, std::vector<std::optional<double>>> gpu_passes;
    uint64_t gpu_pass_mismatches = 0; // timed passes outside their frame's GPU time (never expected)
    uint64_t untimed_passes = 0; // beyond GraphicsDevice::max_timed_passes
    /// Presenting runs: when each sampled frame was shown (host seconds), nullopt when it was not, and
    /// whether the device reported on it at all.
    std::vector<std::optional<double>> presented;
    std::vector<bool> present_reported;
    double sampled_seconds = 0.0;
};

/// What was resident at one moment, tracked and platform-reported kept apart.
struct MemorySample {
    RhiStats device{};
    std::optional<size_t> gpu_reported; // GraphicsDevice::reported_memory
    std::optional<ProcessMemory> process;
    AssetResidency assets{};
    size_t entities = 0; // live entities in the benchmark's World(s)
};

/// One load/play cycle.
struct CycleSample {
    double load = 0.0; // read, validate, and build the World (load_cycles), or start the session
    double first_frame = 0.0; // includes loading the meshes the frame draws
    double longest_frame = 0.0;
    double stop = 0.0; // stop the session and release its World
    MemorySample after; // once the GPU finished and the registry evicted unused versions
};

struct RejectedCase {
    std::string name;
    bool rejected = false;
    std::string reason;
    bool nothing_left = false; // no entities, leases, or resources remained
};

struct Counters {
    size_t entities = 0, mesh_renderers = 0, spinning = 0;
    size_t centers_in_view = 0; // instances whose origin projects inside the view
    uint64_t draws = 0, instances = 0, triangles = 0; // per frame, as submitted
    uint32_t passes = 0;
    size_t unique_meshes = 0, unique_materials = 0; // resident asset versions
};

/// P1: one run's per-tick samples (milliseconds and counts) and what it ended with.
struct PhysicsRun {
    int workers = -1; // as configured
    int worker_threads = 0; // what that resolved to
    std::string failure; // a step error flag, or an exception; the run is not averaged in
    double start_ms = 0.0; // build the World, create the bodies, start the scripts
    std::vector<double> tick, scripts, queries, other_systems, prepare, step, synchronize, events, late, commit;
    std::vector<double> active, sleeping, pairs, contacts, events_delivered, query_hits;
    size_t jolt_live_start = 0, jolt_live_end = 0, jolt_peak = 0; // Jolt heap, bytes
    size_t temp_high_water = 0, temp_capacity = 0; // per-step scratch allocator
    size_t script_bytes = 0; // script VM, at the end
    std::optional<size_t> footprint_start, footprint_end;
    double footprint_slope_per_tick = 0.0; // bytes, from samples every 100 ticks
    uint64_t state = 0; // PlaySession::state_hash after the last tick: every body's pose and more
};
/// P1: what the generated scene holds.
struct PhysicsScene {
    size_t bodies = 0, static_bodies = 0, kinematic_bodies = 0, dynamic_bodies = 0, active_set = 0, sleeping_set = 0;
    size_t boxes = 0, spheres = 0, capsules = 0, obstacles = 0, sensors = 0, scripted = 0;
};

/// Import: one model in one run, in milliseconds.
struct ImportSample {
    std::string model; // its file name
    double import_ms = 0.0; // import_gltf: parse, convert, and write the material, scene, import, and catalog files
    double cold_ms = 0.0; // load every part through an empty cook cache: decode, cook, upload, and write the cache
    double warm_ms = 0.0; // load every part again in a new session, from the cook cache
    size_t meshes = 0, textures = 0, materials = 0, entities = 0;
    size_t triangles = 0;
    size_t texture_gpu_bytes = 0, cache_bytes = 0;
    size_t warm_hits = 0, warm_misses = 0;
    std::string cache_digest; // of every cache entry, in path order: equal across runs when cooking is deterministic
};

struct Result {
    Manifest manifest;
    SystemInfo system;
    BuildInfo build;
    std::string failure; // empty when every step completed
    std::vector<RunSamples> runs;
    std::optional<RunSamples> uninstrumented;
    Counters counters;
    MemorySample baseline; // the empty session: device, catalog, and view target, before any content
    MemorySample resident; // with the workload's content resident, after the runs
    std::vector<CycleSample> cycles;
    std::vector<RejectedCase> rejected;
    std::optional<bool> authored_unchanged; // play cycles
    PhysicsScene physics_scene;
    std::vector<PhysicsRun> physics_runs;
    std::optional<bool> deterministic; // physics: every run and worker configuration ended in the same state; import: every
                                       // run cooked the same bytes
    std::vector<ImportSample> imports; // import: per run, per model
    std::vector<std::pair<std::string, std::string>> unavailable; // metric, reason
    std::optional<double> refresh_hz; // presenting runs: the display's refresh rate
    std::string thermal_state_at_end; // system_info().thermal_state when the benchmark ended
    std::string thread_qos; // the measuring thread's quality-of-service class
};

/// P1's scene, version 1, as scene data (docs/architecture/performance-baseline.md#p1-physics-stress),
/// and what it holds. Its script is physics_script_source(), under physics_script_id.
SceneDocument physics_scene(const Manifest& manifest, PhysicsScene* counts = nullptr);
inline constexpr AssetId physics_script_id{0x7031, 1};
const char* physics_script_source();

/// Runs the manifest on `device` (initialized) with the renderer shader: offscreen, or, when the
/// manifest presents, also into the device's surface every frame, calling `poll` (e.g. the window's
/// event loop) between frames, outside their timing. Failures stop the benchmark and are recorded in
/// `failure`; what completed is kept.
Result run(const Manifest& manifest, GraphicsDevice& device, std::string renderer_shader, const std::function<void()>& poll = {});
/// Present-to-present intervals of consecutive shown frames, and those longer than 1.5 refresh periods.
struct Pacing {
    std::vector<double> intervals; // milliseconds
    size_t shown = 0, not_shown = 0, unreported = 0, missed = 0;
};
Pacing pacing(const RunSamples& run, std::optional<double> refresh_hz);

std::string to_json(const Result& result);
/// A short human-readable report.
std::string to_text(const Result& result);

} // namespace maya::benchmark
