#pragma once
// Packaging a project (#1039, docs/projects.md#packages): its startup scene and any scenes named, with
// every asset they reach, cooked, into a macOS application bundle with the player and its shaders. A
// missing or unreadable asset fails packaging with the reason, never the game at run time.

#include "maya/assets/asset_cooker.hpp"
#include "maya/assets/package.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace maya {

struct PackageOptions {
    std::filesystem::path project; // a project file or the folder holding project.maya
    std::filesystem::path output; // the bundle to write, <Name>.app; replaced when it is already a package
    std::vector<std::filesystem::path> scenes; // besides the startup scene, relative to the content root
    std::filesystem::path player; // the player executable to bundle
    std::filesystem::path shader; // the renderer's Metal source, shipped and compiled at start
    std::string name; // the application's name; the project folder's when empty
    CookLimits limits{}; // what the player's GPU samples: Apple silicon's, ASTC included
};
struct PackageReport {
    std::string error; // why packaging failed; empty on success, when the bundle is written
    PackageManifest manifest;
    uint64_t bytes = 0; // the whole bundle's
    double milliseconds = 0;
    size_t meshes = 0, textures = 0, environments = 0, materials = 0, scripts = 0;
    explicit operator bool() const noexcept { return error.empty(); }
};

/// Packages the project. On failure nothing is written, and an earlier package at `output` stays.
PackageReport package_project(const PackageOptions& options);

} // namespace maya
