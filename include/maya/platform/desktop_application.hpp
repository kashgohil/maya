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
    DeviceOptions device{};
};

/// Desktop host, separate from runtime and game/editor code. Main thread only.
/// Supports --help and --smoke [positive frame count]; returns nonzero on failure.
int run_desktop(int argc, char** argv, std::unique_ptr<Application> application,
    const DesktopOptions& options = {});

} // namespace maya
