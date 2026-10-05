#include "benchmark.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/gltf.hpp"
#include "support/timing_device.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace maya;
using namespace maya::benchmark;
namespace fs = std::filesystem;

namespace {
ManifestResult parse(const std::string& text) {
    auto input = std::istringstream(text);
    return read_manifest(input, "/benchmarks");
}
const std::string minimal = "maya-benchmark 1\nname \"t\"\nworkload scene\nproject \"../game\"\n";

Manifest small(Workload workload) {
    auto manifest = Manifest{};
    manifest.name = "test";
    manifest.workload = workload;
    const auto project = FileSystem::resolve("samples/basic_scene/project.maya");
    REQUIRE(project);
    manifest.project = *project;
    manifest.mesh = {0x6d617961, 2};
    manifest.material = {0x6d617961, 0x11};
    manifest.count = 50;
    manifest.width = 64;
    manifest.height = 36;
    manifest.warmup = 2;
    manifest.samples = 6;
    manifest.runs = 2;
    manifest.cycles = 4;
    manifest.ticks = 3;
    manifest.camera_position = {0.0f, 20.0f, 25.0f};
    return manifest;
}
struct Device : NullGraphicsDevice {
    Device() { REQUIRE(initialize(nullptr, {3, size_t{8} << 20})); }
    ~Device() override { shutdown(); }
};
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
} // namespace

TEST_CASE("Benchmark manifests parse every setting and name the line of a mistake", "[benchmark]") {
    const auto full = parse(R"(maya-benchmark 1
# a comment, and a blank line

name "i1-10k"
workload instances
project "../samples/basic_scene"
mesh 6d617961 2
material 6d617961 11
count 10000
rotating 0.25
spacing 2
seed 7
camera 0 10 20 1 2 3
resolution 1280 720
upload_mib 32
warmup 10
samples 100
runs 2
cycles 5
ticks 6
overhead off
present on
)");
    REQUIRE(full);
    const auto& m = full.manifest;
    CHECK(m.name == "i1-10k");
    CHECK(m.workload == Workload::instances);
    CHECK(m.project == fs::path("/samples/basic_scene")); // relative to the manifest's folder
    CHECK(m.mesh == AssetId{0x6d617961, 2});
    CHECK(m.material == AssetId{0x6d617961, 0x11});
    CHECK(m.count == 10000);
    CHECK(m.rotating == 0.25);
    CHECK(m.spacing == 2.0f);
    CHECK(m.seed == 7);
    CHECK(m.camera_position.z == 20.0f);
    CHECK(m.camera_target.x == 1.0f);
    CHECK(m.width == 1280);
    CHECK(m.upload_mib == 32);
    CHECK(m.warmup == 10);
    CHECK(m.samples == 100);
    CHECK(m.runs == 2);
    CHECK(m.cycles == 5);
    CHECK(m.ticks == 6);
    CHECK_FALSE(m.overhead);
    CHECK(m.present);
    // Defaults follow the measurement protocol: 300 warmup frames, 3,000 samples, three runs.
    const auto defaults = parse(minimal);
    REQUIRE(defaults);
    CHECK(defaults.manifest.warmup == 300);
    CHECK(defaults.manifest.samples == 3000);
    CHECK(defaults.manifest.runs == 3);
    CHECK(defaults.manifest.seed == 990);
    CHECK_FALSE(defaults.manifest.present); // offscreen unless asked

    const auto rejected = [](const std::string& text, const std::string& reason) {
        const auto result = parse(text);
        INFO(text);
        CHECK_FALSE(result);
        CHECK(has(result.error, reason));
    };
    rejected("name \"t\"\n", "line 1: expected a maya-benchmark header");
    rejected("maya-benchmark 2\n", "unsupported benchmark version 2");
    rejected(minimal + "count 5\ncount 6\n", "line 6: 'count' appears twice");
    rejected(minimal + "frames 5\n", "line 5: unknown key 'frames'");
    rejected(minimal + "count 0\n", "invalid value for 'count'");
    rejected(minimal + "rotating 1.5\n", "invalid value for 'rotating'");
    rejected(minimal + "camera 1 1 1 1 1 1\n", "invalid value for 'camera'"); // looking at itself
    rejected(minimal + "samples 10 20\n", "unexpected '20' after 'samples'");
    rejected(minimal + "present maybe\n", "invalid value for 'present'");
    rejected(minimal + "workload fast\n", "'workload' appears twice");
    rejected("maya-benchmark 1\nname \"t\"\nworkload fast\n", "unknown workload 'fast'");
    rejected("maya-benchmark 1\nname \"t\"\nworkload scene\n", "missing 'project'");
    rejected("maya-benchmark 1\nname \"t\"\nworkload instances\nproject \"p\"\nmesh 1 2\n", "needs 'material'");
    rejected("maya-benchmark 1\nname \"t\"\nworkload instances\nproject \"p\"\nmesh 1 zz\n", "invalid value for 'mesh'");
    const auto missing = load_manifest("/no/such.benchmark");
    CHECK(has(missing.error, "cannot read /no/such.benchmark"));
}

