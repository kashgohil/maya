#pragma once

#include "build_info.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/system_info.hpp"
#include "maya/metrics/metrics.hpp"
#include "maya/rhi/graphics_device.hpp"
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace maya::benchmark {

enum class Workload {
    instances, // I1: one mesh and material shared by `count` generated entities, a fraction spinning
    scene, // a project's saved scene, as the player runs it (the fast regression case)
    load_cycles, // L1: load the generated scene from its file, play and render, unload; repeatedly
    play_cycles, // L1: start and stop play sessions from one authored scene; repeatedly
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
    bool overhead = true; // also a matched run without CPU instrumentation, to measure its cost
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
    std::vector<std::pair<std::string, std::string>> unavailable; // metric, reason
};

/// Runs the manifest offscreen on `device` (initialized, headless or not) with the renderer shader.
/// Failures stop the benchmark and are recorded in `failure`; what completed is kept.
Result run(const Manifest& manifest, GraphicsDevice& device, std::string renderer_shader);

std::string to_json(const Result& result);
/// A short human-readable report.
std::string to_text(const Result& result);

} // namespace maya::benchmark
