// The Residency panel (#1063, docs/editor.md#residency): resident memory by category against its budget,
// the device's view of it, the largest resident assets, who holds leases, and what loading and releasing did.
#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include <imgui_internal.h>
#include <algorithm>

namespace maya::editor {
using namespace detail;
namespace {
double in_mib(size_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

/// A bar across the control column: `used` of `budget`, in the warning color when over it.
void budget_bar(const theme::Fonts& fonts, size_t used, size_t budget, const std::string& text) {
    const auto width = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
    const auto height = ImGui::GetFrameHeight() - 6.0f;
    ImGui::AlignTextToFramePadding();
    const auto at = ImGui::GetCursorScreenPos();
    auto* list = ImGui::GetWindowDrawList();
    const auto top = at.y + 3.0f;
    list->AddRectFilled({at.x, top}, {at.x + width, top + height}, theme::color::surface, 3.0f);
    if (budget > 0) {
        const auto fraction = std::min(double(used) / double(budget), 1.0);
        const auto color = used > budget ? theme::color::warning : theme::color::accent;
        if (fraction > 0.0) list->AddRectFilled({at.x, top}, {at.x + float(fraction * width), top + height}, color & 0x8CFFFFFFu, 3.0f);
    }
    ImGui::PushFont(fonts.mono); // figures are monospaced, as elsewhere
    const auto size = ImGui::CalcTextSize(text.c_str());
    list->AddText({at.x + width - size.x - 8.0f, at.y + (ImGui::GetFrameHeight() - size.y) * 0.5f}, theme::color::text, text.c_str());
    ImGui::PopFont();
    ImGui::Dummy({width, ImGui::GetFrameHeight()});
}
} // namespace

ResidencyReport EditorShell::residency_report() const {
    const auto renderer = m_renderer.shadow_bytes() + m_renderer.table_bytes() + m_viewport.gpu_bytes() + m_thumbnails.gpu_bytes() +
                          m_ui.gpu_bytes();
    const auto memory = process_memory();
    return maya::residency_report(m_assets ? m_assets->residency() : AssetResidency{}, 0, renderer, m_device.stats(),
                                  m_device.reported_memory(), memory ? std::optional(size_t(memory->footprint)) : std::nullopt);
}

void EditorShell::draw_residency() {
    if (std::exchange(m_focus_residency, false)) ImGui::SetNextWindowFocus();
    if (!begin_panel(residency_title)) {
        ImGui::End();
        return;
    }
    m_layout.controls.push_back({"residency.panel", ImGui::GetWindowPos(),
                                 {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}});
    auto& shown = m_shown_residency;
    if (!m_residency_pinned && (m_residency_age += ImGui::GetIO().DeltaTime) >= 0.25f) {
        m_residency_age = 0.0f;
        shown.loading = m_load_stats.in_flight;
        shown.ready = m_load_stats.prepared;
        shown.report = residency_report();
        shown.largest = m_assets ? m_assets->largest(8) : std::vector<ResidentAsset>{};
        shown.release = m_assets ? m_assets->release_stats() : AssetReleaseStats{};
        shown.viewport_versions = shown.viewport_bytes = 0;
        if (m_snapshot) {
            for (const auto& mesh : m_snapshot->meshes) shown.viewport_bytes += mesh.value().mesh().gpu_bytes();
            for (const auto& texture : m_snapshot->textures) shown.viewport_bytes += texture.value().gpu_bytes();
            if (m_snapshot->environment && m_snapshot->environment->asset) shown.viewport_bytes += m_snapshot->environment->asset.value().gpu_bytes();
            shown.viewport_versions = m_snapshot->meshes.size() + m_snapshot->textures.size() +
                                      (m_snapshot->environment && m_snapshot->environment->asset ? 1 : 0);
        }
    }
    const auto& report = shown.report;
    const auto mono = [&](const std::string& text, bool muted = false) {
        ImGui::AlignTextToFramePadding();
        theme::mono_text(m_fonts, text.c_str(), muted);
    };

    // First, where it is always in view: releasing what nothing uses.
    const auto release = ImGui::Button((std::string(icon::broom) + "  Release unused").c_str());
    m_layout.controls.push_back({"residency.release", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Releases every resident version that nothing has used since this frame.");
    if (release) release_unused_later("asking to release unused content");
    ImGui::Dummy({0.0f, 2.0f});

    // What is resident, by category, against each budget.
    theme::caption(m_fonts, "RESIDENT", format("%.1f of %.0f MiB", in_mib(report.total()), in_mib(report.budgets.total)).c_str());
    if (theme::begin_properties("resident")) {
        for (size_t c = 0; c < residency_category_count; ++c) {
            const auto category = ResidencyCategory(c);
            const auto& bytes = report.bytes[c];
            const auto budget = report.budgets.bytes[c];
            auto name = std::string(residency_category_name(category));
            name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
            theme::property(name.c_str());
            const auto text = budget > 0 ? format("%.1f / %.0f MiB", in_mib(bytes.total()), in_mib(budget))
                                         : format("%.1f MiB", in_mib(bytes.total()));
            budget_bar(m_fonts, bytes.total(), budget, text);
            m_layout.controls.push_back({"residency." + std::string(residency_category_name(category)), ImGui::GetItemRectMin(),
                                         ImGui::GetItemRectMax()});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("GPU %.1f MiB, CPU %.1f MiB; %.1f MiB of it in use (leased).%s", in_mib(bytes.gpu), in_mib(bytes.cpu),
                                  in_mib(report.leased[c]),
                                  category == ResidencyCategory::renderer ? " The renderer's shadow maps and tables, the viewport, and the "
                                                                            "editor's fonts and thumbnails: sized by the views, never budgeted."
                                  : !budgeted(category) ? " Materials and scripts: small, never budgeted." : "");
        }
        theme::end_properties();
    }

    // The device's view: what it tracks, what no category explains, and what the platform reports.
    ImGui::Dummy({0.0f, 6.0f});
    theme::caption(m_fonts, "DEVICE");
    if (theme::begin_properties("device")) {
        theme::property("Tracked");
        mono(format("GPU %.1f MiB   unattributed %.1f MiB", in_mib(report.tracked_gpu), in_mib(report.unattributed_gpu)));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The device's live buffers and textures, from their descriptors; unattributed is what no category accounts for.");
        theme::property("Transient");
        mono(format("retiring %.1f MiB   upload %.1f MiB", in_mib(report.retiring_gpu), in_mib(report.upload_gpu)));
        theme::property("Reported");
        mono(format("Metal %s   process %s", report.reported_gpu ? format("%.1f MiB", in_mib(*report.reported_gpu)).c_str() : "unavailable",
                    report.footprint ? format("%.1f MiB", in_mib(*report.footprint)).c_str() : "unavailable"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Platform-reported: the device's allocated size and the process's physical footprint. On unified memory "
                              "they overlap; they are not added together.");
        theme::end_properties();
    }

    // The largest resident versions, and whether something holds them.
    ImGui::Dummy({0.0f, 6.0f});
    theme::caption(m_fonts, "LARGEST");
    if (shown.largest.empty()) {
        mono("Nothing is resident", true);
    } else if (ImGui::BeginTable("largest", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("asset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("use", ImGuiTableColumnFlags_WidthFixed);
        for (const auto& asset : shown.largest) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            mono(asset.path); // paths are monospaced, as IDs and figures are
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", residency_category_name(asset.category), asset.path.c_str());
            ImGui::TableNextColumn();
            mono(format("%.1f MiB", in_mib(asset.bytes.total())));
            ImGui::TableNextColumn();
            mono(asset.leased ? "in use" : "unused", !asset.leased);
        }
        ImGui::EndTable();
    }

    // Who holds leases: the viewport's snapshot, and everything else (a play session, tools, thumbnails).
    ImGui::Dummy({0.0f, 6.0f});
    theme::caption(m_fonts, "LEASES");
    if (theme::begin_properties("leases")) {
        auto leased = size_t{0};
        for (const auto bytes : report.leased) leased += bytes;
        theme::property("Viewport");
        mono(format("%zu versions   %.1f MiB", shown.viewport_versions, in_mib(shown.viewport_bytes)));
        theme::property("Elsewhere");
        mono(format("%.1f MiB", in_mib(leased > shown.viewport_bytes ? leased - shown.viewport_bytes : 0)));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Held by a play session's scripts and clips, editing tools, and thumbnails being drawn.");
        theme::end_properties();
    }

    // Loading and releasing.
    ImGui::Dummy({0.0f, 6.0f});
    theme::caption(m_fonts, "RELEASING");
    if (theme::begin_properties("releasing")) {
        theme::property("Loading");
        mono(format("%zu in flight   %zu ready", shown.loading, shown.ready));
        theme::property("Released");
        mono(format("%llu   %llu by budgets   %.2f ms", static_cast<unsigned long long>(shown.release.released),
                    static_cast<unsigned long long>(shown.release.released_for_budget), shown.release.last_ms));
        auto over = std::string{};
        for (size_t c = 0; c < residency_category_count; ++c)
            if (shown.release.over_budget[c]) over += std::string(over.empty() ? "" : ", ") + residency_category_name(ResidencyCategory(c));
        if (!over.empty()) {
            theme::property("Over budget");
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::warning);
            mono(over + ": all in use");
            ImGui::PopStyleColor();
        }
        theme::end_properties();
    }
    ImGui::End();
}

} // namespace maya::editor
