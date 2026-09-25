#pragma once

#include "maya/core/application.hpp"
#include "maya/rhi/resource.hpp"
#include <memory>
#include <string>

namespace maya {

struct DesktopOptions {
    std::string title = "Maya";
    int width = 1280;
    int height = 720;
    bool capture_cursor = false;
    /// Escape closes the window. Editors turn this off so Escape can cancel tools and text entry.
    bool escape_closes = true;
    /// The application's own arguments for --help, e.g. "[project]"; the application removes them
    /// from argv before calling run_desktop.
    std::string usage;
    DeviceOptions device{};
};

/// Desktop host, separate from runtime and game/editor code. Main thread only.
/// Supports --help and --smoke [positive frame count]; returns nonzero on failure. Closing the window
/// asks Application::on_close_requested, except in smoke runs.
int run_desktop(int argc, char** argv, std::unique_ptr<Application> application,
    const DesktopOptions& options = {});

} // namespace maya
