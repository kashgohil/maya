#pragma once

#include <string>

namespace maya {

/// How this binary was built, recorded with every measurement. Generated at build time
/// (cmake/build_info.cmake), so the revision is the one the binary was built from.
struct BuildInfo {
    std::string revision; // git describe --always --dirty, or "unknown"
    std::string build_type; // CMAKE_BUILD_TYPE, or "unspecified"
    std::string sanitizers; // "none" unless sanitizers were enabled
    std::string compiler;
};
BuildInfo build_info();

} // namespace maya