TEST_CASE("Repeated instances share one mesh and material, and every instance is counted", "[benchmark]") {
    auto device = Device{};
    auto manifest = small(Workload::instances);
    const auto result = run(manifest, device, "renderer");
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    REQUIRE(result.runs.size() == 2);
    const auto& counters = result.counters;
    CHECK(counters.entities == 52); // the instances, a camera, and a light
    CHECK(counters.mesh_renderers == 50);
    CHECK(counters.spinning == 5); // exactly 10%, chosen by the seed
    // One draw per instance, and the tone-mapping triangle in its own pass.
    CHECK(counters.draws == 50 + 1);
    CHECK(counters.instances == 50 + 1);
    CHECK(counters.triangles == 50 * 12 + 1); // the cube has 12 triangles
    CHECK(counters.passes == 2);
    // Sharing: the unique allocations do not grow with the instance count.
    CHECK(counters.unique_meshes == 1);
    CHECK(counters.unique_materials == 1);
    CHECK(result.resident.device.buffers == 2); // one vertex and one index buffer
    CHECK(result.resident.assets.mesh_gpu_bytes == result.resident.device.buffer_bytes);
    CHECK(counters.centers_in_view > 0);
    CHECK(counters.centers_in_view <= 50);
    for (const auto& run : result.runs) {
        CHECK(run.instrumented);
        CHECK(run.frame.size() == 6);
        CHECK(run.simulation.size() == 6);
        CHECK(run.encode.size() == 6);
        CHECK(run.gpu.size() == 6);
        for (const auto& gpu : run.gpu) CHECK_FALSE(gpu); // the null device has no GPU clock
        CHECK(run.sampled_seconds > 0.0);
    }
    REQUIRE(result.uninstrumented);
    CHECK_FALSE(result.uninstrumented->instrumented);
    CHECK(result.uninstrumented->frame.size() == 6);
    CHECK(result.uninstrumented->simulation.empty());
    CHECK(std::ranges::any_of(result.unavailable, [](const auto& entry) { return entry.first == "gpu_frame_time"; }));
    // The conditions at the end are recorded for every workload, next to those at the start.
    CHECK_FALSE(result.system.thermal_state.empty());
    CHECK_FALSE(result.thermal_state_at_end.empty());
    CHECK_FALSE(result.thread_qos.empty());

    // The same seed selects the same entities; another seed selects others, still exactly 10%.
    manifest.count = 1000;
    manifest.runs = 1;
    manifest.overhead = false;
    manifest.samples = 1;
    manifest.warmup = 0;
    const auto again = run(manifest, device, "renderer");
    CHECK(again.counters.spinning == 100);
    manifest.seed = 1;
    CHECK(run(manifest, device, "renderer").counters.spinning == 100);

    // More instances than the upload memory can hold fail with the reason, not silently.
    manifest.count = 40000; // 256 bytes each is more than 8 MiB
    const auto overflow = run(manifest, device, "renderer");
    CHECK(has(overflow.failure, "upload"));
    CHECK(has(to_json(overflow), "\"succeeded\":false"));
}

