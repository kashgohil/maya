#include "maya/scene/world_io.hpp"
#include "maya/core/file_replace.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

namespace maya {
namespace {
constexpr std::string_view header_keyword = "maya-world";

/// The words of a line: plain words, and double-quoted strings with \" and \\ escapes.
std::optional<std::vector<std::string>> words(std::string_view line) {
    auto result = std::vector<std::string>{};
    size_t at = 0;
    while (at < line.size()) {
        if (line[at] == ' ' || line[at] == '\t') {
            ++at;
            continue;
        }
        auto word = std::string{};
        if (line[at] == '"') {
            ++at;
            for (;; ++at) {
                if (at >= line.size()) return std::nullopt;
                if (line[at] == '"') break;
                if (line[at] == '\\') {
                    if (++at >= line.size()) return std::nullopt;
                }
                word += line[at];
            }
            ++at;
        } else {
            while (at < line.size() && line[at] != ' ' && line[at] != '\t') word += line[at++];
        }
        result.push_back(std::move(word));
    }
    return result;
}
std::string quoted(const std::string& text) {
    auto result = std::string("\"");
    for (const auto c : text) {
        if (c == '"' || c == '\\') result += '\\';
        result += c;
    }
    return result + '"';
}
template<class T> bool number(const std::string& text, T& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

/// Persistent: what the whole world needs, wherever it is.
bool persistent(const ComponentValue& value) {
    if (std::holds_alternative<CameraComponent>(value) || std::holds_alternative<EnvironmentComponent>(value) ||
        std::holds_alternative<PhysicsSettingsComponent>(value))
        return true;
    const auto* light = std::get_if<LightComponent>(&value);
    return light && light->kind == LightKind::directional;
}
} // namespace

CellIndex cell_of(const math::DVec3& position, double cell_size) {
    const auto index = [&](double coordinate) {
        const auto cell = std::floor(coordinate / cell_size);
        return int32_t(std::clamp(cell, double(std::numeric_limits<int32_t>::min()), double(std::numeric_limits<int32_t>::max())));
    };
    return {index(position.x), index(position.z)};
}

math::DVec3 cell_center(CellIndex cell, double cell_size) {
    return {(double(cell.x) + 0.5) * cell_size, 0.0, (double(cell.z) + 0.5) * cell_size};
}

std::string cell_name(CellIndex cell) { return std::to_string(cell.x) + "_" + std::to_string(cell.z); }

const WorldCell* WorldDocument::cell(CellIndex index) const {
    const auto found = std::ranges::lower_bound(cells, index, {}, &WorldCell::index);
    return found != cells.end() && found->index == index ? &*found : nullptr;
}

WorldReadResult read_world(std::string_view text) {
    auto document = WorldDocument{};
    auto line_number = size_t{0};
    auto header = false, has_persistent = false, has_size = false;
    const auto fail = [&](std::string message) {
        return WorldReadResult{std::nullopt, "line " + std::to_string(line_number) + ": " + std::move(message)};
    };
    auto input = std::istringstream(std::string(text));
    for (auto line = std::string{}; std::getline(input, line);) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto parsed = words(line);
        if (!parsed) return fail("an unterminated quoted string");
        const auto& w = *parsed;
        if (w.empty() || w.front().starts_with('#')) continue;
        if (!header) {
            auto version = uint32_t{};
            if (w.size() != 2 || w[0] != header_keyword || !number(w[1], version)) return fail("not a world file: it begins 'maya-world 1'");
            if (version != world_format_version)
                return fail("world format version " + w[1] + " is not one this build reads (" + std::to_string(world_format_version) + ")");
            header = true;
        } else if (w[0] == "cell_size") {
            if (w.size() != 2 || has_size || !number(w[1], document.cell_size) || !(document.cell_size > 0.0) ||
                !std::isfinite(document.cell_size))
                return fail("'cell_size' takes one positive number of metres, once");
            has_size = true;
        } else if (w[0] == "persistent") {
            if (w.size() != 2 || has_persistent) return fail("'persistent' takes one quoted scene path, once");
            document.persistent = w[1];
            has_persistent = true;
        } else if (w[0] == "cell") {
            auto cell = WorldCell{};
            if (w.size() != 5 || !number(w[1], cell.index.x) || !number(w[2], cell.index.z) || !number(w[4], cell.entities))
                return fail("'cell' takes x, z, a quoted scene path, and an entity count");
            cell.scene = w[3];
            if (cell.scene.is_absolute() || cell.scene.empty()) return fail("a cell's scene path is relative to the world file");
            if (document.cell(cell.index)) return fail("cell " + cell_name(cell.index) + " is listed twice");
            document.cells.insert(std::ranges::upper_bound(document.cells, cell.index, {}, &WorldCell::index), std::move(cell));
        } else {
            return fail("unexpected '" + w[0] + "'");
        }
    }
    if (!header) return fail("not a world file: it is empty");
    if (!has_persistent) return {std::nullopt, "the world names no persistent scene"};
    return {std::move(document), {}};
}

std::string write_world(const WorldDocument& document) {
    auto out = std::string(header_keyword) + ' ' + std::to_string(world_format_version) + '\n';
    char size[32];
    out += "cell_size " + std::string(size, std::to_chars(size, size + sizeof size, document.cell_size).ptr) + '\n';
    out += "persistent " + quoted(document.persistent.generic_string()) + '\n';
    for (const auto& cell : document.cells)
        out += "cell " + std::to_string(cell.index.x) + ' ' + std::to_string(cell.index.z) + ' ' + quoted(cell.scene.generic_string()) + ' ' +
               std::to_string(cell.entities) + '\n';
    return out;
}

WorldContent partition_world(const SceneDocument& scene, double cell_size) {
    auto content = WorldContent{};
    // Each entity's root, and whether its subtree is persistent.
    auto parent_of = std::unordered_map<EntityId, EntityId, PersistentIdHash>{};
    for (const auto& entity : scene.entities)
        if (entity.parent) parent_of.emplace(entity.id, *entity.parent);
    const auto root_of = [&](EntityId id) {
        for (auto found = parent_of.find(id); found != parent_of.end(); found = parent_of.find(id)) id = found->second;
        return id;
    };
    auto persistent_roots = std::set<EntityId>{};
    auto root_place = std::unordered_map<EntityId, std::optional<CellIndex>, PersistentIdHash>{};
    for (const auto& entity : scene.entities) {
        const auto root = root_of(entity.id);
        if (std::ranges::any_of(entity.components, persistent)) persistent_roots.insert(root);
        if (!entity.parent) {
            const auto transform = std::ranges::find_if(entity.components, [](const ComponentValue& value) {
                return std::holds_alternative<TransformComponent>(value);
            });
            root_place[entity.id] = transform == entity.components.end()
                ? std::nullopt
                : std::optional(cell_of(std::get<TransformComponent>(*transform).translation, cell_size));
        }
    }
    for (const auto& entity : scene.entities) {
        const auto root = root_of(entity.id);
        const auto place = root_place.contains(root) ? root_place.at(root) : std::nullopt;
        if (persistent_roots.contains(root) || !place) content.persistent.entities.push_back(entity);
        else content.cells[*place].entities.push_back(entity); // document order, so sibling order holds
    }
    return content;
}

SceneDiagnostics save_world(const std::filesystem::path& path, SceneDocument scene, const PropertyValidationContext& context, double cell_size) {
    if (auto diagnostics = validate_scene(scene, context); !diagnostics.empty()) return diagnostics;
    const auto fail = [&](std::string message) {
        return SceneDiagnostics{{SceneError::io_error, path.generic_string() + ": " + std::move(message)}};
    };
    if (path.extension() != ".world") return fail("a world file's name ends in .world");
    if (!(cell_size > 0.0) || !std::isfinite(cell_size)) return fail("the cell size must be a positive number of metres");
    const auto content = partition_world(scene, cell_size);
    const auto folder_name = path.stem();
    const auto folder = path.parent_path() / folder_name;
    auto document = WorldDocument{cell_size, folder_name / "persistent.scene", {}};
    auto error = std::error_code{};
    std::filesystem::create_directories(folder / "cells", error);
    if (error) return fail("cannot create " + (folder / "cells").generic_string() + ": " + error.message());
    if (auto diagnostics = save_scene_file(path.parent_path() / document.persistent, content.persistent, context); !diagnostics.empty())
        return diagnostics;
    auto written = std::set<std::filesystem::path>{};
    for (const auto& [index, cell] : content.cells) {
        const auto relative = folder_name / "cells" / (cell_name(index) + ".scene");
        if (auto diagnostics = save_scene_file(path.parent_path() / relative, cell, context); !diagnostics.empty()) return diagnostics;
        document.cells.push_back({index, relative, uint32_t(cell.entities.size())});
        written.insert(std::filesystem::path(cell_name(index) + ".scene"));
    }
    // Scenes of cells that are no longer occupied would be listed by nothing: removed.
    for (const auto& file : std::filesystem::directory_iterator(folder / "cells", error))
        if (file.path().extension() == ".scene" && !written.contains(file.path().filename())) std::filesystem::remove(file.path(), error);
    if (auto failed = replace_file(path, write_world(document), "world"); !failed.empty()) return fail(failed);
    return {};
}

SceneDocument WorldLoadResult::whole() const {
    auto result = persistent;
    for (const auto& [index, cell] : cells) result.entities.insert(result.entities.end(), cell.entities.begin(), cell.entities.end());
    return result;
}

WorldLoadResult load_world(const std::filesystem::path& path, const PropertyValidationContext& context) {
    auto result = WorldLoadResult{};
    const auto fail = [&](std::string message) {
        result.diagnostics.push_back({SceneError::io_error, path.generic_string() + ": " + std::move(message)});
        return std::move(result);
    };
    auto file = std::ifstream(path, std::ios::binary);
    if (!file) return fail("cannot open the world file");
    auto text = std::string(std::istreambuf_iterator<char>(file), {});
    auto read = read_world(text);
    if (!read) return fail(read.error);
    const auto folder = path.parent_path();
    auto persistent = load_scene_file(folder / read.document->persistent, context);
    if (!persistent) {
        result.diagnostics = std::move(persistent.diagnostics);
        return result;
    }
    result.persistent = std::move(persistent.document);
    for (const auto& cell : read.document->cells) {
        auto loaded = load_scene_file(folder / cell.scene, context);
        if (!loaded) {
            result.diagnostics = std::move(loaded.diagnostics);
            return result;
        }
        result.cells[cell.index] = std::move(loaded.document);
    }
    // A whole-world load is one scene: IDs are unique across every part.
    auto ids = std::set<EntityId>{};
    for (const auto& part : {&result.persistent}) for (const auto& entity : part->entities) ids.insert(entity.id);
    for (const auto& [index, cell] : result.cells)
        for (const auto& entity : cell.entities)
            if (!ids.insert(entity.id).second)
                return fail("entity " + std::to_string(entity.id.high) + ":" + std::to_string(entity.id.low) + " is in more than one part");
    result.world = std::move(read.document);
    return result;
}

} // namespace maya
