// The stream workload (#1064, docs/performance.md#stream): a generated grid world crossed back and forth
// while its cells stream in and out, headless. Frames are paced at 60 Hz; each streams, then runs one tick.
// It records how long both took, the streaming's part, and the process's footprint at each crossing's end.

#include "benchmark_detail.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/core/system_info.hpp"
#include "maya/scene/world_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/streaming/world_streamer.hpp"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace maya::benchmark {
namespace {
using detail::mix;
const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};

/// The world: a camera and physics settings, and in each cell a ground box and `count` entities, roots with
/// a child each, one root in ten a static collider, placed by the seed.
SceneDocument stream_world(const Manifest& manifest) {
    auto scene = SceneDocument{};
    scene.entities.push_back({detail::camera_id, std::nullopt, {NameComponent{"Camera"}, TransformComponent{{0.0, 2.0, 0.0}}, CameraComponent{}}});
    scene.entities.push_back({detail::light_id, std::nullopt, {NameComponent{"Settings"}, PhysicsSettingsComponent{}}});
    const auto half = int32_t(manifest.grid / 2);
    auto next = detail::first_instance;
    for (int32_t cx = -half; cx < int32_t(manifest.grid) - half; ++cx)
        for (int32_t cz = -half; cz < int32_t(manifest.grid) - half; ++cz) {
            const auto x0 = double(cx) * default_cell_size, z0 = double(cz) * default_cell_size;
            auto ground = ColliderComponent{};
            ground.half_extents = {64.0f, 0.5f, 64.0f};
            scene.entities.push_back({{detail::camera_id.high, next++}, std::nullopt,
                                      {NameComponent{"Ground"}, TransformComponent{{x0 + 64.0, -0.5, z0 + 64.0}}, ground}});
            for (uint32_t i = 0; i + 1 < manifest.count; i += 2) {
                const auto random = mix(manifest.seed ^ next);
                const auto root = EntityId{detail::camera_id.high, next++};
                auto components = std::vector<ComponentValue>{
                    NameComponent{"Prop"},
                    TransformComponent{{x0 + double(random % 12700) / 100.0, 0.5, z0 + double((random >> 20) % 12700) / 100.0}}};
                if ((random >> 40) % 10 == 0) components.push_back(ColliderComponent{});
                scene.entities.push_back({root, std::nullopt, std::move(components)});
                scene.entities.push_back({{detail::camera_id.high, next++}, root, {NameComponent{"Part"}, TransformComponent{{0.0, 1.0, 0.0}}}});
            }
        }
    return scene;
}
} // namespace

void detail::run_stream(Result& result, const Manifest& manifest) {
    const auto folder = std::filesystem::temp_directory_path() / ("maya-benchmark-stream-" + std::to_string(::getpid()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            auto error = std::error_code{};
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    const auto file = folder / "levels/stream.world";
    std::filesystem::create_directories(file.parent_path());
    if (auto saved = save_world(file, stream_world(manifest), any_asset); !saved.empty()) throw std::runtime_error(saved.front().message);
    auto loaded = load_world(file, any_asset);
    if (!loaded) throw std::runtime_error(loaded.diagnostics.front().message);
    const auto half = double(manifest.grid / 2) * default_cell_size;
    const auto extent = double(manifest.grid) * default_cell_size;
    for (uint32_t r = 0; r < manifest.runs; ++r) {
        auto& run = result.stream_runs.emplace_back();
        try {
            run.cells = loaded.world->cells.size();
            for (const auto& [index, cell] : loaded.cells) run.entities += cell.entities.size();
            auto started = PlaySession::start(loaded.persistent, any_asset, builtin_systems());
            if (!started) throw std::runtime_error(started.error.empty() ? started.diagnostics.front().message : started.error);
            auto& session = *started.session;
            auto settings = StreamingSettings{};
            settings.load_radius = manifest.load_radius;
            settings.activate_radius = manifest.activate_radius;
            settings.hysteresis = manifest.hysteresis;
            // The first run cooks the cells into the cache; later runs read them cooked.
            auto streamer = WorldStreamer(*loaded.world, cooked_cell_loader(file.parent_path(), std::make_shared<CookCache>(folder / "cache"), any_asset),
                                          settings);
            const auto interval = session.clock().interval();
            const auto step = manifest.speed * interval;
            const auto z = 0.5 * default_cell_size; // through the middle of a row of cells
            auto x = -half;
            auto direction = 1.0;
            streamer.set_sources({{x, 0.0, z}});
            streamer.settle(session.world(), &session.physics()); // a loading screen at the start
            const auto footprint = [] { const auto m = process_memory(); return m ? std::optional(m->footprint) : std::nullopt; };
            for (uint32_t crossing = 0; crossing < manifest.cycles; ++crossing) {
                const auto crossing_start = std::chrono::steady_clock::now();
                streamer.reset_peaks();
                const auto frames = uint32_t(std::ceil(extent / step));
                auto due = std::chrono::steady_clock::now();
                for (uint32_t frame = 0; frame < frames; ++frame) {
                    // Paced at 60 Hz, as a game is: loads get the time between frames that they would have.
                    due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(interval));
                    std::this_thread::sleep_until(due);
                    x = std::clamp(x + direction * step, -half, extent - half);
                    const auto start = std::chrono::steady_clock::now();
                    streamer.set_sources({{x, 0.0, z}});
                    const auto& stats = streamer.update(session.world(), &session.physics());
                    const auto played = session.update(interval);
                    if (!played.error.empty()) throw std::runtime_error(played.error);
                    if (stats.loads_failed > 0) throw std::runtime_error("a cell failed to load: " + stats.last_error);
                    run.frame.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                    run.streaming.push_back(stats.frame_ms);
                }
                direction = -direction;
                const auto& stats = streamer.stats();
                run.crossings.push_back({std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - crossing_start).count(),
                                         stats.longest_frame_ms, footprint(), stats.active_entities, stats.bytes_loaded});
            }
            const auto& stats = streamer.stats();
            run.loads = stats.loads_started;
            run.cancelled = stats.loads_cancelled;
            run.failed = stats.loads_failed;
            run.discarded = stats.completions_discarded;
            run.activations = stats.activations;
            run.deactivations = stats.deactivations;
        } catch (const std::exception& error) {
            run.failure = error.what();
            throw;
        }
    }
}

} // namespace maya::benchmark
