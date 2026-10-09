#include "benchmark.hpp"
#include "benchmark_detail.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/script_assets.hpp"
#include "maya/world/spatial.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <pthread.h>
#include <unistd.h>

namespace maya::benchmark {
namespace fs = std::filesystem;
namespace {
using detail::camera_id;
using detail::first_instance;
using detail::light_id;
using detail::looking;

const char* workload_name(Workload workload) {
    switch (workload) {
    case Workload::instances: return "instances";
    case Workload::scene: return "scene";
    case Workload::load_cycles: return "load_cycles";
    case Workload::play_cycles: return "play_cycles";
    case Workload::physics: return "physics";
    case Workload::import: return "import";
    case Workload::animation: return "animation";
    case Workload::stream: return "stream";
    }
    return "?";
}
/// Workloads that generate a scene of one mesh and material.
bool generated(Workload workload) {
    return workload != Workload::scene && workload != Workload::physics && workload != Workload::import && workload != Workload::animation &&
           workload != Workload::stream;
}

// Manifests -------------------------------------------------------------------------------------------

ManifestResult fail(size_t line, const std::string& message) {
    return {{}, "line " + std::to_string(line) + ": " + message};
}

template<class T> bool number(std::istringstream& in, T& value) {
    auto word = std::string{};
    if (!(in >> word)) return false;
    const auto* end = word.data() + word.size();
    if constexpr (std::is_floating_point_v<T>) {
        auto parsed = double{};
        const auto [ptr, error] = std::from_chars(word.data(), end, parsed);
        if (error != std::errc{} || ptr != end || !std::isfinite(parsed)) return false;
        value = static_cast<T>(parsed);
        return true;
    } else {
        const auto [ptr, error] = std::from_chars(word.data(), end, value);
        return error == std::errc{} && ptr == end;
    }
}
bool asset_id(std::istringstream& in, AssetId& id) {
    auto high = std::string{}, low = std::string{};
    if (!(in >> high >> low)) return false;
    const auto word = [](const std::string& text, uint64_t& value) {
        const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        return error == std::errc{} && ptr == text.data() + text.size();
    };
    return word(high, id.high) && word(low, id.low) && id.valid();
}

// Scene generation ------------------------------------------------------------------------------------

using detail::mix;

/// I1: `count` entities on a square grid, sharing one mesh and material; exactly
/// round(rotating × count) of them, chosen by seed, spin. A camera and a directional light.
SceneDocument generate(const Manifest& manifest) {
    auto document = SceneDocument{};
    document.entities.reserve(manifest.count + 2);
    auto camera = TransformComponent{};
    camera.translation = manifest.camera_position;
    camera.rotation = looking(manifest.camera_position, manifest.camera_target);
    document.entities.push_back({camera_id, std::nullopt, {NameComponent{"Benchmark camera"}, camera, CameraComponent{}}});
    auto sun = TransformComponent{};
    sun.rotation = math::Quat(-0.5161719f, 0.2429044f, 0.0f, 0.82131857f);
    // Unshadowed, so generated workloads (I1, L1) compare with results from before shadows (#1034).
    auto light = LightComponent{};
    light.cast_shadows = false;
    document.entities.push_back({light_id, std::nullopt, {NameComponent{"Sun"}, sun, light}});

    auto order = std::vector<uint32_t>(manifest.count);
    std::iota(order.begin(), order.end(), 0u);
    std::ranges::sort(order, {}, [&](uint32_t index) { return mix(manifest.seed ^ mix(index)); });
    const auto spinning = size_t(std::llround(manifest.rotating * manifest.count));
    auto spins = std::vector<bool>(manifest.count, false);
    for (size_t i = 0; i < spinning; ++i) spins[order[i]] = true;

    const auto side = uint32_t(std::ceil(std::sqrt(double(manifest.count))));
    const auto offset = (float(side) - 1.0f) * 0.5f * manifest.spacing;
    for (uint32_t i = 0; i < manifest.count; ++i) {
        auto transform = TransformComponent{};
        transform.translation = {float(i % side) * manifest.spacing - offset, 0.0f, float(i / side) * manifest.spacing - offset};
        auto renderer = MeshRendererComponent{};
        renderer.mesh = {manifest.mesh};
        renderer.material = {manifest.material};
        auto entity = SceneEntity{EntityId{0x62656e63, first_instance + i}, std::nullopt, {transform, renderer}};
        if (spins[i]) entity.components.push_back(SpinComponent{{0.0f, 1.0f, 0.0f}, 1.0f});
        document.entities.push_back(std::move(entity));
    }
    return document;
}

std::string scene_text(const SceneDocument& document, const PropertyValidationContext& context) {
    auto out = std::ostringstream{};
    if (auto problems = write_scene(out, document, context); !problems.empty()) throw std::runtime_error(problems.front().message);
    return out.str();
}

// Measured frames -------------------------------------------------------------------------------------

struct Stage {
    GraphicsDevice& device;
    AssetRegistry& registry;
    Renderer& renderer;
    RenderTarget& target;
    uint32_t width, height;
    bool present = false; // also into the device's surface
    const std::function<void()>* poll = nullptr; // between frames, untimed
    DebugView debug_view = DebugView::none;
    SkinBindingCache& skins; // skins' joints between frames, as the player keeps them
};

struct FrameSample {
    double frame = 0.0, simulation = 0.0, wait = 0.0, extract = 0.0, encode = 0.0, submit = 0.0;
    double finalize = 0.0; // the registry's update: finalizing loads
    bool loading = false; // something the view draws was still loading
    size_t skipped = 0, pending = 0, pending_textures = 0; // the snapshot's
};
/// The systems each workload plays: A1 every system of play, so its clips play; the others the built-in ones.
std::vector<std::unique_ptr<SimulationSystem>> systems_for(const Manifest& manifest, AssetRegistry& registry) {
    if (manifest.workload == Workload::scene) // as the player runs it: scripts and clips too
        return play_systems(registry_script_sources(registry), registry_animation_clips(registry));
    if (manifest.workload != Workload::animation) return builtin_systems();
    return play_systems([](AssetId) -> ScriptSourceResult { return {std::nullopt, "the benchmark has no scripts"}; },
                        registry_animation_clips(registry));
}

/// One frame of a fixed-workload run: exactly one simulation tick, then extraction, encoding, and
/// submission of an offscreen view, presented into the surface when the stage presents.
FrameSample run_frame(const Stage& stage, PlaySession& session, EntityId camera, bool instrumented) {
    if (stage.poll && *stage.poll) (*stage.poll)(); // the window's events, outside the frame's time
    auto sample = FrameSample{};
    auto frame = Stopwatch{};
    auto part = Stopwatch{};
    const auto lap = [&](double& into) {
        if (!instrumented) return;
        into = part.milliseconds();
        part.restart();
    };
    // Loading first, as the player does: prepared assets are finalized within the budget, and the frame
    // is marked so that a synchronous load in it is counted (docs/assets.md#asynchronous-loading).
    sample.finalize = stage.registry.update().last_ms;
    stage.registry.begin_frame();
    struct EndFrame {
        AssetRegistry& assets;
        ~EndFrame() { assets.end_frame(); }
    } end_frame{stage.registry};
    if (const auto played = session.update(session.clock().interval()); !played.error.empty())
        throw std::runtime_error(played.error);
    lap(sample.simulation);
    if (auto error = stage.device.begin_frame()) throw std::runtime_error(error.message);
    lap(sample.wait);
    const auto& world = session.world();
    const auto handle = world.find(camera);
    if (!handle) throw std::runtime_error("The camera entity is missing");
    auto view = extract_render_view(world, *handle, stage.width, stage.height);
    if (!view) throw std::runtime_error("The camera has no valid view");
    view->debug_view = stage.debug_view;
    auto options = RenderExtractOptions{};
    options.skins = &stage.skins;
    options.loading = AssetLoading::stream; // frames never wait for assets
    options.origin = view->position; // camera-relative (#1065)
    const auto snapshot = extract_render_snapshot(world, stage.registry, options);
    if (!snapshot.diagnostics.empty()) throw std::runtime_error(snapshot.diagnostics.front().message);
    sample.loading = snapshot.stats.pending > 0 || snapshot.stats.pending_textures > 0 || snapshot.stats.environment_pending;
    sample.skipped = snapshot.stats.skipped;
    sample.pending = snapshot.stats.pending;
    sample.pending_textures = snapshot.stats.pending_textures;
    lap(sample.extract);
    if (auto error = stage.renderer.render(snapshot, *view, stage.target)) throw std::runtime_error(error.message);
    if (stage.present)
        if (const auto surface = stage.device.acquire_surface()) // none this frame: it is counted as not shown
            if (auto error = stage.renderer.present(stage.target, surface.target.texture, {0, 0, surface.target.width, surface.target.height}))
                throw std::runtime_error(error.message);
    lap(sample.encode);
    if (auto error = stage.device.end_frame()) throw std::runtime_error(error.message);
    lap(sample.submit);
    sample.frame = frame.milliseconds();
    return sample;
}

/// With MAYA_BENCHMARK_FOOTPRINT=<folder>, the footprint tool's account of this process by kind of memory
/// (footprint-<label>.json): what attributes the load cycles' plateau (#1063, docs/performance.md#memory).
void footprint_snapshot(const std::string& label) {
    const auto* folder = std::getenv("MAYA_BENCHMARK_FOOTPRINT");
    if (!folder) return;
    const auto command = "footprint --json '" + (fs::path(folder) / ("footprint-" + label + ".json")).string() + "' -p " +
                         std::to_string(::getpid()) + " > /dev/null 2>&1";
    if (std::system(command.c_str()) != 0) std::cerr << "[Benchmark] footprint failed for " << label << '\n';
}

MemorySample memory(GraphicsDevice& device, const AssetRegistry& registry, size_t entities, size_t renderer_gpu = 0) {
    return {device.stats(), device.reported_memory(), process_memory(), registry.residency(), entities, renderer_gpu};
}

/// A run: a fresh play session from the document, warmup frames, then sampled frames, with GPU
/// timings matched to sampled frames by submission serial.
RunSamples measure(const Manifest& manifest, const Stage& stage, const SceneDocument& document,
                   const PropertyValidationContext& context, EntityId camera, bool instrumented, Result& result,
                   bool record_resident) {
    auto started = PlaySession::start(document, context, systems_for(manifest, stage.registry));
    if (!started) throw std::runtime_error(started.error.empty() ? started.diagnostics.front().message : started.error);
    auto& session = *started.session;
    preload_render_assets(session.world(), stage.registry); // a loading screen: nothing streams in while measured
    for (uint32_t i = 0; i < manifest.warmup; ++i) run_frame(stage, session, camera, instrumented);
    auto samples = RunSamples{};
    samples.instrumented = instrumented;
    samples.gpu.assign(manifest.samples, std::nullopt);
    if (stage.present) {
        samples.presented.assign(manifest.samples, std::nullopt);
        samples.present_reported.assign(manifest.samples, false);
    }
    const auto first_serial = stage.device.stats().submitted_frames + 1;
    const auto sampled = [&](uint64_t serial) { return serial >= first_serial && serial < first_serial + manifest.samples; };
    const auto collect = [&] { // warmup frames' timings fall outside the sampled serials
        for (const auto& timing : stage.device.take_gpu_timings()) {
            if (!sampled(timing.frame)) continue;
            const auto index = timing.frame - first_serial;
            samples.gpu[index] = timing.milliseconds;
            samples.untimed_passes += timing.untimed_passes;
            for (const auto& pass : timing.passes) {
                auto& series = samples.gpu_passes[pass.label];
                if (series.empty()) series.assign(manifest.samples, std::nullopt);
                series[index] = series[index].value_or(0.0) + pass.milliseconds();
                // Measured within the frame's own GPU execution; anything else is a measurement fault.
                if (pass.start_ms < -0.05 || pass.end_ms > timing.milliseconds + 0.05) ++samples.gpu_pass_mismatches;
            }
        }
        for (const auto& present : stage.device.take_present_timings())
            if (stage.present && sampled(present.frame)) {
                samples.presented[present.frame - first_serial] = present.presented;
                samples.present_reported[present.frame - first_serial] = true;
            }
    };
    auto most = FrameSample{};
    for (uint32_t i = 0; i < manifest.samples; ++i) {
        const auto frame = run_frame(stage, session, camera, instrumented);
        most.skipped = std::max(most.skipped, frame.skipped);
        most.pending = std::max(most.pending, frame.pending);
        most.pending_textures = std::max(most.pending_textures, frame.pending_textures);
        samples.frame.push_back(frame.frame);
        samples.sampled_seconds += frame.frame / 1000.0;
        if (instrumented) {
            for (const auto& timing : session.system_timings())
                samples.systems[std::string(timing.name)].push_back(timing.fixed_update_ms + timing.late_fixed_update_ms);
            samples.simulation.push_back(frame.simulation);
            samples.wait.push_back(frame.wait);
            samples.extract.push_back(frame.extract);
            samples.encode.push_back(frame.encode);
            samples.submit.push_back(frame.submit);
        }
        // Both modes collect alike, so the matched runs differ only by the CPU scopes; the device keeps
        // at most RhiCompletion::timing_capacity timings, so waiting until the end would lose samples.
        if (i % 64 == 63) collect();
    }
    stage.device.wait_idle();
    collect();
    // The last frames are shown at later display refreshes, after their GPU work completed.
    if (stage.present && stage.device.present_timing_supported())
        for (auto waited = Stopwatch{}; waited.milliseconds() < 1000.0 && std::ranges::count(samples.present_reported, false) > 0;) {
            if (stage.poll && *stage.poll) (*stage.poll)();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            collect();
        }
    if (record_resident) {
        const auto stats = stage.device.stats();
        auto& counters = result.counters;
        counters.draws = stats.frame_draws;
        counters.instances = stats.frame_instances;
        counters.triangles = stats.frame_triangles;
        counters.passes = stats.frame_passes;
        const auto& world = session.world();
        counters.entities = world.size();
        counters.mesh_renderers = world.component_count<MeshRendererComponent>();
        counters.spinning = world.component_count<SpinComponent>();
        result.resident = memory(stage.device, stage.registry, world.size());
        result.loading = stage.registry.load_stats();
        counters.unique_meshes = result.resident.assets.meshes;
        counters.unique_materials = result.resident.assets.materials;
        counters.centers_in_view = stage.renderer.last_view().drawn; // the view culls by bounds (#1025)
        counters.skipped = most.skipped;
        counters.pending = most.pending;
        counters.pending_textures = most.pending_textures;
        const auto& lights = stage.renderer.last_lights();
        counters.local_lights = lights.local;
        counters.dropped_lights = lights.dropped.size();
        counters.unshadowed_lights = lights.unshadowed.size();
        const auto& rendered = stage.renderer.stats();
        if (rendered.views > 0) {
            counters.shadow_maps = double(rendered.shadow_maps) / double(rendered.views);
            counters.shadow_draws = double(rendered.shadow_draws) / double(rendered.views);
            counters.joints = size_t(rendered.joints / rendered.views);
        }
        counters.animated = world.component_count<AnimationComponent>();
        const auto last = extract_render_snapshot(world, stage.registry);
        counters.skinned = size_t(std::ranges::count_if(last.instances, [](const RenderInstance& instance) { return instance.joint_count > 0; }));
    }
    return samples;
}

std::string thread_qos() {
    auto qos = QOS_CLASS_UNSPECIFIED;
    auto relative = 0;
    pthread_get_qos_class_np(pthread_self(), &qos, &relative);
    switch (qos) {
    case QOS_CLASS_USER_INTERACTIVE: return "user_interactive";
    case QOS_CLASS_USER_INITIATED: return "user_initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    default: return "unspecified";
    }
}

// JSON ------------------------------------------------------------------------------------------------

class Json {
public:
    std::string text;
    void open(char bracket) { item(); text += bracket; m_first.push_back(true); }
    void close(char bracket) { m_first.pop_back(); text += bracket; }
    void key(std::string_view name) {
        item();
        string(name);
        text += ':';
        m_keyed = true;
    }
    void string(std::string_view value) {
        text += '"';
        for (const auto c : value) {
            switch (c) {
            case '"': text += "\\\""; break;
            case '\\': text += "\\\\"; break;
            case '\n': text += "\\n"; break;
            case '\t': text += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    text += escaped;
                } else {
                    text += c;
                }
            }
        }
        text += '"';
    }
    void value(std::string_view v) { item(); string(v); }
    void value(const char* v) { value(std::string_view(v)); }
    void value(bool v) { item(); text += v ? "true" : "false"; }
    void value(double v) {
        item();
        if (!std::isfinite(v)) { text += "null"; return; }
        char buffer[32];
        const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), v);
        text.append(buffer, error == std::errc{} ? end : buffer);
    }
    template<class T> requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
    void value(T v) { item(); text += std::to_string(v); }
    void null() { item(); text += "null"; }
    template<class T> void field(std::string_view name, const T& v) { key(name); value(v); }

