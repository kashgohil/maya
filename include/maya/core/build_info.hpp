#pragma once

#include <string>

namespace maya {

/// How this binary was built, recorded with every measurement and play recording. Generated at build
/// time (cmake/build_info.cmake), so the revision is the one the binary was built from.
struct BuildInfo {
    /// git describe --always --dirty; a dirty tree adds a hash of its uncommitted changes, so two
    /// different dirty builds differ. "unknown" without git.
    std::string revision;
    std::string build_type; // CMAKE_BUILD_TYPE, or "unspecified"
    std::string sanitizers; // "none" unless sanitizers were enabled
    std::string compiler;
};
BuildInfo build_info();

} // namespace maya
