#include "benchmark.hpp"
#include "benchmark_detail.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/world/spatial.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <pthread.h>
#include <unistd.h>

namespace maya::benchmark {
namespace fs = std::filesystem;
namespace {

constexpr auto camera_id = EntityId{0x62656e63, 1}; // "benc"
constexpr auto light_id = EntityId{0x62656e63, 2};
constexpr uint64_t first_instance = 0x100;

const char* workload_name(Workload workload) {
    switch (workload) {
    case Workload::instances: return "instances";
    case Workload::scene: return "scene";
    case Workload::load_cycles: return "load_cycles";
    case Workload::play_cycles: return "play_cycles";
    case Workload::physics: return "physics";
    }
    return "?";
}
/// Workloads that generate a scene of one mesh and material.
bool generated(Workload workload) { return workload != Workload::scene && workload != Workload::physics; }

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

math::Quat looking(const math::Vec3& from, const math::Vec3& to) {
    auto forward = to - from;
    const auto length = forward.length();
    forward = length > 1e-6f ? forward * (1.0f / length) : math::Vec3{0.0f, 0.0f, -1.0f};
    const auto yaw = std::atan2(-forward.x, -forward.z);
    const auto pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    auto rotation = math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) * math::Quat::from_axis_angle({1.0f, 0.0f, 0.0f}, pitch);
    rotation.normalize();
    return rotation;
}

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
    document.entities.push_back({light_id, std::nullopt, {NameComponent{"Sun"}, sun, LightComponent{}}});

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
};

struct FrameSample {
    double frame = 0.0, simulation = 0.0, wait = 0.0, extract = 0.0, encode = 0.0, submit = 0.0;
};

/// One frame of a fixed-workload run: exactly one simulation tick, then extraction, encoding, and
/// submission of an offscreen view. Never presented.
FrameSample run_frame(const Stage& stage, PlaySession& session, EntityId camera, bool instrumented) {
    auto sample = FrameSample{};
    auto frame = Stopwatch{};
    auto part = Stopwatch{};
    const auto lap = [&](double& into) {
        if (!instrumented) return;
        into = part.milliseconds();
        part.restart();
    };
    if (const auto played = session.update(session.clock().interval()); !played.error.empty())
        throw std::runtime_error(played.error);
    lap(sample.simulation);
    if (auto error = stage.device.begin_frame()) throw std::runtime_error(error.message);
    lap(sample.wait);
    const auto& world = session.world();
    const auto handle = world.find(camera);
    if (!handle) throw std::runtime_error("The camera entity is missing");
    const auto view = extract_render_view(world, *handle, stage.width, stage.height);
    if (!view) throw std::runtime_error("The camera has no valid view");
    const auto snapshot = extract_render_snapshot(world, stage.registry);
    if (!snapshot.diagnostics.empty()) throw std::runtime_error(snapshot.diagnostics.front().message);
    lap(sample.extract);
    if (auto error = stage.renderer.render(snapshot, *view, stage.target)) throw std::runtime_error(error.message);
    lap(sample.encode);
    if (auto error = stage.device.end_frame()) throw std::runtime_error(error.message);
    lap(sample.submit);
    sample.frame = frame.milliseconds();
    return sample;
}

MemorySample memory(GraphicsDevice& device, const AssetRegistry& registry, size_t entities) {
    return {device.stats(), device.reported_memory(), process_memory(), registry.residency(), entities};
}

