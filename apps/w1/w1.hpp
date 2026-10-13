#pragma once
// W1, the reference world (#1069, docs/acceptance.md#w1), recipe version 2 from the world-scale record
// (docs/architecture/world-scale-decision.md#w1-the-reference-world, with the owner's changes of 12 October
// 2026): 4 km of noise terrain in forest and rock from 4,096 m to 8,192 m, scattered boulders, rocks, and
// deadwood, a clearing with ABeautifulGame and FlightHelmet, CesiumMan walking a loop across three cells, a
// watch camera that follows him from the persistent part, 400 resting rocks and 20 boulders that roll, a
// shadowed sun under the Kloofendal sky, five named views, and S1's camera on its 160-second path. The content
// is fetched by tools/fetch_world_samples.sh and tools/fetch_render_samples.sh and never committed, so the
// project is assembled from it.

#include "maya/assets/texture_data.hpp"
#include "maya/core/identity.hpp"
#include "maya/math/vector.hpp"
#include "maya/scene/world_io.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace maya::w1 {

inline constexpr uint32_t recipe_version = 2;

/// The world's cells, 32 to 63 along x and z: 4,096 m to 8,192 m.
inline constexpr int32_t first_cell = 32, last_cell = 63;

/// S1, the traversal: 120 s on foot at 8 m/s along four legs (three turn-backs across cell edges), then a
/// 40 s flyover at 60 m/s, at 60 ticks a second; 3.36 km in all.
inline constexpr uint32_t s1_ticks = 9600;
inline constexpr uint32_t s1_walk_ticks = 7200;
inline constexpr float s1_seconds = 160.0f;
/// Where S1's camera is at a tick (0 to s1_ticks): on foot it stands `s1_eye_height` over the terrain at
/// (x, z), facing along the leg; in the flyover it is `s1_flight_height` up, looking ahead and down.
struct S1Point {
    double x = 0.0, z = 0.0;
    bool on_foot = true;
    math::Vec3 forward{1.0f, 0.0f, 0.0f};
};
S1Point s1_point(uint32_t tick);
inline constexpr float s1_eye_height = 1.7f, s1_flight_height = 220.0f;
/// The camera that follows S1: the persistent part's first camera, which the player shows.
inline constexpr std::string_view s1_camera_name = "S1 camera";

/// A named view: a camera entity of the persistent part, by its fixed ID.
struct View {
    std::string_view name; // "clearing", "hillside", "vista", "flyover", "cell-edge"
    EntityId camera;
    math::DVec3 from, to;
};
std::span<const View> views();

/// The soak's regeneration (#1069, decided 12 October 2026): a second rocks generator, with the main one's
/// rules, over the 2 x 2 cells the main one is excluded from. Each soak cycle gives it a new seed.
AssetId soak_rocks_generator();
inline constexpr CellIndex soak_first_cell{44, 43}, soak_last_cell{45, 44};
/// The watch camera, in the persistent part, and what it watches: CesiumMan's walk loop, in a cell.
EntityId watch_camera();

struct Options {
    std::filesystem::path world_samples; // tools/fetch_world_samples.sh's folder: models/, textures/, hdri/
    std::filesystem::path render_samples; // tools/fetch_render_samples.sh's folder: Models/
    std::filesystem::path output; // the project folder; assembled again in place, keeping its IDs and cook cache
    TextureCompression compression = TextureCompression::astc; // rgba8 cooks far faster in unoptimized builds
};
struct Result {
    std::string error; // empty on success
    std::filesystem::path project; // <output>/project.maya
    std::filesystem::path world; // <output>/w1.world, the startup world
    size_t cells = 0; // with content
    size_t entities = 0; // authored, in every part
    explicit operator bool() const noexcept { return error.empty(); }
};

/// Writes the W1 project into `options.output`: copies the models, textures, and the sky from the samples,
/// writes the generators, materials, the watch script, and the path files, imports the models, and writes
/// w1.world and its parts. Assembling it again imports again, which keeps every ID, and leaves the cook
/// cache. Fails, with the reason, when the samples are missing or an import fails.
Result assemble(const Options& options);

/// Whether the samples W1 needs are there: what's missing, or empty.
std::string missing_samples(const std::filesystem::path& world_samples, const std::filesystem::path& render_samples);

} // namespace maya::w1
