// The Streaming panel (#1067, docs/editor.md#streaming): a world's cells by state, what putting them in and
// taking them out costs a frame, and loads in flight; in Play, the play World's streaming.
#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include <imgui_internal.h>
#include <algorithm>

namespace maya::editor {
using namespace detail;
namespace {
double in_mib(size_t bytes) { return double(bytes) / (1024.0 * 1024.0); }
} // namespace

void EditorShell::draw_streaming() {
    if (std::exchange(m_focus_streaming, false)) ImGui::SetNextWindowFocus();
    if (!begin_panel(streaming_title)) {
        ImGui::End();
        return;
    }
    m_layout.controls.push_back({"streaming.panel", ImGui::GetWindowPos(),
                                 {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}});
    const auto mono = [&](const std::string& text, bool muted = false) {
        ImGui::AlignTextToFramePadding();
        theme::mono_text(m_fonts, text.c_str(), muted);
    };
    if (!m_cells) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextWrapped("Open a world to see its cells stream. Scenes load whole.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (m_play && m_play_world) {
        // Play: the play World's cells, as the player streams them (docs/world.md#streaming).
        const auto& stats = m_play_world->stats();
        const auto& settings = m_play_world->settings();
        theme::caption(m_fonts, "PLAY", format("%zu cells active", stats.cells[size_t(CellState::active)]).c_str());
        if (theme::begin_properties("play-cells")) {
            for (size_t s = 0; s < cell_state_count; ++s) {
                if (stats.cells[s] == 0 && CellState(s) != CellState::active) continue;
                auto name = std::string(cell_state_name(CellState(s)));
                name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
                theme::property(name.c_str());
                mono(std::to_string(stats.cells[s]));
            }
            theme::property("Frame");
            mono(format("%.2f ms   longest %.2f ms   budget %.1f ms", stats.frame_ms, stats.longest_frame_ms,
                        std::chrono::duration<double, std::milli>(settings.frame_budget).count()));
            theme::property("Loads");
            mono(format("%llu started   %llu failed   %.1f MiB in flight", static_cast<unsigned long long>(stats.loads_started),
                        static_cast<unsigned long long>(stats.loads_failed), in_mib(stats.bytes_in_flight)));
            theme::property("Activations");
            mono(format("%llu in   %llu out", static_cast<unsigned long long>(stats.activations),
                        static_cast<unsigned long long>(stats.deactivations)));
            theme::property("Loaded");
            mono(format("%.1f MiB   %zu entities", in_mib(stats.bytes_loaded), stats.active_entities));
            theme::property("Radii");
            mono(format("load %.0f m   activate %.0f m   hysteresis %.0f m", settings.load_radius, settings.activate_radius, settings.hysteresis));
            theme::end_properties();
        }
        if (!stats.last_error.empty()) {
            icon_text(icon::warning, theme::color::warning, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::warning);
            ImGui::TextWrapped("%s", stats.last_error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::End();
        return;
    }
    // Editing: the editor's cells around its camera, and those kept by hand.
    auto& cells = *m_cells;
    const auto& stats = m_pinned_streaming ? *m_pinned_streaming : cells.stats();
    auto& settings = cells.settings();
    theme::caption(m_fonts, "CELLS", format("%zu of %zu loaded", stats.cells[size_t(EditorCellState::active)], cells.document().cells.size()).c_str());
    ImGui::Checkbox("Follow the camera", &settings.follow);
    m_layout.controls.push_back({"streaming.follow", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cells within the radius of the editor camera load; those it leaves go, unless changed or locked.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, 60.0f));
    auto radius = float(settings.load_radius);
    if (ImGui::DragFloat("##radius", &radius, 4.0f, 0.0f, 4096.0f, "radius %.0f m")) settings.load_radius = std::max(0.0f, radius);
    if (theme::begin_properties("cells")) {
        constexpr std::array<std::pair<EditorCellState, const char*>, 4> rows{{{EditorCellState::active, "Active"},
                                                                              {EditorCellState::loading, "Loading"},
                                                                              {EditorCellState::unloaded, "Unloaded"},
                                                                              {EditorCellState::failed, "Failed"}}};
        for (const auto& [state, name] : rows) {
            theme::property(name);
            mono(std::to_string(stats.cells[size_t(state)]), stats.cells[size_t(state)] == 0);
        }
        theme::property("Changed");
        mono(std::to_string(stats.changed) + (stats.changed ? "   kept loaded until saved" : ""), stats.changed == 0);
        theme::property("Locked by hand");
        mono(std::to_string(stats.held), stats.held == 0);
        theme::end_properties();
    }
    ImGui::Dummy({0.0f, 6.0f});
    theme::caption(m_fonts, "ACTIVATION");
    if (theme::begin_properties("activation")) {
        theme::property("Frame");
        mono(format("%.2f ms   longest %.2f ms   budget %.1f ms", stats.frame_ms, stats.longest_frame_ms,
                    std::chrono::duration<double, std::milli>(settings.frame_budget).count()));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The editor thread's time putting cells into the scene and taking them out, per frame.");
        theme::property("Cell");
        mono(format("last %.2f ms   longest %.2f ms", stats.last_activation_ms, stats.longest_activation_ms));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A cell's time going in, over all the frames it took.");
        theme::property("In flight");
        mono(format("%zu loads", stats.loads_in_flight), stats.loads_in_flight == 0);
        theme::end_properties();
    }
    if (ImGui::Button((std::string(icon::lock_simple_open) + "  Release locks").c_str())) cells.release_holds();
    m_layout.controls.push_back({"streaming.release", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    ImGui::SameLine();
    if (ImGui::Button("Reset peaks")) cells.reset_peaks();
    // Residency (#1063): the cells' share, and the panel with everything else.
    ImGui::Dummy({0.0f, 6.0f});
    const auto report = m_residency_pinned ? m_shown_residency.report : residency_report();
    const auto& bytes = report.bytes[size_t(ResidencyCategory::cells)];
    const auto budget = report.budgets.bytes[size_t(ResidencyCategory::cells)];
    theme::caption(m_fonts, "RESIDENCY", format("%.1f of %.0f MiB resident", in_mib(report.total()), in_mib(report.budgets.total)).c_str());
    if (theme::begin_properties("residency")) {
        theme::property("Cells");
        mono(format("%.1f / %.0f MiB cooked", in_mib(bytes.total()), in_mib(budget)), bytes.total() == 0);
        theme::end_properties();
    }
    if (ImGui::Button((std::string(icon::memory) + "  Open Residency").c_str())) m_focus_residency = true;
    if (!stats.last_error.empty()) {
        ImGui::Dummy({0.0f, 4.0f});
        icon_text(icon::warning, theme::color::warning, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::warning);
        ImGui::TextWrapped("%s", stats.last_error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

} // namespace maya::editor