/// A run: a fresh play session from the document, warmup frames, then sampled frames, with GPU
/// timings matched to sampled frames by submission serial.
RunSamples measure(const Manifest& manifest, const Stage& stage, const SceneDocument& document,
                   const PropertyValidationContext& context, EntityId camera, bool instrumented, Result& result,
                   bool record_resident) {
    auto started = PlaySession::start(document, context, builtin_systems());
    if (!started) throw std::runtime_error(started.error.empty() ? started.diagnostics.front().message : started.error);
    auto& session = *started.session;
    for (uint32_t i = 0; i < manifest.warmup; ++i) run_frame(stage, session, camera, instrumented);
    auto samples = RunSamples{};
    samples.instrumented = instrumented;
    samples.gpu.assign(manifest.samples, std::nullopt);
    const auto first_serial = stage.device.stats().submitted_frames + 1;
    const auto collect = [&] { // warmup frames' timings fall outside the sampled serials
        for (const auto& timing : stage.device.take_gpu_timings())
            if (timing.frame >= first_serial && timing.frame < first_serial + manifest.samples)
                samples.gpu[timing.frame - first_serial] = timing.milliseconds;
    };
    for (uint32_t i = 0; i < manifest.samples; ++i) {
        const auto frame = run_frame(stage, session, camera, instrumented);
        samples.frame.push_back(frame.frame);
        samples.sampled_seconds += frame.frame / 1000.0;
        if (instrumented) {
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
        counters.unique_meshes = result.resident.assets.meshes;
        counters.unique_materials = result.resident.assets.materials;
    }
    return samples;
}

/// Instances whose origin projects inside the view: what a frustum test on origins would keep.
size_t centers_in_view(const SceneDocument& document, const Manifest& manifest, EntityId camera) {
    auto built = instantiate_scene(document, {[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }});
    if (!built) return 0;
    const auto handle = built.world->find(camera);
    const auto view = handle ? extract_render_view(*built.world, *handle, manifest.width, manifest.height) : std::nullopt;
    if (!view) return 0;
    auto inside = size_t{0};
    built.world->for_each<TransformComponent, MeshRendererComponent>(
        [&](EntityHandle entity, const TransformComponent&, const MeshRendererComponent&) {
            const auto matrix = built.world->world_matrix(entity);
            if (!matrix) return;
            const auto clip = view->matrices.view_projection * math::Vec4(matrix->elements[12], matrix->elements[13], matrix->elements[14], 1.0f);
            if (clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w && clip.z >= 0.0f && clip.z <= clip.w) ++inside;
        });
    return inside;
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
    json.close('}');
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

void write_run(Json& json, const RunSamples& run) {
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
        json.close('}');
    }
    auto gpu = std::vector<double>{};
    for (const auto& value : run.gpu) if (value) gpu.push_back(*value);
    write_summary(json, "gpu_ms", gpu);
    json.field("gpu_samples_missing", run.gpu.size() - gpu.size());
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
            else return fail(number_of_line, "unknown workload '" + text + "'; use instances, scene, load_cycles, play_cycles, or physics");
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
        else if (key == "overhead") {
            ok = bool(in >> text) && (text == "on" || text == "off");
            manifest.overhead = text == "on";
        } else return fail(number_of_line, "unknown key '" + key + "'");
        if (!ok) return fail(number_of_line, "invalid value for '" + key + "'");
        if (in >> text) return fail(number_of_line, "unexpected '" + text + "' after '" + key + "'");
    }
    if (!header) return fail(number_of_line, "expected a maya-benchmark header");
    for (const auto* required : {"name", "workload"})
        if (std::ranges::find(seen, required) == seen.end()) return {{}, std::string("missing '") + required + "'"};
    if (manifest.workload != Workload::physics && std::ranges::find(seen, "project") == seen.end()) return {{}, "missing 'project'"};
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
void run_workload(Result& result, const Manifest& manifest, GraphicsDevice& device, std::string renderer_shader) {
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
        {"gpu_pass_time", "per-pass GPU timing is not implemented; each frame's GPU execution time is measured"},
        {"present_pacing", "the benchmark renders offscreen and never presents, so display pacing does not apply"},
        {"culling", "the renderer submits every instance; counters.centers_in_view counts what an origin frustum test keeps"},
        {"gpu_core_count", "Metal does not report it"},
        {"cold_cache_load", "the OS file cache is not controlled; the registry is evicted between load cycles"},
    };
    if (!device.gpu_timing_supported()) result.unavailable.push_back({"gpu_frame_time", "this device does not report GPU execution time"});
    auto temporary = fs::path{};
    try {
        auto opened = open_project(manifest.project);
        if (!opened) throw std::runtime_error(opened.error);
        auto assets = open_project_assets(opened.project, std::make_unique<FileAssetProvider>(device));
        if (!assets) throw std::runtime_error(assets.error);
        auto& registry = *assets.registry;
        const auto context = asset_property_context(registry);

        auto document = SceneDocument{};
        auto camera = camera_id;
        if (manifest.workload == Workload::scene) {
            const auto path = manifest.scene.empty() ? opened.project.startup_scene : opened.project.resolve(manifest.scene);
            if (!path) throw std::runtime_error("the project has no scene to run; name one with 'scene'");
            auto loaded = load_scene_file(*path, context);
            if (!loaded) throw std::runtime_error(loaded.diagnostics.front().message);
            document = std::move(loaded.document);
            auto probe = PlaySession::start(document, context, {});
            if (!probe || !probe.session->camera()) throw std::runtime_error("the scene has no camera");
            camera = *probe.session->camera();
        } else {
            document = generate(manifest);
            if (auto problems = validate_scene(document, context); !problems.empty())
                throw std::runtime_error("the generated scene is invalid: " + problems.front().message);
        }
        result.counters.centers_in_view = centers_in_view(document, manifest, camera);

        auto renderer = Renderer(device, std::move(renderer_shader));
        auto target = RenderTarget(device, {Format::rgba8_unorm, false, "benchmark view"});
        if (auto error = target.resize(manifest.width, manifest.height)) throw std::runtime_error(error.message);
        const auto stage = Stage{device, registry, renderer, target, manifest.width, manifest.height};
        // The warmed empty session: the device, the project's catalog, and the view target, no content.
        result.baseline = memory(device, registry, 0);

        if (manifest.workload == Workload::instances || manifest.workload == Workload::scene) {
            for (uint32_t i = 0; i < manifest.runs; ++i)
                result.runs.push_back(measure(manifest, stage, document, context, camera, true, result, i == 0));
            if (manifest.overhead) result.uninstrumented = measure(manifest, stage, document, context, camera, false, result, false);
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
            for (uint32_t tick = 0; tick < manifest.ticks; ++tick) {
                const auto frame = run_frame(stage, *started.session, camera, false).frame;
                if (tick == 0) sample.first_frame = frame;
                sample.longest_frame = std::max(sample.longest_frame, frame);
            }
            clock.restart();
            started.session.reset();
            sample.stop = clock.milliseconds();
            device.wait_idle(); // retire this cycle's GPU work
            registry.evict_unused(); // explicit maintenance: unused versions leave the cache
            sample.after = memory(device, registry, 0);
            device.take_gpu_timings();
            result.cycles.push_back(sample);
        }
        result.authored_unchanged = scene_text(capture_scene(*authored_world.world), context) == authored_before;
        result.resident = memory(device, registry, authored_world.world->size());
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

Result run(const Manifest& manifest, GraphicsDevice& device, std::string renderer_shader) {
    auto result = Result{};
    result.manifest = manifest;
    result.system = system_info();
    result.build = build_info();
    run_workload(result, manifest, device, std::move(renderer_shader));
    result.thermal_state_at_end = system_info().thermal_state; // throttling during a run shows here
    result.thread_qos = thread_qos();
    return result;
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
    json.field("color_format", "rgba8_unorm");
    json.field("depth_format", "depth32_float");
    json.field("antialiasing", "none");
    json.field("textures", "none (materials are factors)");
    json.field("lighting", "directional lights, factor materials");
    json.field("shadows", "unavailable");
    json.field("presentation", "offscreen, never presented");
    json.field("simulation", "fixed 60 Hz, one tick per frame");
    json.field("upload_mib_per_frame", m.upload_mib);
    json.close('}');
    json.key("counters");
    json.open('{');
    const auto& c = r.counters;
    json.field("entities", c.entities);
    json.field("mesh_renderers", c.mesh_renderers);
    json.field("spinning", c.spinning);
    json.field("centers_in_view", c.centers_in_view);
    json.field("draws_per_frame", c.draws);
    json.field("instances_per_frame", c.instances);
    json.field("triangles_per_frame", c.triangles);
    json.field("passes_per_frame", c.passes);
    json.field("unique_meshes", c.unique_meshes);
    json.field("unique_materials", c.unique_materials);
    json.close('}');
    write_memory(json, "baseline_memory", r.baseline);
    write_memory(json, "resident_memory", r.resident);
    json.key("runs");
    json.open('[');
    for (const auto& run : r.runs) write_run(json, run);
    json.close(']');
    json.key("uninstrumented");
    if (r.uninstrumented) write_run(json, *r.uninstrumented); else json.null();
    json.key("overhead");
    if (r.uninstrumented && !r.runs.empty()) {
        auto instrumented = std::vector<double>{};
        for (const auto& run : r.runs) instrumented.insert(instrumented.end(), run.frame.begin(), run.frame.end());
        const auto with = summarize(instrumented).mean, without = summarize(r.uninstrumented->frame).mean;
        json.open('{');
        json.field("instrumented_mean_ms", with);
        json.field("uninstrumented_mean_ms", without);
        json.field("difference_ms", with - without);
        json.field("difference_percent", without > 0.0 ? (with - without) / without * 100.0 : 0.0);
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
    json.key("unavailable");
    json.open('{');
    for (const auto& [metric, reason] : r.unavailable) json.field(metric, reason);
    json.close('}');
    json.close('}');
    return json.text + "\n";
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
        out << "  run " << i + 1 << ": frame " << frame.mean << " ms (P95 " << frame.p95 << ", P99 " << frame.p99 << ")";
        if (g.count) out << ", GPU " << g.mean << " ms (P95 " << g.p95 << ")";
        else out << ", GPU unavailable";
        out << ", " << (r.runs[i].sampled_seconds > 0 ? double(r.runs[i].frame.size()) / r.runs[i].sampled_seconds : 0.0) << " fps\n";
    }
    if (r.uninstrumented) out << "  without instrumentation: frame " << summarize(r.uninstrumented->frame).mean << " ms\n";
    if (!r.runs.empty())
        out << "  " << r.counters.draws << " draws, " << r.counters.triangles << " triangles, " << r.counters.unique_meshes
            << " unique meshes, " << r.counters.centers_in_view << " of " << r.counters.mesh_renderers << " in view\n";
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