private:
    void item() {
        if (m_keyed) { m_keyed = false; return; }
        if (!m_first.empty()) {
            if (!m_first.back()) text += ',';
            m_first.back() = false;
        }
    }
    std::vector<bool> m_first;
    bool m_keyed = false;
};

void write_summary(Json& json, std::string_view name, std::span<const double> samples) {
    const auto s = summarize(samples);
    json.key(name);
    json.open('{');
    json.field("count", s.count);
    json.field("mean", s.mean);
    json.field("min", s.min);
    json.field("p50", s.p50);
    json.field("p95", s.p95);
    json.field("p99", s.p99);
    json.field("max", s.max);
    json.close('}');
}

void write_memory(Json& json, std::string_view name, const MemorySample& m) {
    json.key(name);
    json.open('{');
    json.key("tracked");
    json.open('{');
    json.field("buffers", m.device.buffers);
    json.field("textures", m.device.textures);
    json.field("pipelines", m.device.pipelines);
    json.field("pending_retirements", m.device.pending_retirements);
    json.field("buffer_bytes", m.device.buffer_bytes);
    json.field("texture_bytes", m.device.texture_bytes);
    json.field("pending_retirement_bytes", m.device.pending_retirement_bytes);
    json.field("upload_bytes", m.device.upload_bytes);
    json.field("transient_high_water", m.device.transient_high_water);
    json.field("transient_failures", m.device.transient_failures);
    json.field("frame_waits", m.device.frame_waits);
    json.field("entities", m.entities);
    json.key("assets");
    json.open('{');
    json.field("entries", m.assets.entries);
    json.field("ready", m.assets.ready);
    json.field("failed", m.assets.failed);
    json.field("resident_meshes", m.assets.meshes);
    json.field("resident_materials", m.assets.materials);
    json.field("leased", m.assets.leased);
    json.field("mesh_gpu_bytes", m.assets.mesh_gpu_bytes);
    json.field("mesh_cpu_bytes", m.assets.mesh_cpu_bytes);
    json.field("resident_textures", m.assets.textures);
    json.field("texture_gpu_bytes", m.assets.texture_gpu_bytes);
    json.field("resident_environments", m.assets.environments);
    json.field("environment_gpu_bytes", m.assets.environment_gpu_bytes);
    json.field("resident_skins", m.assets.skins);
    json.field("resident_animations", m.assets.animations);
    json.close('}');
    json.close('}');
    // By category (#1063, docs/assets.md#residency), against the device's tracked bytes.
    const auto report = residency_report(m.assets, 0, m.renderer_gpu, m.device, m.gpu_reported,
                                         m.process ? std::optional(size_t(m.process->footprint)) : std::nullopt);
    json.key("residency");
    json.open('{');
    for (size_t c = 0; c < residency_category_count; ++c) {
        json.key(residency_category_name(ResidencyCategory(c)));
        json.open('{');
        json.field("cpu_bytes", report.bytes[c].cpu);
        json.field("gpu_bytes", report.bytes[c].gpu);
        json.field("leased_bytes", report.leased[c]);
        json.field("budget_bytes", report.budgets.bytes[c]);
        json.close('}');
    }
    json.field("total_bytes", report.total());
    json.field("budget_total_bytes", report.budgets.total);
    json.field("tracked_gpu_bytes", report.tracked_gpu);
    json.field("unattributed_gpu_bytes", report.unattributed_gpu);
    json.close('}');
    json.key("reported");
    json.open('{');
    json.key("gpu_allocated_bytes");
    if (m.gpu_reported) json.value(*m.gpu_reported); else json.null();
    json.key("process_footprint_bytes");
    if (m.process) json.value(m.process->footprint); else json.null();
    json.key("process_resident_bytes");
    if (m.process) json.value(m.process->resident); else json.null();
    json.key("process_peak_resident_bytes");
    if (m.process) json.value(m.process->peak_resident); else json.null();
    json.close('}');
    json.close('}');
}

