#pragma once
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace maya {
inline constexpr uint32_t project_format_version = 1;
/// The project file's name when a project is given as a directory.
inline constexpr const char* project_file_name = "project.maya";

/// A project file's settings. Every path is relative and stays inside its base, so a project keeps
/// working wherever its directory is moved or copied.
struct ProjectSettings {
    std::filesystem::path content = "."; // content root, relative to the project file's directory
    std::filesystem::path catalog = "catalog.maya"; // asset catalog, relative to the content root
    std::filesystem::path startup_scene; // optional scene to open first, relative to the content root
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
///
/// `content` and `catalog` are required and `startup` is optional, in that order.
ProjectSettingsResult read_project(std::istream& input);
void write_project(std::ostream& output, const ProjectSettings& settings);

/// An opened project: canonical absolute locations derived from its file, never from the working
/// directory or application search roots.
struct Project {
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
/// Opens a project from its file, or from a directory that contains project.maya. A relative path is
/// taken from the working directory. Fails when the file is unreadable or invalid, or its content root
/// is not an existing directory. The catalog and startup scene are checked when they are read.
ProjectResult open_project(const std::filesystem::path& file_or_directory);
} // namespace maya
