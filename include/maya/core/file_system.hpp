#pragma once

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace maya {

/// Resolves runtime asset paths against search roots (see `initialize`).
class FileSystem {
public:
    /// Call once from `main` before `Engine::initialize`. Collects roots from
    /// `MAYA_RESOURCES`, the executable path (walks parents), and the current directory.
    static void initialize(int argc, char** argv);

    /// First path that exists, or nullopt. Absolute paths are used as-is when they exist.
    static std::optional<std::filesystem::path> resolve(const std::string& relative_or_absolute);

    static std::string read_text(const std::string& path);

private:
    static std::vector<std::filesystem::path> s_roots;
};

} // namespace maya