void write_run(Json& json, const RunSamples& run, std::optional<double> refresh_hz) {
    json.open('{');
    json.field("instrumented", run.instrumented);
    json.field("sampled_seconds", run.sampled_seconds);
    json.field("throughput_fps", run.sampled_seconds > 0.0 ? double(run.frame.size()) / run.sampled_seconds : 0.0);
    write_summary(json, "frame_ms", run.frame);
    if (run.instrumented) {
        json.key("cpu_ms");
        json.open('{');
        write_summary(json, "simulation", run.simulation);
        write_summary(json, "wait", run.wait);
        write_summary(json, "extract", run.extract);
        write_summary(json, "encode", run.encode);
        write_summary(json, "submit", run.submit);
        // Each play system's time in the frame's tick (A1's animation among them).
        json.key("systems");
        json.open('{');
        for (const auto& [name, values] : run.systems) write_summary(json, name, values);
        json.close('}');
        json.close('}');
    }
    auto gpu = std::vector<double>{};
    for (const auto& value : run.gpu) if (value) gpu.push_back(*value);
    write_summary(json, "gpu_ms", gpu);
    json.field("gpu_samples_missing", run.gpu.size() - gpu.size());
    // Per pass label: the GPU time of a frame's passes with that label, over the frames that had any.
    json.key("gpu_pass_ms");
    json.open('{');
    for (const auto& [label, series] : run.gpu_passes) {
        auto values = std::vector<double>{};
        for (const auto& value : series) if (value) values.push_back(*value);
        write_summary(json, label, values);
    }
    json.close('}');
    json.field("gpu_pass_mismatches", run.gpu_pass_mismatches);
    json.field("untimed_passes", run.untimed_passes);
    const auto paced = pacing(run, refresh_hz);
    if (!run.presented.empty()) {
        json.key("presentation");
        json.open('{');
        json.field("shown", paced.shown);
        json.field("not_shown", paced.not_shown);
        json.field("unreported", paced.unreported);
        json.key("refresh_hz");
        if (refresh_hz) json.value(*refresh_hz); else json.null();
        write_summary(json, "interval_ms", paced.intervals);
        json.key("missed_deadlines");
        if (refresh_hz) json.value(paced.missed); else json.null();
        json.close('}');
    }
    json.key("samples");
    json.open('{');
    const auto series = [&](std::string_view name, const std::vector<double>& values) {
        json.key(name);
        json.open('[');
        for (const auto v : values) json.value(v);
        json.close(']');
    };
    series("frame_ms", run.frame);
    if (run.instrumented) {
        series("simulation_ms", run.simulation);
        series("wait_ms", run.wait);
        series("extract_ms", run.extract);
        series("encode_ms", run.encode);
        series("submit_ms", run.submit);
    }
    json.key("gpu_pass_ms");
    json.open('{');
    for (const auto& [label, series] : run.gpu_passes) {
        json.key(label);
        json.open('[');
        for (const auto& v : series) { if (v) json.value(*v); else json.null(); }
        json.close(']');
    }
    json.close('}');
    if (!run.presented.empty()) series("present_interval_ms", paced.intervals);
    json.key("gpu_ms");
    json.open('[');
    for (const auto& v : run.gpu) { if (v) json.value(*v); else json.null(); }
    json.close(']');
    json.close('}');
    json.close('}');
}

double slope(const std::vector<double>& ys) {
    if (ys.size() < 2) return 0.0;
    const auto n = double(ys.size());
    const auto mean_x = (n - 1.0) / 2.0;
    const auto mean_y = std::accumulate(ys.begin(), ys.end(), 0.0) / n;
    auto num = 0.0, den = 0.0;
    for (size_t i = 0; i < ys.size(); ++i) {
        num += (double(i) - mean_x) * (ys[i] - mean_y);
        den += (double(i) - mean_x) * (double(i) - mean_x);
    }
    return num / den;
}

void write_physics(Json& json, const Result& r) {
    const auto& m = r.manifest;
    const auto& s = r.physics_scene;
    json.key("physics");
    json.open('{');
    json.key("recipe");
    json.open('{');
    json.field("version", 1);
    json.field("dynamic_bodies", m.count);
    json.field("obstacles", m.obstacles);
    json.field("scripted", m.scripted);
    json.field("sensors", m.sensors);
    json.field("rays", m.rays);
    json.field("overlaps", m.overlaps);
    json.field("shape_casts", m.casts);
    json.key("workers");
    json.open('[');
    for (const auto workers : m.workers) json.value(workers);
    json.close(']');
    json.close('}');
    json.key("scene");
    json.open('{');
    json.field("bodies", s.bodies);
    json.field("static", s.static_bodies);
    json.field("kinematic", s.kinematic_bodies);
    json.field("dynamic", s.dynamic_bodies);
    json.field("dropped", s.active_set);
    json.field("resting", s.sleeping_set);
    json.field("boxes", s.boxes);
    json.field("spheres", s.spheres);
    json.field("capsules", s.capsules);
    json.field("obstacles", s.obstacles);
    json.field("sensors", s.sensors);
    json.field("scripted", s.scripted);
    json.close('}');
    json.key("deterministic");
    if (r.deterministic) json.value(*r.deterministic); else json.null();
    json.key("runs");
    json.open('[');
    for (const auto& run : r.physics_runs) {
        json.open('{');
        json.field("workers", run.workers);
        json.field("worker_threads", run.worker_threads);
        json.key("failure");
        if (run.failure.empty()) json.null(); else json.value(run.failure);
        json.field("start_ms", run.start_ms);
        json.key("ms");
        json.open('{');
        write_summary(json, "tick", run.tick);
        write_summary(json, "scripts", run.scripts);
        write_summary(json, "queries", run.queries);
        write_summary(json, "other_systems", run.other_systems);
        write_summary(json, "prepare", run.prepare);
        write_summary(json, "step", run.step);
        write_summary(json, "synchronize", run.synchronize);
        write_summary(json, "events", run.events);
        write_summary(json, "post_physics_hooks", run.late);
        write_summary(json, "body_commit", run.commit);
        json.close('}');
        json.key("per_tick");
        json.open('{');
        write_summary(json, "active_bodies", run.active);
        write_summary(json, "sleeping_bodies", run.sleeping);
        write_summary(json, "pairs", run.pairs);
        write_summary(json, "contacts", run.contacts);
        write_summary(json, "events", run.events_delivered);
        write_summary(json, "query_hits", run.query_hits);
        json.close('}');
        json.key("memory");
        json.open('{');
        json.field("jolt_live_start_bytes", run.jolt_live_start);
        json.field("jolt_live_end_bytes", run.jolt_live_end);
        json.field("jolt_peak_bytes", run.jolt_peak);
        json.field("temp_high_water_bytes", run.temp_high_water);
        json.field("temp_capacity_bytes", run.temp_capacity);
        json.field("script_vm_bytes", run.script_bytes);
        json.key("footprint_start_bytes");
        if (run.footprint_start) json.value(*run.footprint_start); else json.null();
        json.key("footprint_end_bytes");
        if (run.footprint_end) json.value(*run.footprint_end); else json.null();
        json.field("footprint_slope_bytes_per_tick", run.footprint_slope_per_tick);
        json.close('}');
        char state[17];
        std::snprintf(state, sizeof(state), "%016llx", static_cast<unsigned long long>(run.state));
        json.field("state_hash", std::string_view(state));
        json.key("tick_ms");
        json.open('[');
        for (const auto v : run.tick) json.value(v);
        json.close(']');
        json.close('}');
    }
    json.close(']');
    json.close('}');
}

} // namespace

