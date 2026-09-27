#pragma once

#include "maya/core/application.hpp"
#include "maya/rhi/resource.hpp"
#include <memory>
#include <string>
#include <vector>

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

/// An application's own arguments, separated from the host's options. Arguments that start with "-"
/// belong to the host, and so does the frame count after --smoke; the rest are positional.
struct LaunchArguments {
    std::vector<std::string> positional;
    std::vector<char*> host; // for run_desktop: the program name, then the host's options, then null
    int host_count() const noexcept { return static_cast<int>(host.size()) - 1; }
};
LaunchArguments split_arguments(int argc, char** argv);

/// Desktop host, separate from runtime and game/editor code. Main thread only.
/// Supports --help and --smoke [positive frame count]; returns nonzero on failure. Closing the window
/// asks Application::on_close_requested, except in smoke runs.
int run_desktop(int argc, char** argv, std::unique_ptr<Application> application,
    const DesktopOptions& options = {});

} // namespace maya
