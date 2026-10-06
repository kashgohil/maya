#pragma once

#include "maya/core/application.hpp"
#include "maya/renderer/render_snapshot.hpp"
#include <filesystem>
#include <memory>
#include <optional>

namespace maya::player {

struct PlayerOptions {
    /// A project file or a folder containing project.maya; the sample project when empty.
    std::optional<std::filesystem::path> project;
    /// A scene relative to the project's content root; the project's startup scene when empty.
    std::optional<std::filesystem::path> scene;
    /// Records the session, written to this file when the player stops.
    std::optional<std::filesystem::path> record;
    /// Replays this recording (its scene, with its input) instead of a scene, and checks the result.
    std::optional<std::filesystem::path> replay;
    /// Draws every physics debug view over the game (docs/physics.md#debug-views), for debugging.
    bool debug_physics = false;
    /// Shows a debug view instead of the lit image (docs/renderer.md#debug-views), for debugging.
    DebugView debug_view = DebugView::none;
};

/// Runs a saved scene: opens the project and scene, starts a play session with the built-in systems,
/// and shows the scene's first camera. Gameplay input comes from the window. Startup problems are
/// written to stderr and fail the start; a failure while playing ends the run with its reason.
std::unique_ptr<Application> create_player_application(PlayerOptions options = {});

} // namespace maya::player