ManifestResult read_manifest(std::istream& input, const fs::path& folder) {
    auto manifest = Manifest{};
    auto line = std::string{};
    size_t number_of_line = 0;
    auto seen = std::vector<std::string>{};
    auto header = false;
    while (std::getline(input, line)) {
        ++number_of_line;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        auto in = std::istringstream(line);
        auto key = std::string{};
        in >> key;
        if (!header) {
            auto version = 0u;
            if (key != "maya-benchmark" || !(in >> version)) return fail(number_of_line, "expected a maya-benchmark header");
            if (version != 1) return fail(number_of_line, "unsupported benchmark version " + std::to_string(version));
            header = true;
            continue;
        }
        if (std::ranges::find(seen, key) != seen.end()) return fail(number_of_line, "'" + key + "' appears twice");
        seen.push_back(key);
        auto ok = true;
        auto text = std::string{};
        if (key == "name") {
            ok = bool(in >> std::quoted(text)) && !text.empty();
            manifest.name = text;
        } else if (key == "workload") {
            ok = bool(in >> text);
            if (text == "instances") manifest.workload = Workload::instances;
            else if (text == "scene") manifest.workload = Workload::scene;
            else if (text == "load_cycles") manifest.workload = Workload::load_cycles;
            else if (text == "play_cycles") manifest.workload = Workload::play_cycles;
            else if (text == "physics") manifest.workload = Workload::physics;
            else if (text == "import") manifest.workload = Workload::import;
            else if (text == "animation") manifest.workload = Workload::animation;
            else if (text == "stream") manifest.workload = Workload::stream;
            else return fail(number_of_line, "unknown workload '" + text + "'; use instances, scene, load_cycles, play_cycles, physics, import, animation, or stream");
        } else if (key == "content") {
            ok = bool(in >> std::quoted(text)) && !text.empty();
            manifest.content = (folder / text).lexically_normal();
        } else if (key == "models") {
            while (in >> std::quoted(text)) manifest.models.emplace_back(text);
            ok = !manifest.models.empty();
        } else if (key == "project") {
            ok = bool(in >> std::quoted(text)) && !text.empty();
            manifest.project = (folder / text).lexically_normal();
        } else if (key == "scene") {
            ok = bool(in >> std::quoted(text)) && !text.empty();
            manifest.scene = text;
        } else if (key == "mesh") ok = asset_id(in, manifest.mesh);
        else if (key == "material") ok = asset_id(in, manifest.material);
        else if (key == "count") ok = number(in, manifest.count) && manifest.count > 0 && manifest.count <= 10'000'000;
        else if (key == "rotating") ok = number(in, manifest.rotating) && manifest.rotating >= 0.0 && manifest.rotating <= 1.0;
        else if (key == "spacing") ok = number(in, manifest.spacing) && manifest.spacing > 0.0f;
        else if (key == "seed") ok = number(in, manifest.seed);
        else if (key == "camera") {
            auto& p = manifest.camera_position;
            auto& t = manifest.camera_target;
            ok = number(in, p.x) && number(in, p.y) && number(in, p.z) && number(in, t.x) && number(in, t.y) && number(in, t.z) &&
                 (t - p).length() > 1e-3f;
        } else if (key == "resolution")
            ok = number(in, manifest.width) && number(in, manifest.height) && manifest.width > 0 && manifest.height > 0 &&
                 manifest.width <= 16384 && manifest.height <= 16384;
        else if (key == "upload_mib") ok = number(in, manifest.upload_mib) && manifest.upload_mib > 0 && manifest.upload_mib <= 1024;
        else if (key == "warmup") ok = number(in, manifest.warmup);
        else if (key == "samples") ok = number(in, manifest.samples) && manifest.samples > 0;
        else if (key == "runs") ok = number(in, manifest.runs) && manifest.runs > 0;
        else if (key == "cycles") ok = number(in, manifest.cycles) && manifest.cycles > 0;
        else if (key == "ticks") ok = number(in, manifest.ticks) && manifest.ticks > 0;
        else if (key == "slope_from") ok = number(in, manifest.slope_from) && manifest.slope_from > 0;
        else if (key == "obstacles") ok = number(in, manifest.obstacles);
        else if (key == "scripted") ok = number(in, manifest.scripted);
        else if (key == "sensors") ok = number(in, manifest.sensors);
        else if (key == "queries") ok = number(in, manifest.rays) && number(in, manifest.overlaps) && number(in, manifest.casts);
        else if (key == "workers") {
            manifest.workers.clear();
            while (in >> text) {
                auto count = 0;
                if (text == "default") count = -1;
                else if (const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), count);
                         error != std::errc{} || ptr != text.data() + text.size() || count < 0 || count > 64)
                    return fail(number_of_line, "workers are 'default' or a count from 0 to 64, not '" + text + "'");
                manifest.workers.push_back(count);
            }
            ok = !manifest.workers.empty();
        }
        else if (key == "stream_load") {
            ok = bool(in >> text) && (text == "on" || text == "off");
            manifest.stream_load = text == "on";
        } else if (key == "overhead") {
            ok = bool(in >> text) && (text == "on" || text == "off");
            manifest.overhead = text == "on";
        } else if (key == "present") {
            ok = bool(in >> text) && (text == "on" || text == "off");
            manifest.present = text == "on";
        } else if (key == "grid") {
            ok = bool(in >> manifest.grid) && manifest.grid >= 1 && manifest.grid <= 256;
        } else if (key == "speed") {
            ok = bool(in >> manifest.speed) && manifest.speed > 0.0 && manifest.speed <= 1000.0;
        } else if (key == "radii") {
            ok = bool(in >> manifest.load_radius >> manifest.activate_radius >> manifest.hysteresis) && manifest.activate_radius > 0.0 &&
                 manifest.load_radius >= manifest.activate_radius && manifest.hysteresis >= 0.0;
        } else if (key == "skinning") {
            ok = bool(in >> text) && (text == "on" || text == "off");
            manifest.skinning = text == "on";
        } else if (key == "debug_view") {
            const auto view = bool(in >> text) ? debug_view_named(text) : std::nullopt;
            ok = view.has_value();
            if (view) manifest.debug_view = *view;
        } else return fail(number_of_line, "unknown key '" + key + "'");
        if (!ok) return fail(number_of_line, "invalid value for '" + key + "'");
        if (in >> text) return fail(number_of_line, "unexpected '" + text + "' after '" + key + "'");
    }
    if (!header) return fail(number_of_line, "expected a maya-benchmark header");
    for (const auto* required : {"name", "workload"})
        if (std::ranges::find(seen, required) == seen.end()) return {{}, std::string("missing '") + required + "'"};
    if (manifest.workload == Workload::import || manifest.workload == Workload::animation) {
        for (const auto* required : {"content", "models"})
            if (std::ranges::find(seen, required) == seen.end())
                return {{}, std::string("the ") + workload_name(manifest.workload) + " workload needs '" + required + "'"};
        if (manifest.workload == Workload::animation && manifest.models.size() != 1) return {{}, "the animation workload plays one model"};
    } else if (manifest.workload != Workload::physics && manifest.workload != Workload::stream &&
               std::ranges::find(seen, "project") == seen.end()) {
        return {{}, "missing 'project'"};
    }
    if ((manifest.workload == Workload::load_cycles || manifest.workload == Workload::play_cycles) && manifest.slope_from > manifest.cycles)
        return {{}, "'slope_from' is past the last cycle"};
    if (generated(manifest.workload))
        for (const auto* required : {"mesh", "material"})
            if (std::ranges::find(seen, required) == seen.end())
                return {{}, std::string("the ") + workload_name(manifest.workload) + " workload needs '" + required + "'"};
    return {std::move(manifest), {}};
}

ManifestResult load_manifest(const fs::path& file) {
    auto input = std::ifstream(file);
    if (!input) return {{}, "cannot read " + file.string()};
    auto result = read_manifest(input, fs::absolute(file).parent_path());
    if (!result) result.error = file.string() + ": " + result.error;
    return result;
}

