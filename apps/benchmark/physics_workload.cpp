// P1, the physics stress workload, version 1 (docs/architecture/performance-baseline.md#p1-physics-stress):
// a seeded scene built from the same components as authored content, ticked back to back with no views.

#include "benchmark_detail.hpp"
#include "maya/core/system_info.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/scripting.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace maya::benchmark {
namespace {
using detail::mix;

// Collision groups: Static collides with Dynamic, Dynamic with everything, Trigger with Dynamic.
constexpr int32_t static_group = 0, dynamic_group = 1, trigger_group = 2;
constexpr uint32_t static_mask = 1u << dynamic_group;
constexpr uint32_t dynamic_mask = 1u << static_group | 1u << dynamic_group | 1u << trigger_group;
constexpr uint32_t trigger_mask = 1u << dynamic_group;

constexpr EntityId id(uint64_t low) { return {0x7031, low}; }
constexpr auto paddle_id = id(6);
constexpr float bin_half = 10.0f; // the bin is 20 x 20 m at the origin
constexpr float paddle_speed = 1.0f; // rad/s about Y

/// A number in [0, 1) from the seed and two indices: the same everywhere for the same inputs.
double unit(uint64_t seed, uint64_t a, uint64_t b) {
    return double(mix(seed ^ mix(a * 0x100000001b3ull + b)) >> 11) * 0x1.0p-53;
}
float between(uint64_t seed, uint64_t a, uint64_t b, float low, float high) {
    return low + float(unit(seed, a, b)) * (high - low);
}

TransformComponent placed(math::Vec3 at, float yaw = 0.0f, math::Quat extra = {}) {
    auto transform = TransformComponent{};
    transform.translation = at;
    transform.rotation = math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) * extra;
    transform.rotation.normalize();
    return transform;
}
ColliderComponent box(math::Vec3 half, int32_t group, uint32_t mask, bool sensor = false) {
    auto collider = ColliderComponent{};
    collider.half_extents = half;
    collider.group = group;
    collider.mask = mask;
    collider.sensor = sensor;
    return collider;
}

enum class Shape { box, sphere, capsule };
/// 60% boxes, 25% spheres, 15% capsules, exactly, by index.
Shape shape_of(uint32_t index) { return index % 20 < 12 ? Shape::box : index % 20 < 17 ? Shape::sphere : Shape::capsule; }
ColliderComponent dynamic_collider(Shape shape) {
    auto collider = box(math::Vec3(0.5f), dynamic_group, dynamic_mask);
    if (shape == Shape::sphere) {
        collider.shape = ColliderShape::sphere;
        collider.radius = 0.5f;
    } else if (shape == Shape::capsule) {
        collider.shape = ColliderShape::capsule;
        collider.radius = 0.3f;
        collider.half_height = 0.5f;
    }
    return collider;
}

/// Turns the kinematic paddle about Y, so the bin's bodies never settle.
class PaddleSystem final : public SimulationSystem {
public:
    std::string_view name() const override { return "P1 paddle"; }
    void start(const World& world) override {
        const auto found = world.find(paddle_id);
        if (!found) throw std::runtime_error("the paddle is missing");
        m_paddle = *found;
    }
    void fixed_update(TickContext& tick) override {
        const auto angle = float(double(tick.tick + 1) * tick.delta * paddle_speed); // where it is at the end of this step
        tick.bodies.set_kinematic_target(m_paddle, {0.0f, 1.0f, 0.0f}, math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, angle));
    }

private:
    EntityHandle m_paddle{};
};

