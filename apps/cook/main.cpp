// maya_cook: maintains a project's cook cache (#1063, docs/assets.md#cook-cache). `--prune` removes the
// entries no current source or setting can produce; `--dry-run` only reports what it would remove.
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/project.hpp"
#include "maya/streaming/world_streamer.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
std::string mib(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MiB", double(bytes) / (1024.0 * 1024.0));
    return text;
}
} // namespace

int main(int argc, char** argv) {
    const auto usage = [] {
        std::cerr << "Usage: maya_cook <project> --prune [--dry-run]\n";
        return 2;
    };
    auto prune = false, dry_run = false;
    auto positional = std::vector<std::string>{};
    for (int i = 1; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--prune") prune = true;
        else if (argument == "--dry-run") dry_run = true;
        else if (argument.starts_with("--")) return usage();
        else positional.emplace_back(argument);
    }
    if (positional.size() != 1 || !prune) return usage();
    const auto opened = maya::open_project(std::filesystem::absolute(positional[0]));
    if (!opened) {
        std::cerr << "[Cook] " << opened.error << '\n';
        return 1;
    }
    const auto& project = opened.project;
    auto file = std::ifstream(project.catalog);
    const auto catalog = file ? maya::read_asset_catalog(file) : maya::AssetCatalogResult{{}, {maya::AssetError::missing_file, {}}};
    if (!catalog) {
        std::cerr << "[Cook] cannot read the asset catalog " << project.catalog.string() << '\n';
        return 1;
    }
    // Apple silicon's limits, as packaging cooks: what this machine's editor and player read.
    const auto cache = std::make_shared<maya::CookCache>(maya::cook_cache_folder(project));
    const auto before = cache->usage();
    const auto keys = maya::reachable_cook_keys(project, catalog.records, cache);
    const auto pruning = cache->prune(keys, dry_run);
    std::cout << "[Cook] " << cache->folder().string() << ": " << before.entries << " entries, " << mib(before.bytes) << '\n'
              << "[Cook] " << (dry_run ? "would keep " : "kept ") << pruning.kept << " (" << mib(pruning.kept_bytes) << "), "
              << (dry_run ? "would remove " : "removed ") << pruning.removed << " (" << mib(pruning.removed_bytes) << ") that no source or "
              << "setting of the project produces now\n";
    for (const auto& error : pruning.errors) std::cerr << "[Cook] " << error << '\n';
    return pruning.errors.empty() ? 0 : 1;
}
