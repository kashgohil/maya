#include "maya/assets/project.hpp"
#include <charconv>
#include <cmath>
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

CollisionGroupNames default_collision_groups() {
    auto names = CollisionGroupNames{};
    names[0] = "Default";
    return names;
}

std::string validate_collision_group_name(std::string_view name) {
    if (name.empty()) return "A collision group name cannot be empty";
    if (name.size() > max_collision_group_name) return "A collision group name has at most 32 bytes";
    if (name.front() == ' ' || name.back() == ' ') return "A collision group name cannot start or end with a space";
    for (const auto c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '"' || c == '\\')
            return "A collision group name cannot contain quotes, backslashes, or control characters";
    return {};
}

std::string collision_group_label(const CollisionGroupNames& groups, size_t index) {
    if (index >= groups.size()) return "Group " + std::to_string(index);
    return groups[index].empty() ? "Group " + std::to_string(index) : groups[index];
}

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
    auto named = std::array<bool, collision_group_names>{};
    while (input >> key) {
        if (key == "script_work" || key == "script_memory") {
            const auto work = key == "script_work";
            const auto minimum = work ? min_script_work : uint64_t{1};
            const auto maximum = work ? max_script_work : uint64_t{max_script_memory};
            auto number = uint64_t{};
            const auto parsed = (input >> value) ? std::from_chars(value.data(), value.data() + value.size(), number)
                                                 : std::from_chars_result{value.data(), std::errc::invalid_argument};
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number < minimum || number > maximum)
                return {{}, work ? "'script_work' needs a whole number of safepoints per call from 1000 to 1000000000"
                                 : "'script_memory' needs a whole number of MiB from 1 to 4096"};
            if (work ? settings.script_work.has_value() : settings.script_memory.has_value())
                return {{}, "'" + key + "' is set twice"};
            if (work) settings.script_work = number;
            else settings.script_memory = uint32_t(number);
            continue;
        }
        if (key == "stream_load" || key == "stream_activate" || key == "stream_hysteresis") {
            auto& field = key == "stream_load" ? settings.stream_load : key == "stream_activate" ? settings.stream_activate : settings.stream_hysteresis;
            auto number = 0.0;
            const auto parsed = (input >> value) ? std::from_chars(value.data(), value.data() + value.size(), number)
                                                 : std::from_chars_result{value.data(), std::errc::invalid_argument};
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(number) || number < 0.0 ||
                number > 1.0e6)
                return {{}, "'" + key + "' needs a distance in metres from 0 to 1000000"};
            if (field) return {{}, "'" + key + "' is set twice"};
            field = number;
            continue;
        }
        if (key != "group") return {{}, "Unexpected '" + key + "' after the project settings"};
        auto index = 0u;
        if (!(input >> index) || index >= collision_group_names)
            return {{}, "'group' needs a collision group number from 0 to 15"};
        input >> std::ws;
        if (input.peek() != '"' || !(input >> std::quoted(value)))
            return {{}, "'group " + std::to_string(index) + "' needs a quoted name"};
        if (named[index]) return {{}, "Collision group " + std::to_string(index) + " is named twice"};
        if (auto error = validate_collision_group_name(value); !error.empty())
            return {{}, "Collision group " + std::to_string(index) + ": " + error};
        named[index] = true;
        settings.collision_groups[index] = value;
    }
    return {settings, {}};
}

void write_project(std::ostream& output, const ProjectSettings& settings) {
    auto text = std::ostringstream{};
    text << "maya-project " << project_format_version << '\n'
         << "content " << std::quoted(settings.content.generic_string()) << '\n'
         << "catalog " << std::quoted(settings.catalog.generic_string()) << '\n';
    if (!settings.startup_scene.empty()) text << "startup " << std::quoted(settings.startup_scene.generic_string()) << '\n';
    if (settings.script_work) text << "script_work " << *settings.script_work << '\n';
    if (settings.script_memory) text << "script_memory " << *settings.script_memory << '\n';
    if (settings.stream_load) text << "stream_load " << *settings.stream_load << '\n';
    if (settings.stream_activate) text << "stream_activate " << *settings.stream_activate << '\n';
    if (settings.stream_hysteresis) text << "stream_hysteresis " << *settings.stream_hysteresis << '\n';
    const auto defaults = default_collision_groups();
    for (size_t i = 0; i < collision_group_names; ++i)
        if (settings.collision_groups[i] != defaults[i] && !settings.collision_groups[i].empty())
            text << "group " << i << ' ' << std::quoted(settings.collision_groups[i]) << '\n';
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

ProjectAssetsResult open_project_assets(const Project& project, std::unique_ptr<AssetProvider> provider) {
    const auto shown = project.relative(project.catalog).generic_string();
    auto file = std::ifstream(project.catalog);
    if (!file) return {nullptr, "Cannot read the asset catalog " + shown + " in " + project.content_root.string()};
    const auto catalog = read_asset_catalog(file);
    if (!catalog) return {nullptr, shown + ": " + catalog.diagnostic.message};
    auto registry = std::unique_ptr<AssetRegistry>{};
    try {
        registry = std::make_unique<AssetRegistry>(project.content_root, std::move(provider));
    } catch (const std::invalid_argument& error) {
        return {nullptr, error.what()};
    }
    for (const auto& record : catalog.records)
        if (const auto error = registry->register_asset(record)) return {nullptr, shown + ": " + error.message};
    return {std::move(registry), {}};
}

std::string save_project(const Project& project) {
    // Written beside the file, then renamed over it, so a failed write leaves the old file intact.
    auto temporary = project.file;
    temporary += ".saving";
    try {
        {
            auto output = std::ofstream(temporary, std::ios::trunc);
            if (!output) return "Cannot write " + temporary.string();
            write_project(output, project.settings);
            output.flush();
            if (!output) return "Cannot write " + temporary.string();
        }
        std::filesystem::rename(temporary, project.file);
    } catch (const std::exception& failure) {
        auto ignored = std::error_code{};
        std::filesystem::remove(temporary, ignored);
        return "Cannot save " + project.file.string() + ": " + failure.what();
    }
    return {};
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
    project.settings = settings.settings;
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
