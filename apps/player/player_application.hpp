#pragma once

#include "maya/core/application.hpp"
#include <filesystem>
#include <memory>
#include <optional>

namespace maya::player {

struct PlayerOptions {
    /// A project file or a folder containing project.maya; the sample project when empty.
    std::optional<std::filesystem::path> project;
    /// A scene relative to the project's content root; the project's startup scene when empty.
    std::optional<std::filesystem::path> scene;
};

/// Runs a saved scene: opens the project and scene, starts a play session with the built-in systems,
/// and shows the scene's first camera. Gameplay input comes from the window. Startup problems are
/// written to stderr and fail the start; a failure while playing ends the run with its reason.
std::unique_ptr<Application> create_player_application(PlayerOptions options = {});

} // namespace maya::player
