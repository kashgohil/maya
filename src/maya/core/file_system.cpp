#include "maya/core/file_system.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#endif

namespace maya {

std::vector<std::filesystem::path> FileSystem::s_roots;

namespace {

std::filesystem::path get_executable_path(int argc, char** argv) {
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec);
        if (!ec) {
            return p;
        }
    }
#endif
    if (argc > 0 && argv && argv[0] && argv[0][0] != '\0') {
        std::error_code ec;
        auto p = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
        if (!ec) {
            return p;
        }
    }
    return {};
}

void push_unique(std::vector<std::filesystem::path>& out, const std::filesystem::path& p) {
    if (p.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return;
    }
    for (const auto& existing : out) {
        std::error_code eq_ec;
        if (std::filesystem::equivalent(existing, p, eq_ec)) {
            return;
        }
    }
    out.push_back(p);
}

} // namespace

void FileSystem::initialize(int argc, char** argv) {
    s_roots.clear();

    if (const char* env = std::getenv("MAYA_RESOURCES")) {
        if (env[0] != '\0') {
            push_unique(s_roots, std::filesystem::path(env));
        }
    }

    std::filesystem::path exe = get_executable_path(argc, argv);
    if (!exe.empty()) {
        std::filesystem::path dir = exe.parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            push_unique(s_roots, dir);
            std::filesystem::path parent = dir.parent_path();
            if (parent == dir) {
                break;
            }
            dir = std::move(parent);
        }
    }

    std::error_code ec;
    push_unique(s_roots, std::filesystem::current_path(ec));
}

std::optional<std::filesystem::path> FileSystem::resolve(const std::string& path_str) {
    if (s_roots.empty()) {
        initialize(0, nullptr);
    }

    std::filesystem::path p(path_str);
    std::error_code ec;

    if (p.is_absolute()) {
        if (std::filesystem::is_regular_file(p, ec)) {
            return std::filesystem::weakly_canonical(p, ec);
        }
        return std::nullopt;
    }

    for (const auto& root : s_roots) {
        std::filesystem::path candidate = root / p;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::weakly_canonical(candidate, ec);
        }
    }
    return std::nullopt;
}

std::string FileSystem::read_text(const std::string& path) {
    auto resolved = resolve(path);
    if (!resolved) {
        return "";
    }
    std::ifstream file(*resolved);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace maya