namespace {
/// The workload itself; run() records what surrounds it.
void run_workload(Result& result, const Manifest& manifest, GraphicsDevice& device, std::string renderer_shader,
                  const std::function<void()>& poll) {
    if (manifest.workload == Workload::import) { // no views: imports, and loads into the device
        result.unavailable = {
            {"rendering", "the import workload renders nothing"},
            {"cold_cache_load", "the OS file cache is not controlled; 'cold' means an empty cook cache"},
        };
        try {
            detail::run_import(result, manifest, device);
        } catch (const std::exception& error) {
            result.failure = error.what();
        }
        return;
    }
    if (manifest.workload == Workload::stream) { // headless: a generated world, no views
        result.unavailable = {{"rendering", "the stream workload renders nothing; it measures streaming and ticks"}};
        try {
            detail::run_stream(result, manifest);
        } catch (const std::exception& error) {
            result.failure = error.what();
        }
        return;
    }
    if (manifest.workload == Workload::physics) { // headless: no project, views, or device work
        result.unavailable = {
            {"contact_constraints", "Jolt does not report its contact constraint count; pairs and solid contacts are counted"},
            {"rendering", "the physics workload renders nothing"},
        };
        try {
            detail::run_physics(result, manifest);
        } catch (const std::exception& error) {
            result.failure = error.what();
        }
        return;
    }
    result.unavailable = {
        {"gpu_core_count", "Metal does not report it"},
        {"cold_cache_load", "the OS file cache is not controlled; the registry is evicted between load cycles"},
    };
    if (!device.gpu_timing_supported()) result.unavailable.push_back({"gpu_frame_time", "this device does not report GPU execution time"});
    if (!device.gpu_pass_timing_supported()) result.unavailable.push_back({"gpu_pass_time", device.gpu_pass_timing_unavailable()});
    if (!manifest.present)
        result.unavailable.push_back({"present_pacing", "the benchmark renders offscreen and never presents, so display pacing does not apply"});
    else if (!device.present_timing_supported())
        result.unavailable.push_back({"present_pacing", "this device does not report when frames are shown"});
    if (manifest.present) {
        result.refresh_hz = device.display_refresh_rate();
        if (!result.refresh_hz) result.unavailable.push_back({"missed_deadlines", "the display's refresh rate is unknown"});
    }
    auto temporary = fs::path{};
    try {
        // A1 imports its model into a new project first, which it then plays from like any other.
        auto project_path = manifest.project;
        auto animated_scene_path = fs::path{};
        if (manifest.workload == Workload::animation) {
            temporary = fs::temp_directory_path() / ("maya-benchmark-animation-" + std::to_string(::getpid()));
            animated_scene_path = detail::prepare_animation(manifest, temporary);
            project_path = temporary;
        }
        auto opened = open_project(project_path);
        if (!opened) throw std::runtime_error(opened.error);
        // A project's scene loads through its cook cache, as the player does; generated scenes cook nothing.
        auto cache = manifest.workload == Workload::scene ? std::make_shared<CookCache>(cook_cache_folder(opened.project)) : nullptr;
        auto assets = open_project_assets(opened.project, std::make_unique<FileAssetProvider>(device, cache));
        if (!assets) throw std::runtime_error(assets.error);
        auto& registry = *assets.registry;
        const auto context = asset_property_context(registry);

        auto document = SceneDocument{};
        auto camera = camera_id;
        if (manifest.workload == Workload::scene) {
            const auto path = manifest.scene.empty() ? opened.project.startup_scene : opened.project.resolve(manifest.scene);
            if (!path) throw std::runtime_error("the project has no scene to run; name one with 'scene'");
            auto clock = Stopwatch{};
            auto loaded = load_scene_file(*path, context);
            if (!loaded) throw std::runtime_error(loaded.diagnostics.front().message);
            result.load = SceneLoad{clock.milliseconds()};
            document = std::move(loaded.document);
        } else if (manifest.workload == Workload::animation) {
            auto loaded = load_scene_file(animated_scene_path, context);
            if (!loaded) throw std::runtime_error(loaded.diagnostics.front().message);
            auto duration = 0.0f;
            for (const auto& record : registry.records())
                if (record.kind == AssetKind::animation)
                    if (const auto clip = registry.acquire(AssetRef<AnimationAsset>{record.id})) duration = std::max(duration, clip.lease.value().duration);
            document = detail::animated_scene(manifest, loaded.document, duration);
            if (auto problems = validate_scene(document, context); !problems.empty())
                throw std::runtime_error("the animated scene is invalid: " + problems.front().message);
        } else {
            document = generate(manifest);
            if (auto problems = validate_scene(document, context); !problems.empty())
                throw std::runtime_error("the generated scene is invalid: " + problems.front().message);
        }

        auto renderer = Renderer(device, std::move(renderer_shader));
        auto target = RenderTarget(device, {Format::rgba8_unorm, false, "benchmark view"});
        if (auto error = target.resize(manifest.width, manifest.height)) throw std::runtime_error(error.message);
        if (manifest.present && device.surface_format() == Format::undefined)
            throw std::runtime_error("'present on' needs a window; this device has no surface");
        auto skins = SkinBindingCache{};
        const auto stage = Stage{device, registry, renderer, target, manifest.width, manifest.height, manifest.present, &poll, manifest.debug_view, skins};
        // The warmed empty session: the device, the project's catalog, the view target, and what the
        // renderer keeps (its texture placeholder), with no content.
        if (auto error = device.begin_frame()) throw std::runtime_error(error.message);
        auto warmed = renderer.render(RenderSnapshot{}, RenderView{manifest.width, manifest.height}, target);
        if (auto ended = device.end_frame(); !warmed) warmed = std::move(ended);
        if (warmed) throw std::runtime_error(warmed.message);
        const auto renderer_gpu = [&] { return renderer.shadow_bytes() + renderer.table_bytes() + target.gpu_bytes(); };
        result.baseline = memory(device, registry, 0, renderer_gpu());
        footprint_snapshot("baseline");
        if (manifest.workload == Workload::scene) {
            // Starting play, then loading what the scene draws through the cook cache (a loading screen),
            // and the first frame: first_frame_ms is both, as before loading was asynchronous.
            auto clock = Stopwatch{};
            auto probe = PlaySession::start(document, context, systems_for(manifest, registry));
            if (!probe) throw std::runtime_error(probe.error.empty() ? probe.diagnostics.front().message : probe.error);
            if (!probe.session->camera()) throw std::runtime_error("the scene has no camera");
            result.load->start_ms = clock.milliseconds();
            camera = *probe.session->camera();
            clock.restart();
            if (manifest.stream_load) {
                // Frames from the start, as the editor runs: until nothing the view draws is loading.
                auto& load = *result.load;
                load.streamed = true;
                const auto budget = std::chrono::duration<double, std::milli>(AssetLoadBudget{}.time).count();
                for (;;) {
                    const auto sample = run_frame(stage, *probe.session, camera, false);
                    const auto elapsed = clock.milliseconds();
                    if (load.frames++ == 0) load.first_frame_ms = elapsed;
                    load.longest_frame_ms = std::max(load.longest_frame_ms, sample.frame);
                    load.longest_finalize_ms = std::max(load.longest_finalize_ms, sample.finalize);
                    if (sample.finalize > budget) ++load.over_budget;
                    const auto stats = registry.load_stats();
                    if (!sample.loading && stats.in_flight == 0 && stats.prepared == 0) {
                        load.resident_ms = elapsed;
                        break;
                    }
                    if (elapsed > 600000.0) throw std::runtime_error("the scene was still loading after 10 minutes");
                    device.take_gpu_timings();
                }
            } else {
                preload_render_assets(probe.session->world(), registry);
                result.load->first_frame_ms = clock.milliseconds() + run_frame(stage, *probe.session, camera, false).frame;
            }
            device.take_gpu_timings();
        }

        if (manifest.workload == Workload::instances || manifest.workload == Workload::scene || manifest.workload == Workload::animation) {
            for (uint32_t i = 0; i < manifest.runs; ++i)
                result.runs.push_back(measure(manifest, stage, document, context, camera, true, result, i == 0));
            if (manifest.overhead) { // without CPU scopes or GPU pass timing
                const auto timed = device.gpu_pass_timing_enabled();
                device.set_gpu_pass_timing(false);
                result.uninstrumented = measure(manifest, stage, document, context, camera, false, result, false);
                device.set_gpu_pass_timing(timed);
            }
            return;
        }

        // Cycles: load (or start) the same scene again and again, and look at what remains after each.
        const auto authored = scene_text(document, context);
        if (manifest.workload == Workload::load_cycles) {
            temporary = fs::temp_directory_path() / ("maya-benchmark-" + std::to_string(::getpid()));
            fs::create_directories(temporary);
            const auto path = temporary / "generated.scene";
            if (auto problems = save_scene_file(path, document, context); !problems.empty())
                throw std::runtime_error(problems.front().message);
            // Scenes that must be refused, leaving nothing behind.
            const auto refuse = [&](std::string name, std::string text) {
                const auto bad = temporary / (name + ".scene");
                std::ofstream(bad) << text;
                auto loaded = load_scene_file(bad, context);
                auto rejected = RejectedCase{std::move(name), !loaded, loaded ? std::string{} : loaded.diagnostics.front().message};
                device.wait_idle();
                const auto after = memory(device, registry, 0);
                rejected.nothing_left = after.assets.leased == 0 && after.device.buffers == result.baseline.device.buffers;
                result.rejected.push_back(std::move(rejected));
            };
            refuse("malformed", authored.substr(0, authored.size() / 2) + "\ncomponent\n");
            auto missing = authored;
            auto mesh_words = std::ostringstream{};
            mesh_words << std::hex << "mesh " << manifest.mesh.high << ' ' << manifest.mesh.low;
            if (const auto at = missing.find(mesh_words.str()); at != std::string::npos)
                missing.replace(at, mesh_words.str().size(), "mesh 6d617961 ffffffff");
            refuse("missing_asset", missing);
        }
        auto authored_world = instantiate_scene(document, context); // must not change while playing
        if (!authored_world) throw std::runtime_error("the authored scene could not be built");
        const auto authored_before = scene_text(capture_scene(*authored_world.world), context);
        for (uint32_t cycle = 0; cycle < manifest.cycles; ++cycle) {
            auto sample = CycleSample{};
            auto clock = Stopwatch{};
            auto source = document;
            if (manifest.workload == Workload::load_cycles) {
                auto loaded = load_scene_file(temporary / "generated.scene", context);
                if (!loaded) throw std::runtime_error(loaded.diagnostics.front().message);
                source = std::move(loaded.document);
            }
            auto started = PlaySession::start(std::move(source), context, builtin_systems());
            if (!started) throw std::runtime_error(started.error.empty() ? started.diagnostics.front().message : started.error);
            sample.load = clock.milliseconds();
            clock.restart();
            preload_render_assets(started.session->world(), registry); // counted in the first frame, as before
            const auto preload = clock.milliseconds();
            for (uint32_t tick = 0; tick < manifest.ticks; ++tick) {
                const auto frame = run_frame(stage, *started.session, camera, false).frame + (tick == 0 ? preload : 0.0);
                if (tick == 0) sample.first_frame = frame;
                sample.longest_frame = std::max(sample.longest_frame, frame);
            }
            clock.restart();
            started.session.reset();
            sample.stop = clock.milliseconds();
            device.wait_idle(); // retire this cycle's GPU work
            registry.evict_unused(); // explicit maintenance: unused versions leave the cache
            sample.after = memory(device, registry, 0, renderer_gpu());
            if (cycle == 0 || cycle == 9 || cycle == 99 || cycle + 1 == manifest.cycles) footprint_snapshot("cycle-" + std::to_string(cycle + 1));
            device.take_gpu_timings();
            result.cycles.push_back(sample);
        }
        result.authored_unchanged = scene_text(capture_scene(*authored_world.world), context) == authored_before;
        result.resident = memory(device, registry, authored_world.world->size(), renderer_gpu());
        result.loading = registry.load_stats();
    } catch (const std::exception& error) {
        result.failure = error.what();
    }
    if (!temporary.empty()) {
        auto error = std::error_code{};
        fs::remove_all(temporary, error);
    }
    device.wait_idle();
}
} // namespace

