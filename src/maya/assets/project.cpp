#include "maya/assets/project.hpp"
#include <fstream>
#include <iomanip>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace maya {
namespace {
/// A path that stays inside its base however the project moves: relative, without "..".
bool portable(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path.lexically_normal()) if (part == "..") return false;
    return true;
}
bool names_file(const std::filesystem::path& path) {
    const auto normal = path.lexically_normal();
    return portable(path) && normal != "." && normal.has_filename();
}
bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == ".") return false; // the root itself is not inside it
    for (const auto& part : relative) if (part == "..") return false;
    return true;
}
} // namespace

ProjectSettingsResult read_project(std::istream& input) {
    auto magic = std::string{};
    auto version = 0u;
    if (!(input >> magic >> version) || magic != "maya-project")
        return {{}, "Expected a maya-project header"};
    if (version != project_format_version)
        return {{}, "Unsupported project version " + std::to_string(version) + "; this editor reads version 1"};
    auto settings = ProjectSettings{};
    auto key = std::string{}, value = std::string{};
    const auto field = [&](const char* expected, bool required) -> std::optional<std::string> {
        const auto start = input.tellg();
        if (!(input >> key)) {
            if (required) return std::string("Missing '") + expected + "'";
            input.clear();
            return std::nullopt;
        }
        if (key != expected) {
            if (required) return std::string("Expected '") + expected + "' but found '" + key + "'";
            input.seekg(start);
            return std::nullopt;
        }
        if (!(input >> std::quoted(value))) return std::string("'") + expected + "' needs a quoted path";
        return std::string{};
    };
    if (auto error = field("content", true); !error->empty()) return {{}, *error};
    settings.content = value;
    if (!portable(settings.content))
        return {{}, "The content root \"" + value + "\" must be a relative path inside the project directory"};
    if (auto error = field("catalog", true); !error->empty()) return {{}, *error};
    settings.catalog = value;
    if (!names_file(settings.catalog))
        return {{}, "The catalog \"" + value + "\" must be a relative file path inside the content root"};
    if (auto error = field("startup", false)) {
        if (!error->empty()) return {{}, *error};
        settings.startup_scene = value;
        if (!names_file(settings.startup_scene))
            return {{}, "The startup scene \"" + value + "\" must be a relative file path inside the content root"};
    }
    if (input >> key) return {{}, "Unexpected '" + key + "' after the project settings"};
    return {settings, {}};
}

void write_project(std::ostream& output, const ProjectSettings& settings) {
    auto text = std::ostringstream{};
    text << "maya-project " << project_format_version << '\n'
         << "content " << std::quoted(settings.content.generic_string()) << '\n'
         << "catalog " << std::quoted(settings.catalog.generic_string()) << '\n';
    if (!settings.startup_scene.empty()) text << "startup " << std::quoted(settings.startup_scene.generic_string()) << '\n';
    output << text.str();
    if (!output) throw std::runtime_error("Cannot write project settings");
}

std::optional<std::filesystem::path> Project::resolve(const std::filesystem::path& path) const {
    if (path.empty() || content_root.empty()) return std::nullopt;
    if (!path.is_absolute() && !portable(path)) return std::nullopt;
    auto error = std::error_code{};
    const auto full = std::filesystem::weakly_canonical(path.is_absolute() ? path : content_root / path, error);
    if (error || !inside(content_root, full)) return std::nullopt;
    return full;
}

std::filesystem::path Project::relative(const std::filesystem::path& path) const {
    auto error = std::error_code{};
    const auto full = std::filesystem::weakly_canonical(path, error);
    if (error || !inside(content_root, full)) return {};
    return full.lexically_relative(content_root);
}

std::string Project::name() const {
    return file.parent_path().filename().string();
}

ProjectResult open_project(const std::filesystem::path& file_or_directory) {
    auto error = std::error_code{};
    auto file = std::filesystem::absolute(file_or_directory, error);
    if (error || file_or_directory.empty()) return {{}, "No project path given"};
    if (std::filesystem::is_directory(file, error)) file /= project_file_name;
    auto stream = std::ifstream(file);
    if (!stream) return {{}, "Cannot read project file " + file.string()};
    const auto settings = read_project(stream);
    if (!settings) return {{}, file.string() + ": " + settings.error};
    auto project = Project{};
    project.file = std::filesystem::canonical(file, error);
    if (error) return {{}, "Cannot resolve project file " + file.string()};
    const auto root = project.file.parent_path() / settings.settings.content.lexically_normal();
    project.content_root = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(project.content_root, error))
        return {{}, file.string() + ": the content root " + root.lexically_normal().string() + " is not a directory"};
    const auto catalog = project.resolve(settings.settings.catalog);
    if (!catalog) return {{}, file.string() + ": the catalog leaves the content root"};
    project.catalog = *catalog;
    if (!settings.settings.startup_scene.empty()) {
        project.startup_scene = project.resolve(settings.settings.startup_scene);
        if (!project.startup_scene) return {{}, file.string() + ": the startup scene leaves the content root"};
    }
    return {std::move(project), {}};
}
} // namespace maya
