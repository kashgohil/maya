#pragma once
#include "maya/assets/registry.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace maya {
inline constexpr uint32_t project_format_version = 1;
/// The project file's name when a project is given as a directory.
inline constexpr const char* project_file_name = "project.maya";

inline constexpr size_t collision_group_names = 16;
inline constexpr size_t max_collision_group_name = 32; // bytes
/// Names of the physics collision groups, by index. Group 0 is the default group every collider starts
/// in; an empty name is an unnamed group, shown as "Group <n>".
using CollisionGroupNames = std::array<std::string, collision_group_names>;
CollisionGroupNames default_collision_groups();
/// Why a name cannot name a collision group, or empty: names are 1 to 32 bytes of printable text,
/// without quotes, backslashes, or surrounding spaces.
std::string validate_collision_group_name(std::string_view name);
/// The name to show for group `index`: its name, or "Group <n>" when it has none.
std::string collision_group_label(const CollisionGroupNames& groups, size_t index);

/// Bounds of the script limits a project may set (docs/scripting.md#sandbox-and-limits).
inline constexpr uint64_t min_script_work = 1'000, max_script_work = 1'000'000'000; // safepoints per hook call
inline constexpr uint32_t max_script_memory = 4096; // MiB per play session
/// The resident-memory budgets a project may set (#1063, docs/assets.md#residency), in MiB, by key: the
/// budgeted categories, then the total.
inline constexpr std::array<const char*, 6> resident_setting_keys = {"resident_meshes", "resident_textures", "resident_environments",
                                                                      "resident_animation", "resident_cells", "resident_total"};
inline constexpr uint32_t max_resident_mib = 1u << 20; // 1 TiB

/// A project file's settings. Every path is relative and stays inside its base, so a project keeps
/// working wherever its directory is moved or copied.
struct ProjectSettings {
    std::filesystem::path content = "."; // content root, relative to the project file's directory
    std::filesystem::path catalog = "catalog.maya"; // asset catalog, relative to the content root
    std::filesystem::path startup_scene; // optional scene to open first, relative to the content root
    CollisionGroupNames collision_groups = default_collision_groups();
    std::optional<uint64_t> script_work; // safepoints per hook call; the engine's default when unset
    std::optional<uint32_t> script_memory; // MiB per play session; the engine's default when unset
    /// Streaming radii for worlds (#1064, docs/world.md#streaming), in metres: from a source to a cell, to
    /// load it, to activate it, and the hysteresis before either is undone. The engine's defaults when unset.
    std::optional<double> stream_load, stream_activate, stream_hysteresis;
    /// Resident-memory budgets in MiB, as resident_setting_keys orders them (#1063); W1's when unset.
    std::array<std::optional<uint32_t>, resident_setting_keys.size()> resident;
    /// The cook cache's size in MiB past which opening the project prunes it; 4 GiB when unset, 0 for never.
    std::optional<uint32_t> cook_cache_limit;
};
struct ProjectSettingsResult {
    ProjectSettings settings;
    std::string error; // empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Parses the versioned text format:
///
///     maya-project 1
///     content "assets"
///     catalog "catalog.maya"
///     startup "basic.scene"
///     script_work 5000000
///     script_memory 256
///     stream_load 640
///     group 1 "Player"
///
/// `content` and `catalog` are required and `startup` is optional, in that order. The optional script
/// limits and streaming radii (`stream_load`, `stream_activate`, `stream_hysteresis`) follow, then any number of `group <index> "<name>"` lines, one per named collision group other
/// than the default names; after `startup`, these lines may come in any order.
ProjectSettingsResult read_project(std::istream& input);
void write_project(std::ostream& output, const ProjectSettings& settings);

/// An opened project: canonical absolute locations derived from its file, never from the working
/// directory or application search roots.
struct Project {
    ProjectSettings settings; // as read from the file; written back when project settings change
    std::filesystem::path file; // the project file
    std::filesystem::path content_root; // an existing directory
    std::filesystem::path catalog; // may not exist; reading it reports that
    std::optional<std::filesystem::path> startup_scene;

    /// The absolute path of a content-relative path (or an absolute one) inside the content root,
    /// following symlinks; nullopt when it would leave the root. The file need not exist.
    std::optional<std::filesystem::path> resolve(const std::filesystem::path& path) const;
    /// A path inside the content root as a content-relative path, e.g. "scenes/level.scene"; empty
    /// when it is outside.
    std::filesystem::path relative(const std::filesystem::path& path) const;
    /// The project directory's name, for display.
    std::string name() const;
};
struct ProjectResult {
    Project project;
    std::string error; // empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};
struct ProjectAssetsResult {
    std::unique_ptr<AssetRegistry> registry; // null on failure
    std::string error; // names the catalog and the reason
    explicit operator bool() const noexcept { return static_cast<bool>(registry); }
};
/// Reads the project's catalog into a new registry rooted at its content root. Nothing is loaded yet.
/// A catalog that cannot be read, parsed, or registered produces no registry.
ProjectAssetsResult open_project_assets(const Project& project, std::unique_ptr<AssetProvider> provider);

/// Writes the project's settings back to its file, replacing it atomically.
std::string save_project(const Project& project);

/// Opens a project from its file, or from a directory that contains project.maya. A relative path is
/// taken from the working directory. Fails when the file is unreadable or invalid, or its content root
/// is not an existing directory. The catalog and startup scene are checked when they are read.
ProjectResult open_project(const std::filesystem::path& file_or_directory);
} // namespace maya
