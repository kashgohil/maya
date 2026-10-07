#pragma once
// R1, the realistic reference environment (#1040, docs/acceptance.md#milestone-3-rendering-and-content), recipe
// version 1 from the rendering and content record: ABeautifulGame, FlightHelmet, and CesiumMan walking a loop
// around the board, under the Aerodynamics Workshop HDRI with a shadowed sun, a shadowed spot light, and two
// point lights; five named views; and a camera that circles the board in 600 ticks. The content is fetched
// by tools/fetch_render_samples.sh and never committed, so the project is assembled from it.

#include "maya/assets/texture_data.hpp"
#include "maya/core/identity.hpp"
#include "maya/math/vector.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace maya::r1 {

inline constexpr uint32_t recipe_version = 1;
/// The camera path and the walk loop: one 10-second clip, 600 ticks at 60 Hz.
inline constexpr uint32_t path_ticks = 600;
inline constexpr float path_seconds = 10.0f;

/// A named view: a camera entity of r1.scene, by its fixed ID.
struct View {
    std::string_view name; // "overview", "board", "helmet", "walker", "grazing"
    EntityId camera;
    math::Vec3 from, to;
};
std::span<const View> views();
/// The camera that follows the path: the scene's first camera, which the player shows.
inline constexpr std::string_view path_camera_name = "Path camera";

struct Options {
    std::filesystem::path samples; // tools/fetch_render_samples.sh's folder: Models/ and hdri/
    std::filesystem::path output; // the project folder; assembled again in place, keeping its IDs and cook cache
    TextureCompression compression = TextureCompression::astc; // rgba8 cooks far faster in unoptimized builds
};
struct Result {
    std::string error; // empty on success
    std::filesystem::path project; // <output>/project.maya
    std::filesystem::path scene; // <output>/r1.scene, the startup scene
    explicit operator bool() const noexcept { return error.empty(); }
};

/// Writes the R1 project into `options.output`: copies the models and the HDRI from the samples, writes the
/// path file, a floor, and an environment, imports the models, and writes r1.scene. Assembling it again
/// imports again, which keeps every ID, and leaves the cook cache. Fails, with the reason, when the samples
/// are missing or an import fails.
Result assemble(const Options& options);

/// Whether the samples R1 needs are there.
bool samples_present(const std::filesystem::path& samples);

} // namespace maya::r1