Result run(const Manifest& manifest, GraphicsDevice& device, std::string renderer_shader, const std::function<void()>& poll) {
    auto result = Result{};
    result.manifest = manifest;
    result.system = system_info();
    result.build = build_info();
    job_system().reset_peaks();
    result.jobs.start = job_system().stats();
    const auto watch = Stopwatch{};
    run_workload(result, manifest, device, std::move(renderer_shader), poll);
    result.jobs.end = job_system().stats();
    result.jobs.seconds = watch.milliseconds() / 1000.0;
    for (const auto& samples : result.runs) result.jobs.frames += samples.frame.size();
    if (result.uninstrumented) result.jobs.frames += result.uninstrumented->frame.size();
    for (const auto& physics : result.physics_runs) result.jobs.frames += physics.tick.size();
    result.thermal_state_at_end = system_info().thermal_state; // throttling during a run shows here
    result.thread_qos = thread_qos();
    return result;
}

// What the job system did during the benchmark: counts and busy time by tier, per measured frame where
// there were frames, and the completion queues' work (docs/jobs.md#instruments).
void write_jobs(Json& json, const JobsRecord& jobs) {
    json.key("jobs");
    json.open('{');
    json.field("seconds", jobs.seconds);
    json.field("frames", jobs.frames);
    const auto tier = [&](std::string_view name, const JobTierStats& start, const JobTierStats& end) {
        json.key(name);
        json.open('{');
        json.field("workers", end.workers);
        json.field("submitted", end.submitted - start.submitted);
        json.field("succeeded", end.succeeded - start.succeeded);
        json.field("failed", end.failed - start.failed);
        json.field("cancelled", end.cancelled - start.cancelled);
        json.field("busy_ms", end.busy_ms - start.busy_ms);
        json.field("queued_peak", end.queued_peak);
        json.key("jobs_per_frame");
        if (jobs.frames) json.value(double(end.submitted - start.submitted) / double(jobs.frames)); else json.null();
        json.key("busy_ms_per_frame");
        if (jobs.frames) json.value((end.busy_ms - start.busy_ms) / double(jobs.frames)); else json.null();
        json.close('}');
    };
    tier("frame", jobs.start.frame, jobs.end.frame);
    tier("background", jobs.start.background, jobs.end.background);
    json.key("completions");
    json.open('{');
    json.field("applied", jobs.end.completions_applied - jobs.start.completions_applied);
    json.field("discarded", jobs.end.completions_discarded - jobs.start.completions_discarded);
    json.field("drain_ms", jobs.end.drain_ms - jobs.start.drain_ms);
    json.close('}');
    json.close('}');
}