/// Asks the tick's queries, generated from the seed and the tick index, and counts their hits.
class QuerySystem final : public SimulationSystem {
public:
    QuerySystem(const Manifest& manifest, uint64_t& hits) : m_manifest(manifest), m_hits(hits) {}
    std::string_view name() const override { return "P1 queries"; }
    void fixed_update(TickContext& tick) override {
        const auto seed = m_manifest.seed ^ 0x717565726965ull; // "queries"
        const auto& physics = tick.physics;
        auto hits = uint64_t{0};
        auto n = tick.tick * 16; // each tick's numbers start apart from the last tick's
        const auto in_bin = [&](float low, float high) {
            return math::Vec3{between(seed, n, 1, -9.0f, 9.0f), between(seed, n, 2, low, high), between(seed, n, 3, -9.0f, 9.0f)};
        };
        for (uint32_t i = 0; i < m_manifest.rays; ++i, ++n) {
            // From a 60 m sphere around the bin, 5 to 30 m high, toward a point in it.
            const auto height = between(seed, n, 4, 5.0f, 30.0f);
            const auto angle = between(seed, n, 5, 0.0f, 6.2831853f);
            const auto across = std::sqrt(60.0f * 60.0f - height * height);
            const auto origin = math::Vec3{std::cos(angle) * across, height, std::sin(angle) * across};
            hits += physics.raycast_nearest(origin, in_bin(0.0f, 3.0f) - origin, 100.0f) ? 1 : 0; // closest-hit rays
        }
        for (uint32_t i = 0; i < m_manifest.overlaps; ++i, ++n) hits += physics.overlap(SphereShape{2.0f}, in_bin(0.5f, 3.0f), {}).size();
        for (uint32_t i = 0; i < m_manifest.casts; ++i, ++n) {
            auto origin = in_bin(0.0f, 0.0f);
            origin.y = 15.0f;
            hits += physics.shape_cast(BoxShape{math::Vec3(0.5f)}, origin, {}, {0.0f, -1.0f, 0.0f}, 20.0f).size();
        }
        m_hits = hits;
    }

private:
    const Manifest& m_manifest;
    uint64_t& m_hits;
};

double since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

double slope(const std::vector<double>& ys, double step) {
    if (ys.size() < 2) return 0.0;
    const auto n = double(ys.size());
    const auto mean_x = (n - 1.0) / 2.0;
    const auto mean_y = std::accumulate(ys.begin(), ys.end(), 0.0) / n;
    auto num = 0.0, den = 0.0;
    for (size_t i = 0; i < ys.size(); ++i) {
        num += (double(i) - mean_x) * (ys[i] - mean_y);
        den += (double(i) - mean_x) * (double(i) - mean_x);
    }
    return num / den / step;
}

/// Limits for the scene's size: the engine's defaults, raised only where the scene needs more.
PhysicsSettings settings_for(size_t bodies) {
    auto settings = PhysicsSettings{};
    while (settings.max_bodies < bodies + bodies / 4) settings.max_bodies *= 2;
    settings.max_body_pairs = std::max<uint32_t>(settings.max_body_pairs, uint32_t(bodies * 8));
    settings.max_contact_constraints = std::max<uint32_t>(settings.max_contact_constraints, uint32_t(bodies * 4));
    return settings;
}