TEST_CASE("Load cycles return to their baseline and refuse bad scenes", "[benchmark]") {
    auto device = Device{};
    const auto loads = run(small(Workload::load_cycles), device, "renderer");
    INFO(loads.failure);
    REQUIRE(loads.failure.empty());
    REQUIRE(loads.cycles.size() == 4);
    for (const auto& cycle : loads.cycles) {
        CHECK(cycle.load > 0.0);
        CHECK(cycle.first_frame > 0.0);
        CHECK(cycle.longest_frame >= cycle.first_frame);
        CHECK(cycle.after.device.buffers == loads.baseline.device.buffers);
        CHECK(cycle.after.device.textures == loads.baseline.device.textures); // the view target stays
        CHECK(cycle.after.device.pending_retirements == 0);
        CHECK(cycle.after.assets.meshes == 0); // evicted
        CHECK(cycle.after.assets.leased == 0);
    }
    REQUIRE(loads.rejected.size() == 2);
    for (const auto& rejected : loads.rejected) {
        INFO(rejected.name);
        CHECK(rejected.rejected);
        CHECK_FALSE(rejected.reason.empty());
        CHECK(rejected.nothing_left);
    }
    CHECK(has(loads.rejected[1].reason, "ffffffff")); // the missing asset is named
    CHECK(loads.authored_unchanged == true);

    auto play_manifest = small(Workload::play_cycles);
    play_manifest.cycles = 6;
    play_manifest.slope_from = 5; // the footprint is fitted over cycles 5-6, after the timings' warmup (3)
    const auto plays = run(play_manifest, device, "renderer");
    INFO(plays.failure);
    REQUIRE(plays.failure.empty());
    CHECK(plays.cycles.size() == 6);
    CHECK(has(to_json(plays), "\"footprint_slope_cycles\":[5,6]"));
    CHECK(plays.rejected.empty());
    CHECK(plays.authored_unchanged == true);
    CHECK(plays.cycles.back().after.device.buffers == plays.baseline.device.buffers);
}

TEST_CASE("Results are complete JSON with raw samples, summaries, and what was unavailable", "[benchmark]") {
    auto device = Device{};
    auto manifest = small(Workload::scene);
    const auto result = run(manifest, device, "renderer");
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    CHECK(result.counters.mesh_renderers == 4); // the sample scene
    const auto json = to_json(result);
    for (const auto* key : {"\"format\":\"maya-benchmark-result\"", "\"succeeded\":true", "\"environment\":{", "\"model\":",
                            "\"build\":{", "\"revision\":", "\"quality\":{", "\"width\":64", "\"counters\":{",
                            "\"baseline_memory\":{", "\"resident_memory\":{", "\"tracked\":{", "\"reported\":{",
                            "\"gpu_allocated_bytes\":null", "\"runs\":[{", "\"frame_ms\":{\"count\":6", "\"p95\":",
                            "\"samples\":{\"frame_ms\":[", "\"gpu_ms\":[null,null", "\"gpu_samples_missing\":6",
                            "\"overhead\":{", "\"unavailable\":{", "\"gpu_frame_time\":",
                            "\"resident_textures\":0", "\"texture_gpu_bytes\":0"}) { // nothing in the scene uses a texture
        INFO(key);
        CHECK(has(json, key));
    }
    // Balanced brackets and no stray commas.
    auto depth = 0;
    for (const auto c : json) {
        depth += (c == '{' || c == '[') - (c == '}' || c == ']');
        CHECK(depth >= 0);
    }
    CHECK(depth == 0);
    CHECK_FALSE(has(json, ",}"));
    CHECK_FALSE(has(json, ",]"));
    CHECK_FALSE(has(json, "[,"));
    CHECK(has(to_text(result), "run 2: frame"));
}

