#include "maya/assets/residency.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/registry.hpp"
#include "maya/rhi/resource.hpp"
#include <iomanip>
#include <sstream>

namespace maya {
namespace {
constexpr size_t mib = size_t{1} << 20;
// The budgeted categories in the order of resident_setting_keys.
constexpr std::array<ResidencyCategory, 5> setting_categories = {ResidencyCategory::meshes, ResidencyCategory::textures,
                                                                 ResidencyCategory::environments, ResidencyCategory::animation,
                                                                 ResidencyCategory::cells};
} // namespace

const char* residency_category_name(ResidencyCategory category) noexcept {
    switch (category) {
    case ResidencyCategory::meshes: return "meshes";
    case ResidencyCategory::textures: return "textures";
    case ResidencyCategory::environments: return "environments";
    case ResidencyCategory::animation: return "animation";
    case ResidencyCategory::cells: return "cells";
    case ResidencyCategory::other: return "other";
    case ResidencyCategory::renderer: return "renderer";
    }
    return "other";
}

ResidencyBudgets default_residency_budgets() noexcept {
    auto budgets = ResidencyBudgets{};
    budgets.bytes[size_t(ResidencyCategory::meshes)] = 256 * mib;
    budgets.bytes[size_t(ResidencyCategory::textures)] = 768 * mib;
    budgets.bytes[size_t(ResidencyCategory::environments)] = 128 * mib;
    budgets.bytes[size_t(ResidencyCategory::animation)] = 64 * mib;
    budgets.bytes[size_t(ResidencyCategory::cells)] = 64 * mib;
    budgets.total = 1536 * mib;
    return budgets;
}

ResidencyBudgets project_residency_budgets(const ProjectSettings& project) noexcept {
    auto budgets = default_residency_budgets();
    for (size_t i = 0; i < setting_categories.size(); ++i)
        if (project.resident[i]) budgets.bytes[size_t(setting_categories[i])] = size_t(*project.resident[i]) * mib;
    if (project.resident.back()) budgets.total = size_t(*project.resident.back()) * mib;
    return budgets;
}
ResidencyReport residency_report(const AssetResidency& assets, size_t cell_bytes, size_t renderer_gpu, const RhiStats& device,
                                 std::optional<size_t> reported_gpu, std::optional<size_t> footprint) {
    auto report = ResidencyReport{};
    report.bytes = assets.bytes;
    report.leased = assets.leased_bytes;
    report.budgets = assets.budgets;
    report.bytes[size_t(ResidencyCategory::cells)].cpu += cell_bytes;
    report.bytes[size_t(ResidencyCategory::renderer)].gpu += renderer_gpu;
    report.tracked_gpu = device.buffer_bytes + device.texture_bytes;
    auto attributed = size_t{0};
    for (const auto& category : report.bytes) attributed += category.gpu;
    report.unattributed_gpu = report.tracked_gpu > attributed ? report.tracked_gpu - attributed : 0;
    report.retiring_gpu = device.pending_retirement_bytes;
    report.upload_gpu = device.upload_bytes;
    report.reported_gpu = reported_gpu;
    report.footprint = footprint;
    return report;
}
std::string residency_summary(const ResidencyReport& report) {
    auto text = std::ostringstream{};
    text << std::fixed << std::setprecision(1);
    const auto in_mib = [](size_t bytes) { return double(bytes) / double(mib); };
    for (size_t c = 0; c < residency_category_count; ++c) {
        text << (c ? ", " : "") << residency_category_name(ResidencyCategory(c)) << ' ' << in_mib(report.bytes[c].total());
        if (report.budgets.bytes[c] > 0) text << '/' << in_mib(report.budgets.bytes[c]);
        text << " MiB";
    }
    text << "; total " << in_mib(report.total());
    if (report.budgets.total > 0) text << '/' << in_mib(report.budgets.total);
    text << " MiB; GPU tracked " << in_mib(report.tracked_gpu) << " MiB (unattributed " << in_mib(report.unattributed_gpu) << ')';
    if (report.reported_gpu) text << ", Metal " << in_mib(*report.reported_gpu) << " MiB";
    if (report.footprint) text << "; footprint " << in_mib(*report.footprint) << " MiB";
    return text.str();
}
} // namespace maya
