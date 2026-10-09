#pragma once

#include "maya/scene/scene_io.hpp"
#include <compare>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

// World assets (#1064, docs/world.md#worlds): a world is divided into cells on a uniform grid in x and z
// (#1060: 128 m), each a column that streams on its own, plus a persistent part that is always loaded.
// On disk, `levels/w1.world` names the grid, the persistent scene, and each occupied cell's scene, kept
// beside it in `levels/w1/`. Cell scenes are ordinary scene text, cooked to packed binary when loaded.

inline constexpr uint32_t world_format_version = 1;
inline constexpr double default_cell_size = 128.0; // metres

/// A cell's place on the grid: floor(position / cell size) in x and z.
struct CellIndex {
    int32_t x = 0;
    int32_t z = 0;
    auto operator<=>(const CellIndex&) const = default;
};
/// The cell a world position is in. Positions beyond the grid's 32-bit range clamp to its edge.
CellIndex cell_of(const math::DVec3& position, double cell_size = default_cell_size);
/// The cell's centre in x and z (y is 0).
math::DVec3 cell_center(CellIndex cell, double cell_size = default_cell_size);
std::string cell_name(CellIndex cell); // "3_-4"

struct WorldCell {
    CellIndex index;
    std::filesystem::path scene; // relative to the world file's folder
    uint32_t entities = 0;
};
/// The world file: what it holds, not the scenes themselves.
struct WorldDocument {
    double cell_size = default_cell_size;
    std::filesystem::path persistent; // relative to the world file's folder
    std::vector<WorldCell> cells; // sorted by index, one per occupied cell
    const WorldCell* cell(CellIndex index) const;
};

struct WorldReadResult {
    std::optional<WorldDocument> document;
    std::string error; // "line 3: ..." when the file is not a world this build reads
    explicit operator bool() const noexcept { return document.has_value(); }
};
WorldReadResult read_world(std::string_view text);
std::string write_world(const WorldDocument& document);

/// A world's content, split: the persistent part and each occupied cell's scene.
struct WorldContent {
    SceneDocument persistent;
    std::map<CellIndex, SceneDocument> cells;
};
/// Splits a scene into a world's parts. A root entity and everything below it go together: into the
/// persistent part when any of them is a camera, a directional light, an environment, or physics
/// settings (what the whole world needs), else into the cell of the root's translation. Roots without a
/// transform are persistent. A hierarchy therefore never crosses a cell.
WorldContent partition_world(const SceneDocument& scene, double cell_size = default_cell_size);

/// Saves `scene` as the world `path` (a `.world` file): its persistent scene and one scene per occupied
/// cell in the folder named after it, each file atomically, then the world file last. Validates first;
/// nothing is written when it fails. Cell scenes of cells no longer occupied are removed.
SceneDiagnostics save_world(const std::filesystem::path& path, SceneDocument scene, const PropertyValidationContext& context,
                            double cell_size = default_cell_size);

/// A world read in full: the world file, and each part loaded and validated. For tests and tools; play
/// streams cells instead (simulation/world_streamer.hpp).
struct WorldLoadResult {
    std::optional<WorldDocument> world;
    SceneDocument persistent;
    std::map<CellIndex, SceneDocument> cells;
    SceneDiagnostics diagnostics;
    explicit operator bool() const noexcept { return world.has_value() && diagnostics.empty(); }
    /// Every part as one scene: what a whole-world load builds.
    SceneDocument whole() const;
};
WorldLoadResult load_world(const std::filesystem::path& path, const PropertyValidationContext& context);

} // namespace maya
