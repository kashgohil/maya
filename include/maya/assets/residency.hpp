#pragma once
// Resident memory by category, and the budgets that hold it (#1063, docs/assets.md#residency).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace maya {
struct ProjectSettings;
struct AssetResidency;
struct RhiStats;

/// What resident memory is for. The registry's assets fall in the first four and `other` (materials and
/// scripts); a world's streamer holds `cells`; `renderer` is the views' targets, shadow maps, and tables.
enum class ResidencyCategory : uint8_t { meshes, textures, environments, animation, cells, other, renderer };
inline constexpr size_t residency_category_count = 7;
/// "meshes", "textures", "environments", "animation", "cells", "other", or "renderer".
const char* residency_category_name(ResidencyCategory category) noexcept;
/// The categories a budget can hold: content that can be released and loaded again.
constexpr bool budgeted(ResidencyCategory category) noexcept {
    return category != ResidencyCategory::other && category != ResidencyCategory::renderer;
}

/// Byte budgets by category (0: none), and the total the whole is checked against (reported, not
/// enforced: the renderer's share depends on the views' sizes).
struct ResidencyBudgets {
    std::array<size_t, residency_category_count> bytes{};
    size_t total = 0;
    size_t of(ResidencyCategory category) const noexcept { return bytes[size_t(category)]; }
};
/// W1's (#1060): 1.5 GiB in all; textures 768 MiB, meshes 256, environments 128, cells 64, skins and
/// clips 64.
ResidencyBudgets default_residency_budgets() noexcept;
/// The defaults with what the project sets (`resident_<category>` and `resident_total`, in MiB).
ResidencyBudgets project_residency_budgets(const ProjectSettings& project) noexcept;

/// A category's resident bytes.
struct ResidentBytes {
    size_t cpu = 0, gpu = 0;
    size_t total() const noexcept { return cpu + gpu; }
};
/// One resident asset, for the largest-assets list.
struct ResidentAsset {
    std::string path; // its catalog path
    ResidencyCategory category = ResidencyCategory::other;
    ResidentBytes bytes;
    bool leased = false; // held outside the registry: cannot be released now
};

/// Everything resident, by category, against the device's view of it (#1063): what the editor's
/// Residency panel, the player's log, and the benchmarks show.
struct ResidencyReport {
    std::array<ResidentBytes, residency_category_count> bytes{};
    std::array<size_t, residency_category_count> leased{}; // of those, bytes held outside the registry
    ResidencyBudgets budgets;
    size_t tracked_gpu = 0; // the device's live buffers and textures (RhiStats)
    size_t unattributed_gpu = 0; // of those, what no category accounts for
    size_t retiring_gpu = 0; // destroyed, waiting for the GPU to finish with them
    size_t upload_gpu = 0; // the device's own per-frame upload memory
    std::optional<size_t> reported_gpu; // what Metal says it has allocated
    std::optional<size_t> footprint; // the process's physical footprint
    size_t total() const noexcept {
        auto sum = size_t{0};
        for (const auto& category : bytes) sum += category.total();
        return sum;
    }
    const ResidentBytes& of(ResidencyCategory category) const noexcept { return bytes[size_t(category)]; }
};
/// Combines the registry's residency, a streamer's cells (CPU bytes of cooked content held), the renderer's
/// bytes (targets, shadow maps, tables), and the device's.
ResidencyReport residency_report(const AssetResidency& assets, size_t cell_bytes, size_t renderer_gpu, const RhiStats& device,
                                 std::optional<size_t> reported_gpu, std::optional<size_t> footprint);

/// One line: each category's bytes (against its budget), the total, and the device's view.
std::string residency_summary(const ResidencyReport& report);

/// The cook cache's default size limit (#1063): past it, opening the project removes entries no
/// current source or setting can produce.
inline constexpr uint64_t default_cook_cache_limit = uint64_t{4} << 30;
} // namespace maya