PhysicsRun run_once(const Manifest& manifest, const SceneDocument& document, size_t bodies, int workers) {
    auto run = PhysicsRun{};
    run.workers = workers;
    set_physics_worker_threads(workers);
    run.worker_threads = physics_worker_threads();
    reset_physics_peak();
    run.jolt_live_start = physics_memory().live_bytes;
    try {
        auto hits = uint64_t{0};
        auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
        systems.push_back(std::make_unique<PaddleSystem>());
        systems.push_back(std::make_unique<QuerySystem>(manifest, hits));
        for (auto& system : play_systems([](AssetId script) -> ScriptSourceResult {
                 if (script != physics_script_id) return {std::nullopt, "not the benchmark's script"};
                 return {ScriptSource{"p1.luau", physics_script_source()}, {}};
             }, {}))
            systems.push_back(std::move(system));
        const auto accept = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
        auto clock = std::chrono::steady_clock::now();
        auto started = PlaySession::start(document, accept, std::move(systems), {}, settings_for(bodies));
        if (!started) throw std::runtime_error(started.error.empty() ? started.diagnostics.front().message : started.error);
        run.start_ms = since(clock);
        auto& session = *started.session;
        const auto interval = session.clock().interval();
        const auto tick = [&] {
            const auto played = session.update(interval);
            if (!played.error.empty()) throw std::runtime_error(played.error);
            if (played.ticks_run != 1) throw std::runtime_error("a frame ran " + std::to_string(played.ticks_run) + " ticks, not one");
        };
        for (uint32_t i = 0; i < manifest.warmup; ++i) tick();
        const auto footprint = [] { const auto m = process_memory(); return m ? std::optional(m->footprint) : std::nullopt; };
        run.footprint_start = footprint();
        auto footprints = std::vector<double>{};
        auto events_before = session.physics().stats().events;
        for (uint32_t i = 0; i < manifest.samples; ++i) {
            clock = std::chrono::steady_clock::now();
            tick();
            run.tick.push_back(since(clock));
            // Outside the timed tick: the parts, and the counts.
            auto scripts = 0.0, queries = 0.0, other = 0.0, late = 0.0;
            for (const auto& system : session.system_timings()) {
                if (system.name == "Scripts") scripts += system.fixed_update_ms;
                else if (system.name == "P1 queries") queries += system.fixed_update_ms;
                else other += system.fixed_update_ms;
                late += system.late_fixed_update_ms;
            }
            const auto stats = session.physics().stats();
            run.scripts.push_back(scripts);
            run.queries.push_back(queries);
            run.other_systems.push_back(other);
            run.prepare.push_back(stats.prepare_ms);
            run.step.push_back(stats.step_ms);
            run.synchronize.push_back(stats.synchronize_ms);
            run.events.push_back(stats.events_ms);
            run.late.push_back(late);
            run.commit.push_back(stats.commit_ms);
            const auto moving = stats.kinematic_bodies + stats.dynamic_bodies;
            run.active.push_back(double(stats.active_bodies));
            run.sleeping.push_back(double(moving - stats.active_bodies));
            run.pairs.push_back(double(stats.contacts + stats.overlaps));
            run.contacts.push_back(double(stats.contacts));
            run.events_delivered.push_back(double(stats.events - events_before));
            events_before = stats.events;
            run.query_hits.push_back(double(hits));
            if (i % 100 == 0)
                if (const auto bytes = footprint()) footprints.push_back(double(*bytes));
            if (stats.steps_with_errors > 0 && run.failure.empty())
                run.failure = "a step hit a physics limit (manifold cache " + std::to_string(stats.manifold_cache_full) + ", body pairs " +
                              std::to_string(stats.body_pair_cache_full) + ", contact constraints " +
                              std::to_string(stats.contact_constraints_full) + ")";
        }
        const auto stats = session.physics().stats();
        run.temp_high_water = stats.temp_high_water_bytes;
        run.temp_capacity = stats.temp_capacity_bytes;
        run.script_bytes = script_memory_in_use();
        run.state = session.state_hash();
        run.footprint_end = footprint();
        run.footprint_slope_per_tick = slope(footprints, 100.0);
        run.jolt_peak = physics_memory().peak_bytes;
    } catch (const std::exception& error) {
        run.failure = error.what();
    }
    run.jolt_live_end = physics_memory().live_bytes; // after the session released its world
    set_physics_worker_threads(-1);
    return run;
}

} // namespace

const char* physics_script_source() {
    // Reads its body's velocity and pushes it a little toward the bin's centre; counts its contacts.
    return R"(
local P = {}
function P:start()
    self.contacts = 0
end
function P:fixed_update(dt)
    local v = self.entity:velocity()
    if v == nil then return end
    local p = self.entity:world_position()
    self.entity:add_force(vector.create(-p.x * 20 - v.x * 5, 0, -p.z * 20 - v.z * 5))
end
function P:on_contact_begin(other, contact)
    self.contacts += 1
end
return P
)";
}

