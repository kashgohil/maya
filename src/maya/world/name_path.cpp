#include "maya/world/name_path.hpp"
#include "maya/world/components.hpp"
#include <unordered_map>

namespace maya {

std::string append_name_path(std::string_view path, std::string_view name) {
    auto out = std::string(path);
    if (!out.empty()) out += '/';
    for (const auto c : name) {
        if (c == '/' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::optional<std::vector<std::string>> split_name_path(std::string_view path) {
    auto names = std::vector<std::string>{};
    auto name = std::string{};
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '\\') {
            if (++i == path.size()) return std::nullopt;
            name += path[i];
        } else if (path[i] == '/') {
            if (name.empty()) return std::nullopt;
            names.push_back(std::move(name));
            name.clear();
        } else {
            name += path[i];
        }
    }
    if (name.empty()) return std::nullopt;
    names.push_back(std::move(name));
    return names;
}

std::vector<std::optional<EntityHandle>> resolve_name_paths(const World& world, EntityHandle root,
                                                            std::span<const std::string> paths) {
    auto result = std::vector<std::optional<EntityHandle>>(paths.size());
    if (!world.alive(root)) return result;
    // Each escaped prefix resolved so far: "Armature/Hips" once for every joint below it.
    auto known = std::unordered_map<std::string, std::optional<EntityHandle>>{};
    const auto child_named = [&](EntityHandle parent, const std::string& name) -> std::optional<EntityHandle> {
        for (const auto child : world.children(parent)) {
            auto match = false;
            world.with<NameComponent>(child, [&](const NameComponent& value) { match = value.value == name; });
            if (match) return child;
        }
        return std::nullopt;
    };
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto names = split_name_path(paths[i]);
        if (!names) continue;
        auto at = std::optional<EntityHandle>{root};
        auto prefix = std::string{};
        for (const auto& name : *names) {
            prefix = append_name_path(prefix, name);
            if (const auto found = known.find(prefix); found != known.end()) {
                at = found->second;
            } else {
                at = child_named(*at, name);
                known.emplace(prefix, at);
            }
            if (!at) break;
        }
        result[i] = at;
    }
    return result;
}

} // namespace maya