std::string to_json(const Result& r) {
    auto json = Json{};
    json.open('{');
    json.field("format", "maya-benchmark-result");
    json.field("version", 1);
    json.field("succeeded", r.failure.empty());
    json.key("failure");
    if (r.failure.empty()) json.null(); else json.value(r.failure);
    const auto& m = r.manifest;
    json.key("manifest");
    json.open('{');
    json.field("name", m.name);
    json.field("workload", workload_name(m.workload));
    json.field("project", m.project.string());
    json.field("scene", m.scene.generic_string());
    json.field("count", m.count);
    json.field("rotating", m.rotating);
    json.field("spacing", double(m.spacing));
    json.field("seed", m.seed);
    json.key("camera");
    json.open('[');
    for (const auto v : {m.camera_position.x, m.camera_position.y, m.camera_position.z, m.camera_target.x, m.camera_target.y, m.camera_target.z})
        json.value(double(v));
    json.close(']');
    json.field("warmup", m.warmup);
    json.field("samples", m.samples);
    json.field("runs", m.runs);
    json.field("cycles", m.cycles);
    json.field("ticks", m.ticks);
    json.field("present", m.present);
    json.field("stream_load", m.stream_load);
    json.field("debug_view", debug_view_name(m.debug_view));
    if (m.workload == Workload::animation) {
        json.field("model", (m.content / m.models.front()).string());
        json.field("skinning", m.skinning);
    }
    json.close('}');
    json.key("environment");
    json.open('{');
    json.field("model", r.system.model);
    json.field("cpu", r.system.cpu);
    json.field("physical_cores", r.system.physical_cores);
    json.field("logical_cores", r.system.logical_cores);
    json.field("performance_cores", r.system.performance_cores);
    json.field("efficiency_cores", r.system.efficiency_cores);
    json.field("memory_bytes", r.system.memory_bytes);
    json.field("os", r.system.os);
    json.field("gpu", r.system.gpu);
    json.field("unified_memory", r.system.unified_memory);
    json.field("thermal_state", r.system.thermal_state);
    json.field("thermal_state_at_end", r.thermal_state_at_end);
    json.field("thread_qos", r.thread_qos);
    json.field("low_power_mode", r.system.low_power_mode);
    json.close('}');
    json.key("build");
    json.open('{');
    json.field("revision", r.build.revision);
    json.field("build_type", r.build.build_type);
    json.field("sanitizers", r.build.sanitizers);
    json.field("compiler", r.build.compiler);
    json.close('}');
    json.key("quality");
    json.open('{');
    json.field("width", m.width);
    json.field("height", m.height);
    json.field("color_format", "rgba8_unorm, sRGB-encoded");
    json.field("scene_format", "rgba16_float");
    json.field("exposure", "the camera's, in EV100 (0 for generated scenes)");
    json.field("tone_mapping", "the camera's (AgX for generated scenes)");
    json.field("depth_format", "depth32_float");
    json.field("antialiasing", "none");
    json.field("textures", "none (materials are factors)");
    json.field("lighting", "the scene's: up to 4 directional lights and 16 point and spot lights per view");
    json.field("shadows", m.workload == Workload::scene || m.workload == Workload::animation
        ? "the scene's: 4 cascades of 2048 texels for the first shadowed directional light, 1024 texels for each of up to 4 spot lights"
        : "none (the generated sun casts none)");
    json.field("presentation", m.present ? "presented to a window every frame, synchronized with the display" : "offscreen, never presented");
    json.field("simulation", "fixed 60 Hz, one tick per frame");
    json.field("upload_mib_per_frame", m.upload_mib);
    json.close('}');
    json.key("counters");
    json.open('{');
    const auto& c = r.counters;
    json.field("entities", c.entities);
    json.field("mesh_renderers", c.mesh_renderers);
    json.field("skipped", c.skipped);
    json.field("pending", c.pending);
    json.field("pending_textures", c.pending_textures);
    json.field("spinning", c.spinning);
    json.field("centers_in_view", c.centers_in_view);
    json.field("draws_per_frame", c.draws);
    json.field("instances_per_frame", c.instances);
    json.field("triangles_per_frame", c.triangles);
    json.field("passes_per_frame", c.passes);
    json.field("unique_meshes", c.unique_meshes);
    json.field("unique_materials", c.unique_materials);
    json.field("local_lights", c.local_lights);
    json.field("dropped_lights", c.dropped_lights);
    json.field("unshadowed_lights", c.unshadowed_lights);
    json.field("shadow_maps_per_view", c.shadow_maps);
    json.field("shadow_draws_per_view", c.shadow_draws);
    json.field("animated", c.animated);
    json.field("skinned_instances", c.skinned);
    json.field("joints_per_view", c.joints);
    json.close('}');
    write_jobs(json, r.jobs);
    // The registry's loading (docs/assets.md#asynchronous-loading): waited_in_frames must be 0.
    json.key("asset_loading");
    if (r.loading) {
        const auto& l = *r.loading;
        json.open('{');
        json.field("started", l.started);
        json.field("merged", l.merged);
        json.field("finalized", l.finalized);
        json.field("failed", l.failed);
        json.field("cancelled", l.cancelled);
        json.field("discarded", l.discarded);
        json.field("waited", l.waited);
        json.field("waited_in_frames", l.waited_in_frames);
        json.field("bytes_finalized", l.bytes_finalized);
        json.field("longest_update_ms", l.longest_ms);
        json.field("latency_p50_ms", l.latency_p50_ms);
        json.field("latency_p95_ms", l.latency_p95_ms);
        json.field("latency_max_ms", l.latency_max_ms);
        json.close('}');
    } else {
        json.null();
    }
    json.key("load");
    if (r.load) {
        json.open('{');
        json.field("scene_ms", r.load->scene_ms);
        json.field("start_ms", r.load->start_ms);
        json.field("first_frame_ms", r.load->first_frame_ms);
        if (r.load->streamed) {
            json.field("frames", r.load->frames);
            json.field("resident_ms", r.load->resident_ms);
            json.field("longest_frame_ms", r.load->longest_frame_ms);
            json.field("longest_finalize_ms", r.load->longest_finalize_ms);
            json.field("finalize_over_budget", r.load->over_budget);
        }
        json.close('}');
    } else {
        json.null();
    }
    write_memory(json, "baseline_memory", r.baseline);
    write_memory(json, "resident_memory", r.resident);
    json.key("runs");
    json.open('[');
    for (const auto& run : r.runs) write_run(json, run, r.refresh_hz);
    json.close(']');
    json.key("uninstrumented");
    if (r.uninstrumented) write_run(json, *r.uninstrumented, r.refresh_hz); else json.null();
    json.key("overhead");
    if (r.uninstrumented && !r.runs.empty()) {
        // The uninstrumented run has no CPU scopes and no GPU pass timing.
        auto instrumented = std::vector<double>{}, instrumented_gpu = std::vector<double>{}, uninstrumented_gpu = std::vector<double>{};
        for (const auto& run : r.runs) {
            instrumented.insert(instrumented.end(), run.frame.begin(), run.frame.end());
            for (const auto& value : run.gpu) if (value) instrumented_gpu.push_back(*value);
        }
        for (const auto& value : r.uninstrumented->gpu) if (value) uninstrumented_gpu.push_back(*value);
        const auto with = summarize(instrumented).mean, without = summarize(r.uninstrumented->frame).mean;
        const auto gpu_with = summarize(instrumented_gpu).mean, gpu_without = summarize(uninstrumented_gpu).mean;
        json.open('{');
        json.field("instrumented_mean_ms", with);
        json.field("uninstrumented_mean_ms", without);
        json.field("difference_ms", with - without);
        json.field("difference_percent", without > 0.0 ? (with - without) / without * 100.0 : 0.0);
        json.field("gpu_instrumented_mean_ms", gpu_with);
        json.field("gpu_uninstrumented_mean_ms", gpu_without);
        json.field("gpu_difference_percent", gpu_without > 0.0 ? (gpu_with - gpu_without) / gpu_without * 100.0 : 0.0);
        json.close('}');
    } else {
        json.null();
    }
    if (!r.cycles.empty()) {
        json.key("cycles");
        json.open('{');
        const auto warm = std::min<size_t>(10, r.cycles.size() / 2); // cycles 1-10 warm up
        // The footprint is fitted from slope_from, once the allocators have warmed up too.
        const auto fit_from = std::min<size_t>(m.slope_from, r.cycles.size()) - 1;
        auto load = std::vector<double>{}, first = std::vector<double>{}, longest = std::vector<double>{}, stop = std::vector<double>{};
        auto footprint = std::vector<double>{};
        for (size_t i = warm; i < r.cycles.size(); ++i) {
            load.push_back(r.cycles[i].load);
            first.push_back(r.cycles[i].first_frame);
            longest.push_back(r.cycles[i].longest_frame);
            stop.push_back(r.cycles[i].stop);
        }
        for (size_t i = fit_from; i < r.cycles.size(); ++i)
            if (r.cycles[i].after.process) footprint.push_back(double(r.cycles[i].after.process->footprint));
        json.field("warmup_cycles", warm);
        json.key("footprint_slope_cycles");
        json.open('[');
        json.value(fit_from + 1);
        json.value(r.cycles.size());
        json.close(']');
        write_summary(json, "load_ms", load);
        write_summary(json, "first_frame_ms", first);
        write_summary(json, "longest_frame_ms", longest);
        write_summary(json, "stop_ms", stop);
        json.field("footprint_slope_bytes_per_cycle", slope(footprint));
        const auto& last = r.cycles.back().after;
        json.key("returned_to_baseline");
        json.open('{');
        json.field("buffers", last.device.buffers == r.baseline.device.buffers);
        json.field("textures", last.device.textures == r.baseline.device.textures);
        json.field("pending_retirements", last.device.pending_retirements == 0);
        json.field("leased_assets", last.assets.leased == 0);
        json.field("resident_meshes", last.assets.meshes == r.baseline.assets.meshes);
        json.close('}');
        json.key("each");
        json.open('[');
        for (const auto& cycle : r.cycles) {
            json.open('{');
            json.field("load_ms", cycle.load);
            json.field("first_frame_ms", cycle.first_frame);
            json.field("longest_frame_ms", cycle.longest_frame);
            json.field("stop_ms", cycle.stop);
            json.field("buffers", cycle.after.device.buffers);
            json.field("buffer_bytes", cycle.after.device.buffer_bytes);
            json.field("pending_retirements", cycle.after.device.pending_retirements);
            json.field("resident_meshes", cycle.after.assets.meshes);
            json.key("footprint_bytes");
            if (cycle.after.process) json.value(cycle.after.process->footprint); else json.null();
            json.close('}');
        }
        json.close(']');
        json.close('}');
    }
    json.key("rejected");
    json.open('[');
    for (const auto& rejected : r.rejected) {
        json.open('{');
        json.field("case", rejected.name);
        json.field("rejected", rejected.rejected);
        json.field("reason", rejected.reason);
        json.field("nothing_left", rejected.nothing_left);
        json.close('}');
    }
    json.close(']');
    json.key("authored_unchanged");
    if (r.authored_unchanged) json.value(*r.authored_unchanged); else json.null();
    if (m.workload == Workload::physics) write_physics(json, r);
    if (m.workload == Workload::stream) {
        json.key("streaming");
        json.open('[');
        for (const auto& run : r.stream_runs) {
            json.open('{');
            json.field("failure", run.failure);
            json.field("cells", run.cells);
            json.field("entities", run.entities);
            json.field("grid", m.grid);
            json.field("speed_m_s", m.speed);
            json.field("load_radius_m", m.load_radius);
            json.field("activate_radius_m", m.activate_radius);
            json.field("hysteresis_m", m.hysteresis);
            write_summary(json, "frame_ms", run.frame);
            write_summary(json, "streaming_ms", run.streaming);
            json.field("loads", run.loads);
            json.field("loads_cancelled", run.cancelled);
            json.field("loads_failed", run.failed);
            json.field("completions_discarded", run.discarded);
            json.field("activations", run.activations);
            json.field("deactivations", run.deactivations);
            json.key("crossings");
            json.open('[');
            for (const auto& crossing : run.crossings) {
                json.open('{');
                json.field("ms", crossing.ms);
                json.field("longest_streaming_ms", crossing.longest_streaming_ms);
                json.key("footprint_bytes");
                if (crossing.footprint) json.value(*crossing.footprint);
                else json.null();
                json.field("active_entities", crossing.active_entities);
                json.field("bytes_loaded", crossing.bytes_loaded);
                json.close('}');
            }
            json.close(']');
            json.key("samples");
            json.open('{');
            json.key("frame_ms");
            json.open('[');
            for (const auto value : run.frame) json.value(value);
            json.close(']');
            json.key("streaming_ms");
            json.open('[');
            for (const auto value : run.streaming) json.value(value);
            json.close(']');
            json.close('}');
            json.close('}');
        }
        json.close(']');
    }
    if (m.workload == Workload::import) {
        json.key("imports");
        json.open('[');
        for (const auto& sample : r.imports) {
            json.open('{');
            json.field("model", sample.model);
            json.field("import_ms", sample.import_ms);
            json.field("cold_load_ms", sample.cold_ms);
            json.field("warm_load_ms", sample.warm_ms);
            json.field("meshes", sample.meshes);
            json.field("textures", sample.textures);
            json.field("materials", sample.materials);
            json.field("entities", sample.entities);
            json.field("triangles", sample.triangles);
            json.field("texture_gpu_bytes", sample.texture_gpu_bytes);
            json.field("cache_bytes", sample.cache_bytes);
            json.field("warm_hits", sample.warm_hits);
            json.field("warm_misses", sample.warm_misses);
            json.field("cache_digest", sample.cache_digest);
            json.close('}');
        }
        json.close(']');
        json.key("deterministic");
        if (r.deterministic) json.value(*r.deterministic); else json.null();
    }
    json.key("unavailable");
    json.open('{');
    for (const auto& [metric, reason] : r.unavailable) json.field(metric, reason);
    json.close('}');
    json.close('}');
    return json.text + "\n";
}

Pacing pacing(const RunSamples& run, std::optional<double> refresh_hz) {
    auto result = Pacing{};
    auto previous = std::optional<double>{};
    for (size_t i = 0; i < run.presented.size(); ++i) {
        const auto& shown = run.presented[i];
        if (i < run.present_reported.size() && !run.present_reported[i]) {
            ++result.unreported;
            continue;
        }
        if (!shown) {
            ++result.not_shown;
            continue;
        }
        ++result.shown;
        if (previous) {
            const auto interval = (*shown - *previous) * 1000.0;
            result.intervals.push_back(interval);
            // A frame shown later than 1.5 refresh periods after the one before missed its deadline.
            if (refresh_hz && interval > 1.5 * 1000.0 / *refresh_hz) ++result.missed;
        }
        previous = shown;
    }
    return result;
}

