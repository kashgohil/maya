// maya_package: packages a project as a standalone macOS application (#1039, docs/projects.md#packages).
#include "maya/core/file_system.hpp"
#include "maya/package/packager.hpp"
#include <cstdio>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    maya::FileSystem::initialize(argc, argv);
    const auto usage = [] {
        std::cerr << "Usage: maya_package <project> <output.app> [--scene <path>]... [--name <name>] [--player <path>] [--shader <path>]\n";
        return 2;
    };
    auto options = maya::PackageOptions{};
    auto positional = std::vector<std::string>{};
    for (int i = 1; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--scene" || argument == "--name" || argument == "--player" || argument == "--shader") {
            if (i + 1 >= argc) return usage();
            const auto value = std::string(argv[++i]);
            if (argument == "--scene") options.scenes.emplace_back(value);
            else if (argument == "--name") options.name = value;
            else if (argument == "--player") options.player = value;
            else options.shader = value;
        } else if (argument.starts_with("--")) {
            return usage();
        } else {
            positional.emplace_back(argument);
        }
    }
    if (positional.size() != 2) return usage();
    options.project = std::filesystem::absolute(positional[0]);
    options.output = std::filesystem::absolute(positional[1]);
    // The player built beside this tool, and the shader the development build finds.
    if (options.player.empty()) {
        if (const auto here = maya::FileSystem::resolve(argv[0])) options.player = here->parent_path() / "maya_player";
    }
    if (options.shader.empty())
        if (const auto shader = maya::FileSystem::resolve("resources/shaders/metal/renderer.metal")) options.shader = *shader;
    const auto report = maya::package_project(options);
    if (!report) {
        std::cerr << "[Package] " << report.error << '\n';
        return 1;
    }
    const auto& manifest = report.manifest;
    std::cout << "[Package] " << manifest.name << ": " << manifest.scenes.size() << " scene(s), " << report.meshes << " meshes, "
              << report.textures << " textures, " << report.environments << " environments, " << report.materials << " materials, "
              << report.scripts << " scripts\n";
    char size[64];
    std::snprintf(size, sizeof(size), "%.1f MiB in %.0f ms", double(report.bytes) / (1024.0 * 1024.0), report.milliseconds);
    std::cout << "[Package] " << options.output.string() << ": " << size << ", content " << maya::sha256_text(manifest.content) << '\n';
    return 0;
}