TEST_CASE("Cycle footprints are fitted from slope_from, and physics manifests name their counts", "[benchmark]") {
    const auto cycles = parse(minimal + "cycles 300\nslope_from 101\n");
    REQUIRE(cycles);
    CHECK(cycles.manifest.slope_from == 101);
    CHECK(parse(minimal).manifest.slope_from == 11);
    const auto late = parse("maya-benchmark 1\nname \"t\"\nworkload play_cycles\nproject \"p\"\nmesh 1 2\nmaterial 1 3\ncycles 50\nslope_from 51\n");
    CHECK(has(late.error, "past the last cycle"));

    const auto physics = parse("maya-benchmark 1\nname \"p1\"\nworkload physics\ncount 5000\nobstacles 500\nscripted 400\nsensors 50\n"
                               "queries 1000 100 20\nworkers default 0 3\n");
    INFO(physics.error);
    REQUIRE(physics); // no project, mesh, or material: it uses no assets
    const auto& m = physics.manifest;
    CHECK(m.workload == Workload::physics);
    CHECK(m.count == 5000);
    CHECK(m.obstacles == 500);
    CHECK(m.scripted == 400);
    CHECK(m.sensors == 50);
    CHECK(m.rays == 1000);
    CHECK(m.overlaps == 100);
    CHECK(m.casts == 20);
    CHECK(m.workers == std::vector<int>{-1, 0, 3});
    CHECK(has(parse("maya-benchmark 1\nname \"p\"\nworkload physics\nworkers some\n").error, "workers are 'default' or a count"));
    CHECK(has(parse("maya-benchmark 1\nname \"p\"\nworkload physics\nqueries 1 2\n").error, "invalid value for 'queries'"));

    // The recipe's proportions: 40% dropped, 60% resting; 60/25/15 boxes, spheres, and capsules.
    auto counts = PhysicsScene{};
    const auto scene = physics_scene(m, &counts);
    CHECK(counts.dynamic_bodies == 5000);
    CHECK(counts.active_set == 2000);
    CHECK(counts.sleeping_set == 3000);
    CHECK(counts.boxes == 3000);
    CHECK(counts.spheres == 1250);
    CHECK(counts.capsules == 750);
    CHECK(counts.obstacles == 500);
    CHECK(counts.sensors == 50);
    CHECK(counts.scripted == 400);
    CHECK(counts.kinematic_bodies == 1);
    CHECK(counts.static_bodies == 5 + 500 + 50);
    CHECK(counts.bodies == scene.entities.size());
    // The same seed gives the same scene.
    auto again = physics_scene(m);
    REQUIRE(again.entities.size() == scene.entities.size());
    CHECK(std::get<TransformComponent>(again.entities.back().components[1]).translation.x ==
          std::get<TransformComponent>(scene.entities.back().components[1]).translation.x);
}

TEST_CASE("The physics workload runs headless, counts every part, and ends every run in the same state", "[benchmark]") {
    auto manifest = Manifest{};
    manifest.name = "p1-test";
    manifest.workload = Workload::physics;
    manifest.count = 100;
    manifest.obstacles = 10;
    manifest.scripted = 10;
    manifest.sensors = 2;
    manifest.rays = 20;
    manifest.overlaps = 4;
    manifest.casts = 2;
    manifest.warmup = 5;
    manifest.samples = 20;
    manifest.runs = 2;
    manifest.workers = {-1, 0};
    auto device = Device{};
    const auto result = run(manifest, device, "renderer");
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    REQUIRE(result.physics_runs.size() == 4);
    CHECK(result.deterministic == true);
    CHECK(result.physics_runs[2].worker_threads == 0);
    for (const auto& run : result.physics_runs) {
        CHECK(run.failure.empty());
        CHECK(run.tick.size() == 20);
        CHECK(run.step.size() == 20);
        CHECK(run.scripts.size() == 20);
        CHECK(run.queries.front() > 0.0);
        CHECK(run.query_hits.front() > 0.0); // rays toward the bin hit it
        CHECK(run.active.back() > 0.0); // the paddle keeps the bin awake
        CHECK(run.contacts.back() > 0.0);
        CHECK(run.temp_high_water > 0);
        CHECK(run.jolt_peak > 0);
        CHECK(run.script_bytes > 0);
        CHECK(run.state == result.physics_runs.front().state);
    }
    const auto json = to_json(result);
    for (const auto* key : {"\"physics\":{", "\"recipe\":{\"version\":1", "\"deterministic\":true", "\"worker_threads\":0", "\"step\":{",
                            "\"query_hits\":{", "\"temp_high_water_bytes\":", "\"state_hash\":\"", "\"contact_constraints\":"}) {
        INFO(key);
        CHECK(has(json, key));
    }
    CHECK(has(to_text(result), "every run ended in the same state"));
    CHECK(device.stats().submitted_frames == 0); // headless: nothing rendered
}