std::string to_text(const Result& r) {
    auto out = std::ostringstream{};
    out << std::fixed << std::setprecision(3);
    out << r.manifest.name << " (" << workload_name(r.manifest.workload) << ") on " << r.system.cpu << ", " << r.system.os
        << ", build " << r.build.revision << " " << r.build.build_type << "\n";
    if (!r.failure.empty()) out << "  FAILED: " << r.failure << "\n";
    if (r.system.thermal_state != "nominal" || r.thermal_state_at_end != "nominal")
        out << "  WARNING: thermal state " << r.system.thermal_state << " at the start and " << r.thermal_state_at_end
            << " at the end; results are not comparable with a cool machine\n";
    for (size_t i = 0; i < r.runs.size(); ++i) {
        const auto frame = summarize(r.runs[i].frame);
        auto gpu = std::vector<double>{};
        for (const auto& value : r.runs[i].gpu) if (value) gpu.push_back(*value);
        const auto g = summarize(gpu);
        out << "  run " << i + 1 << (r.manifest.present ? " (presenting)" : "") << ": frame " << frame.mean << " ms (P95 " << frame.p95
            << ", P99 " << frame.p99 << ")";
        if (g.count) out << ", GPU " << g.mean << " ms (P95 " << g.p95 << ")";
        else out << ", GPU unavailable";
        out << ", " << (r.runs[i].sampled_seconds > 0 ? double(r.runs[i].frame.size()) / r.runs[i].sampled_seconds : 0.0) << " fps\n";
        if (!r.runs[i].gpu_passes.empty()) {
            out << "    GPU passes:";
            for (const auto& [label, series] : r.runs[i].gpu_passes) {
                auto values = std::vector<double>{};
                for (const auto& value : series) if (value) values.push_back(*value);
                out << " " << label << " " << summarize(values).mean << " ms;";
            }
            if (r.runs[i].gpu_pass_mismatches) out << " " << r.runs[i].gpu_pass_mismatches << " OUTSIDE THEIR FRAME;";
            out << "\n";
        }
        if (r.manifest.workload == Workload::animation && !r.runs[i].systems.empty()) {
            out << "    per tick:";
            for (const auto& [name, values] : r.runs[i].systems) {
                const auto summary = summarize(values);
                out << " " << name << " " << summary.mean << " ms (P99 " << summary.p99 << ");";
            }
            out << "\n";
        }
        if (!r.runs[i].presented.empty()) {
            const auto paced = pacing(r.runs[i], r.refresh_hz);
            const auto interval = summarize(paced.intervals);
            out << "    presented " << paced.shown << " of " << r.runs[i].presented.size() << "; interval " << interval.mean << " ms (P95 "
                << interval.p95 << ", P99 " << interval.p99 << ")";
            if (r.refresh_hz) out << "; " << paced.missed << " missed deadlines at " << *r.refresh_hz << " Hz";
            out << "\n";
            if (paced.not_shown > 0 || paced.unreported > 0)
                out << "    WARNING: " << paced.not_shown << " frames were never shown and " << paced.unreported
                    << " not reported; a hidden window or a sleeping display shows nothing, so this run's pacing is not valid\n";
        }
    }
    if (r.uninstrumented) out << "  without instrumentation: frame " << summarize(r.uninstrumented->frame).mean << " ms\n";
    if (!r.runs.empty())
        out << "  " << r.counters.draws << " draws, " << r.counters.triangles << " triangles, " << r.counters.unique_meshes
            << " unique meshes, " << r.counters.centers_in_view << " of " << r.counters.mesh_renderers << " drawn in view\n";
    if (!r.runs.empty() && (r.counters.local_lights > 0 || r.counters.shadow_maps > 0))
        out << "  " << r.counters.local_lights << " point and spot lights drawn, " << r.counters.dropped_lights << " left out, "
            << r.counters.unshadowed_lights << " without shadows; " << r.counters.shadow_maps << " shadow maps and "
            << r.counters.shadow_draws << " shadow draws per view\n";
    if (r.load && r.load->streamed)
        out << "  load: scene " << r.load->scene_ms << " ms, start " << r.load->start_ms << " ms; streamed: first frame " << r.load->first_frame_ms
            << " ms, resident after " << r.load->resident_ms << " ms and " << r.load->frames << " frames, longest frame "
            << r.load->longest_frame_ms << " ms, longest finalize " << r.load->longest_finalize_ms << " ms (" << r.load->over_budget
            << " over budget)\n";
    else if (r.load)
        out << "  load: scene " << r.load->scene_ms << " ms, start " << r.load->start_ms << " ms, first frame " << r.load->first_frame_ms
            << " ms (through the project's cook cache: cooking first when it is empty)\n";
    if (!r.runs.empty() && r.manifest.workload == Workload::animation)
        out << "  " << r.counters.animated << " playing clips, " << r.counters.skinned << " drawn skinned, " << r.counters.joints
            << " joints per view\n";
    if (!r.cycles.empty()) {
        auto load = std::vector<double>{};
        for (const auto& cycle : r.cycles) load.push_back(cycle.load);
        const auto& last = r.cycles.back().after;
        out << "  " << r.cycles.size() << " cycles: load " << summarize(load).mean << " ms mean; after the last, " << last.device.buffers
            << " buffers (baseline " << r.baseline.device.buffers << "), " << last.assets.meshes << " resident meshes\n";
    }
    for (const auto& rejected : r.rejected)
        out << "  " << rejected.name << ": " << (rejected.rejected ? "refused" : "NOT REFUSED") << (rejected.nothing_left ? "" : ", LEFT RESOURCES") << "\n";
    if (r.authored_unchanged) out << "  authored scene " << (*r.authored_unchanged ? "unchanged" : "CHANGED") << "\n";
    if (r.manifest.workload == Workload::import) {
        // Per model: the median of the runs.
        auto models = std::vector<std::string>{};
        for (const auto& sample : r.imports)
            if (std::ranges::find(models, sample.model) == models.end()) models.push_back(sample.model);
        for (const auto& model : models) {
            auto import = std::vector<double>{}, cold = std::vector<double>{}, warm = std::vector<double>{};
            const ImportSample* last = nullptr;
            for (const auto& sample : r.imports)
                if (sample.model == model) {
                    import.push_back(sample.import_ms);
                    cold.push_back(sample.cold_ms);
                    warm.push_back(sample.warm_ms);
                    last = &sample;
                }
            out << "  " << model << ": import " << summarize(import).p50 << " ms, cold load " << summarize(cold).p50 << " ms, warm load "
                << summarize(warm).p50 << " ms (median of " << import.size() << "); " << last->meshes << " meshes, " << last->textures
                << " textures, " << last->materials << " materials, " << last->triangles << " triangles; cache "
                << double(last->cache_bytes) / (1024.0 * 1024.0) << " MiB, warm hits " << last->warm_hits << " of "
                << last->warm_hits + last->warm_misses << "\n";
        }
        if (r.deterministic) out << "  " << (*r.deterministic ? "every run cooked the same bytes" : "RUNS DIFFER: a determinism failure") << "\n";
    }
    if (r.manifest.workload == Workload::stream)
        for (const auto& run : r.stream_runs) {
            if (!run.failure.empty()) {
                out << "  FAILED: " << run.failure << '\n';
                continue;
            }
            const auto frame = summarize(run.frame), streaming = summarize(run.streaming);
            out << "  " << run.cells << " cells, " << run.entities << " entities; " << run.crossings.size() << " crossings: frame "
                << frame.mean << " ms (P99 " << frame.p99 << ", max " << frame.max << "), streaming " << streaming.mean << " ms (P99 "
                << streaming.p99 << ", max " << streaming.max << "); " << run.activations << " activations, " << run.deactivations
                << " deactivations, " << run.loads << " loads (" << run.cancelled << " cancelled, " << run.failed << " failed)\n";
            for (size_t i = 0; i < run.crossings.size(); ++i) {
                const auto& crossing = run.crossings[i];
                out << "    crossing " << i + 1 << ": " << crossing.ms << " ms, longest streaming " << crossing.longest_streaming_ms << " ms, "
                    << crossing.active_entities << " active entities, " << crossing.bytes_loaded / 1024 << " KiB loaded";
                if (crossing.footprint) out << ", footprint " << *crossing.footprint / (1024 * 1024) << " MiB";
                out << '\n';
            }
        }
    if (r.manifest.workload == Workload::physics) {
        const auto& s = r.physics_scene;
        out << "  " << s.bodies << " bodies (" << s.dynamic_bodies << " dynamic: " << s.active_set << " dropped, " << s.sleeping_set
            << " resting; " << s.scripted << " scripted), " << s.obstacles << " obstacles, " << s.sensors << " sensors\n";
        for (const auto& run : r.physics_runs) {
            out << "  workers " << run.worker_threads << ": ";
            if (!run.failure.empty()) {
                out << "FAILED: " << run.failure << "\n";
                continue;
            }
            const auto t = summarize(run.tick);
            out << "tick " << t.mean << " ms (P95 " << t.p95 << ", P99 " << t.p99 << ", max " << t.max << "); step "
                << summarize(run.step).mean << ", scripts " << summarize(run.scripts).mean << ", queries " << summarize(run.queries).mean
                << "; " << summarize(run.active).mean << " active, " << summarize(run.contacts).mean << " contacts; state "
                << std::hex << run.state << std::dec << "\n";
        }
        if (r.deterministic) out << "  " << (*r.deterministic ? "every run ended in the same state" : "RUNS DIFFER: a determinism failure") << "\n";
    }
    return out.str();
}

} // namespace maya::benchmark
