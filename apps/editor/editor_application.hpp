#pragma once

#include "maya/core/application.hpp"
#include <filesystem>
#include <memory>
#include <optional>

namespace maya::editor {
/// The editor opens `project` (a project file or a folder containing project.maya). Without one, it
/// opens the sample project when it can find it (samples/basic_scene/project.maya).
std::unique_ptr<Application> create_editor_application(std::optional<std::filesystem::path> project = std::nullopt);
}