TEST_CASE("The import workload imports each model, loads it cold and warm, and cooks the same bytes every run", "[benchmark]") {
    // The R1 manifest names its content and models.
    const auto r1 = load_manifest(fs::path(MAYA_SOURCE_DIR) / "benchmarks/r1_import.benchmark");
    INFO(r1.error);
    REQUIRE(r1);
    CHECK(r1.manifest.workload == Workload::import);
    CHECK(r1.manifest.models.size() == 3);
    CHECK(r1.manifest.content == (fs::path(MAYA_SOURCE_DIR) / "build/render-samples").lexically_normal());
    CHECK(parse("maya-benchmark 1\nname \"t\"\nworkload import\nmodels \"a.glb\"\n").error == "the import workload needs 'content'");

    const auto content = fs::temp_directory_path() / ("maya-benchmark-content-" + std::to_string(::getpid()));
    fs::create_directories(content / "textures");
    std::ofstream(content / "props.gltf", std::ios::binary) << test::props_gltf();
    std::ofstream(content / "textures/normal.png", std::ios::binary) << test::flat_normal_png();
    auto manifest = Manifest{};
    manifest.name = "import-test";
    manifest.workload = Workload::import;
    manifest.content = content;
    manifest.models = {"props.gltf"};
    manifest.runs = 2;
    auto device = Device{};
    const auto result = run(manifest, device, "renderer");
    fs::remove_all(content);
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    REQUIRE(result.imports.size() == 2);
    CHECK(result.deterministic == true);
    for (const auto& sample : result.imports) {
        CHECK(sample.model == "props.gltf");
        CHECK(sample.meshes == 3);
        CHECK(sample.textures == 2);
        CHECK(sample.materials == 3);
        CHECK(sample.triangles == 6);
        CHECK(sample.warm_hits == 5); // every mesh and texture; materials are not cooked
        CHECK(sample.warm_misses == 0);
        CHECK(sample.cold_ms > 0.0);
        CHECK(sample.cache_bytes > 0);
    }
    const auto json = to_json(result);
    for (const auto* key : {"\"imports\":[", "\"cold_load_ms\":", "\"warm_load_ms\":", "\"cache_digest\":\"", "\"deterministic\":true"}) {
        INFO(key);
        CHECK(has(json, key));
    }
    CHECK(has(to_text(result), "props.gltf: import "));
    CHECK(has(to_text(result), "every run cooked the same bytes"));
}

TEST_CASE("Per-pass GPU times are summed by label, checked against their frame, and left out of the matched run", "[benchmark]") {
    const auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
    test::TimingDevice device;
    REQUIRE(device.initialize(nullptr, {3, size_t{8} << 20}));
    const auto manifest = small(Workload::instances);
    const auto result = run(manifest, device, shader);
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    for (const auto& run : result.runs) {
        REQUIRE(run.gpu_passes.size() == 2); // the renderer's passes: the scene, then tone mapping
        for (const auto* label : {"view", "tone map"}) {
            const auto& series = run.gpu_passes.at(label);
            REQUIRE(series.size() == manifest.samples);
            for (const auto& value : series) {
                REQUIRE(value);
                CHECK(*value == device.pass_ms + 0.01); // vertex and fragment stages
            }
        }
        CHECK(run.gpu_pass_mismatches == 0);
        CHECK(run.untimed_passes == 0);
    }
    REQUIRE(result.uninstrumented);
    CHECK(result.uninstrumented->gpu_passes.empty()); // matched without pass timing...
    CHECK(device.gpu_pass_timing_enabled()); // ...which is turned back on afterwards
    const auto json = to_json(result);
    CHECK(has(json, "\"gpu_pass_ms\":{\"tone map\":{\"count\":6"));
    CHECK(has(json, "\"view\":{\"count\":6"));
    CHECK(has(json, "\"gpu_pass_mismatches\":0"));
    CHECK(has(json, "\"gpu_difference_percent\":"));
    CHECK_FALSE(std::ranges::any_of(result.unavailable, [](const auto& entry) { return entry.first == "gpu_pass_time"; }));

    // A pass outside its frame's GPU time is a measurement fault, counted and reported.
    device.overrun = true;
    const auto faulty = run(manifest, device, shader);
    CHECK(faulty.runs.front().gpu_pass_mismatches == manifest.samples);
    CHECK(has(to_text(faulty), "OUTSIDE THEIR FRAME"));

    // A device that cannot time passes says why.
    Device null;
    const auto unavailable = run(manifest, null, shader);
    CHECK(std::ranges::find(unavailable.unavailable, std::pair<std::string, std::string>{"gpu_pass_time", "the null device executes no GPU work"}) !=
          unavailable.unavailable.end());
}

