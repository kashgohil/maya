#pragma once
// Entities named by their path of names below another (docs/animation.md#binding): "Armature/Hips/Spine"
// is the child named Spine of the child named Hips of the child named Armature. Skins and clips name their
// joints this way, so they bind to whatever entities carry those names. A '/' or '\' in a name is escaped
// with '\'.

#include "maya/world/world.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

/// `path` with `name` appended, escaped; `name` alone when `path` is empty.
std::string append_name_path(std::string_view path, std::string_view name);
/// The names along a path, unescaped; nullopt for an empty path, an empty name, or a trailing '\'.
std::optional<std::vector<std::string>> split_name_path(std::string_view path);

/// Each path's entity below `root`, or nullopt where a name is missing. Siblings sharing a name match the
/// first in child order. Paths sharing a prefix resolve it once.
std::vector<std::optional<EntityHandle>> resolve_name_paths(const World& world, EntityHandle root,
                                                            std::span<const std::string> paths);

} // namespace maya