SceneDocument physics_scene(const Manifest& manifest, PhysicsScene* counts) {
    const auto seed = manifest.seed;
    auto scene = PhysicsScene{};
    auto document = SceneDocument{};
    const auto add = [&](EntityId entity, std::string name, std::vector<ComponentValue> components) {
        components.insert(components.begin(), NameComponent{std::move(name)});
        document.entities.push_back({entity, std::nullopt, std::move(components)});
    };
    const auto fixed = [&](EntityId entity, std::string name, math::Vec3 at, math::Vec3 half, float yaw = 0.0f) {
        add(entity, std::move(name), {placed(at, yaw), box(half, static_group, static_mask)});
        ++scene.static_bodies;
    };
    // Ground (top at y = 0), and the bin: four 4 m walls around 20 x 20 m.
    fixed(id(1), "Ground", {0.0f, -0.5f, 0.0f}, {100.0f, 0.5f, 100.0f});
    fixed(id(2), "Wall +X", {bin_half + 0.25f, 2.0f, 0.0f}, {0.25f, 2.0f, bin_half + 0.5f});
    fixed(id(3), "Wall -X", {-bin_half - 0.25f, 2.0f, 0.0f}, {0.25f, 2.0f, bin_half + 0.5f});
    fixed(id(4), "Wall +Z", {0.0f, 2.0f, bin_half + 0.25f}, {bin_half, 2.0f, 0.25f});
    fixed(id(5), "Wall -Z", {0.0f, 2.0f, -bin_half - 0.25f}, {bin_half, 2.0f, 0.25f});
    // The paddle: 18 x 1 x 0.5 m, 0.5 m above the floor, kinematic.
    auto paddle = RigidBodyComponent{};
    paddle.motion = BodyMotion::kinematic;
    add(paddle_id, "Paddle", {placed({0.0f, 1.0f, 0.0f}), box({9.0f, 0.5f, 0.25f}, static_group, static_mask), paddle});
    ++scene.kinematic_bodies;

    // The field outside the bin: 13 m cells, shuffled. The first hold the sleeping patches; obstacles
    // go in the others, so the two never overlap.
    constexpr float cell = 13.0f;
    constexpr int cells_per_side = 12;
    auto cells = std::vector<std::pair<float, float>>{};
    for (int x = 0; x < cells_per_side; ++x)
        for (int z = 0; z < cells_per_side; ++z) {
            const auto cx = (float(x) - (cells_per_side - 1) * 0.5f) * cell, cz = (float(z) - (cells_per_side - 1) * 0.5f) * cell;
            if (std::abs(cx) < cell && std::abs(cz) < cell) continue; // the bin
            cells.emplace_back(cx, cz);
        }
    auto order = std::vector<size_t>(cells.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::ranges::sort(order, {}, [&](size_t i) { return mix(seed ^ mix(i + 0x63656c6c)); });

    const auto active = manifest.count * 2 / 5; // dropped into the bin; the rest rest in patches
    const auto sleeping = manifest.count - active;
    const auto patches = (sleeping + 99) / 100;
    if (patches >= cells.size()) throw std::runtime_error("the field has room for " + std::to_string(cells.size() - 1) + " patches");
    for (uint32_t i = 0; i < manifest.obstacles; ++i) {
        const auto [cx, cz] = cells[order[patches + i % (cells.size() - patches)]];
        const auto half = math::Vec3{between(seed, i, 1, 0.25f, 1.5f), between(seed, i, 2, 0.25f, 1.5f), between(seed, i, 3, 0.25f, 1.5f)};
        // Inside the cell, clear of its edges even when turned.
        const auto at = math::Vec3{cx + between(seed, i, 4, -4.3f, 4.3f), half.y, cz + between(seed, i, 5, -4.3f, 4.3f)};
        fixed(id(0x1000 + i), "Obstacle", at, half, between(seed, i, 6, 0.0f, 6.2831853f));
        ++scene.obstacles;
    }
    for (uint32_t i = 0; i < manifest.sensors; ++i) {
        const auto at = math::Vec3{between(seed, 0x5000 + i, 1, -8.0f, 8.0f), between(seed, 0x5000 + i, 2, 1.0f, 3.0f),
                                   between(seed, 0x5000 + i, 3, -8.0f, 8.0f)};
        add(id(0x2000 + i), "Sensor", {placed(at), box(math::Vec3(1.0f), trigger_group, trigger_mask, true)});
        ++scene.static_bodies;
        ++scene.sensors;
    }

    // Scripted bodies: a seeded choice from the active set.
    auto chosen = std::vector<uint32_t>(active);
    std::iota(chosen.begin(), chosen.end(), 0u);
    std::ranges::sort(chosen, {}, [&](uint32_t i) { return mix(seed ^ mix(i + 0x736372)); });
    auto scripted = std::vector<bool>(active, false);
    for (uint32_t i = 0; i < std::min(manifest.scripted, active); ++i) scripted[chosen[i]] = true;

    const auto lying = math::Quat::from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707964f); // capsules rest on their side
    for (uint32_t i = 0; i < manifest.count; ++i) {
        const auto shape = shape_of(i);
        auto components = std::vector<ComponentValue>{};
        if (i < active) {
            // A seeded grid above the bin: 15 x 15 per layer, 1.2 m apart, from 3 m up.
            const auto layer = i / 225, row = i % 225 / 15, column = i % 15;
            const auto at = math::Vec3{-8.4f + 1.2f * float(column) + between(seed, i, 7, -0.1f, 0.1f), 3.0f + 1.25f * float(layer),
                                       -8.4f + 1.2f * float(row) + between(seed, i, 8, -0.1f, 0.1f)};
            components = {placed(at, between(seed, i, 9, 0.0f, 6.2831853f)), dynamic_collider(shape), RigidBodyComponent{}};
            if (scripted[i]) {
                components.push_back(ScriptComponent{AssetRef<ScriptAsset>{physics_script_id}, {}});
                ++scene.scripted;
            }
            ++scene.active_set;
        } else {
            // Single-layer 10 x 10 patches resting on the ground.
            const auto k = i - active;
            const auto [cx, cz] = cells[order[k / 100]];
            const auto rest = shape == Shape::capsule ? 0.3f : 0.5f;
            const auto at = math::Vec3{cx - 5.175f + 1.15f * float(k % 10), rest, cz - 5.175f + 1.15f * float(k % 100 / 10)};
            components = {placed(at, 0.0f, shape == Shape::capsule ? lying : math::Quat{}), dynamic_collider(shape), RigidBodyComponent{}};
            ++scene.sleeping_set;
        }
        add(id(0x10000 + i), "Body", std::move(components));
        ++scene.dynamic_bodies;
        ++(shape == Shape::box ? scene.boxes : shape == Shape::sphere ? scene.spheres : scene.capsules);
    }
    scene.bodies = scene.static_bodies + scene.kinematic_bodies + scene.dynamic_bodies;
    if (counts) *counts = scene;
    return document;
}

void detail::run_physics(Result& result, const Manifest& manifest) {
    const auto document = physics_scene(manifest, &result.physics_scene);
    for (const auto workers : manifest.workers)
        for (uint32_t i = 0; i < manifest.runs; ++i) result.physics_runs.push_back(run_once(manifest, document, result.physics_scene.bodies, workers));
    auto completed = std::vector<const PhysicsRun*>{};
    for (const auto& run : result.physics_runs)
        if (run.failure.empty()) completed.push_back(&run);
    if (!completed.empty())
        result.deterministic = std::ranges::all_of(completed, [&](const PhysicsRun* run) { return run->state == completed.front()->state; });
    if (completed.size() != result.physics_runs.size()) {
        for (const auto& run : result.physics_runs)
            if (!run.failure.empty()) {
                result.failure = "a physics run failed: " + run.failure;
                break;
            }
    } else if (result.deterministic && !*result.deterministic) {
        result.failure = "the runs ended in different states: a determinism failure";
    }
}

} // namespace maya::benchmark