TEST_CASE("Presenting runs record when each frame was shown, its interval, and missed deadlines", "[benchmark]") {
    const auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
    auto manifest = small(Workload::scene);
    manifest.present = true;
    manifest.runs = 1;
    manifest.overhead = false;
    test::TimingDevice device({.surface = true, .surface_width = 64, .surface_height = 36});
    auto window = 1;
    REQUIRE(device.initialize(&window, {3, size_t{8} << 20}));
    // The fourth sampled frame is never shown: the interval across it is two refresh periods.
    device.dropped = {device.stats().submitted_frames + manifest.warmup + 4};
    auto polls = 0;
    const auto result = run(manifest, device, shader, [&] { ++polls; });
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    CHECK(polls >= int(manifest.warmup + manifest.samples)); // the window's events, every frame
    REQUIRE(result.refresh_hz == 120.0);
    const auto& run = result.runs.front();
    REQUIRE(run.presented.size() == manifest.samples);
    const auto paced = pacing(run, result.refresh_hz);
    CHECK(paced.shown == manifest.samples - 1);
    CHECK(paced.not_shown == 1);
    CHECK(paced.unreported == 0);
    REQUIRE(paced.intervals.size() == manifest.samples - 2);
    CHECK(std::abs(paced.intervals[0] - 1000.0 / 120.0) < 1e-6);
    CHECK(paced.missed == 1);
    CHECK(run.gpu_passes.contains("present view")); // the presentation pass is timed too
    const auto json = to_json(result);
    CHECK(has(json, "\"present\":true"));
    CHECK(has(json, "presented to a window every frame"));
    CHECK(has(json, "\"presentation\":{\"shown\":5,\"not_shown\":1,\"unreported\":0,\"refresh_hz\":120"));
    CHECK(has(json, "\"missed_deadlines\":1"));
    CHECK(has(json, "\"present_interval_ms\":["));
    CHECK(has(to_text(result), "(presenting)"));
    CHECK(has(to_text(result), "1 missed deadlines at 120.000 Hz"));
    CHECK(has(to_text(result), "WARNING: 1 frames were never shown and 0 not reported")); // not valid pacing
    CHECK_FALSE(std::ranges::any_of(result.unavailable, [](const auto& entry) { return entry.first == "present_pacing"; }));

    // A device that does not report presents: every frame is unreported, and the reason is given.
    device.presents = false;
    const auto silent = benchmark::run(manifest, device, shader);
    CHECK(pacing(silent.runs.front(), silent.refresh_hz).unreported == manifest.samples);
    CHECK(std::ranges::find(silent.unavailable, std::pair<std::string, std::string>{"present_pacing", "this device does not report when frames are shown"}) !=
          silent.unavailable.end());

    // Without a window there is nothing to present into.
    Device headless;
    const auto refused = benchmark::run(manifest, headless, shader);
    CHECK(refused.failure == "'present on' needs a window; this device has no surface");
    // Offscreen runs say pacing does not apply.
    manifest.present = false;
    const auto offscreen = benchmark::run(manifest, headless, shader);
    CHECK(std::ranges::any_of(offscreen.unavailable, [](const auto& entry) { return entry.first == "present_pacing"; }));
    CHECK(has(to_json(offscreen), "offscreen, never presented"));
}

TEST_CASE("On Metal every sampled frame gets its own GPU time, and warmup frames none", "[benchmark][gpu]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr, {3, size_t{8} << 20}));
    const auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
    REQUIRE_FALSE(shader.empty());
    auto manifest = small(Workload::instances);
    manifest.warmup = 40; // more than the frames in flight, so warmup timings arrive during sampling
    manifest.samples = 1100; // more than the device keeps waiting, in both the instrumented and matched runs
    manifest.runs = 1;
    const auto result = run(manifest, device, shader);
    INFO(result.failure);
    REQUIRE(result.failure.empty());
    REQUIRE(result.uninstrumented);
    for (const auto& run : {result.runs.front(), *result.uninstrumented}) {
        REQUIRE(run.gpu.size() == 1100);
        for (const auto& gpu : run.gpu) {
            REQUIRE(gpu);
            CHECK(*gpu > 0.0);
        }
    }
    CHECK(result.resident.gpu_reported);
    CHECK(result.resident.gpu_reported.value_or(0) > 0);
    CHECK_FALSE(std::ranges::any_of(result.unavailable, [](const auto& entry) { return entry.first == "gpu_frame_time"; }));
    // Every sampled frame's one pass is timed from GPU timestamps, inside its frame; none in the matched run.
    const auto& passes = result.runs.front().gpu_passes;
    REQUIRE(passes.contains("view"));
    for (const auto& value : passes.at("view")) {
        REQUIRE(value);
        CHECK(*value > 0.0);
    }
    CHECK(result.runs.front().gpu_pass_mismatches == 0);
    CHECK(result.uninstrumented->gpu_passes.empty());
    device.shutdown();
}
