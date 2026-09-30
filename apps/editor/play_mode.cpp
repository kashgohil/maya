// Play mode: a separate play World for the open scene, its controls, and who gets the input.

#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/script_assets.hpp"

namespace maya::editor {
using namespace detail;
namespace {
constexpr auto play_lock = "Stop playing to edit the scene";
} // namespace

bool EditorShell::start_play() {
    if (m_play || !m_scene || !m_assets) return false;
    if (m_scene->group_open()) m_scene->end_group(); // a control mid-drag finishes its step first
    m_edit_group_open = false;
    const auto scripts = m_project ? project_script_settings(m_project->settings) : ScriptSettings{};
    auto started = PlaySession::start(m_scene->document(), asset_property_context(*m_assets),
                                      play_systems(registry_script_sources(*m_assets), scripts));
    if (!started) {
        auto reason = started.error;
        for (const auto& problem : started.diagnostics) {
            m_log.add(DiagnosticSource::play, problem.message, m_frame);
            reason += (reason.empty() ? "" : "\n") + problem.message;
        }
        if (!started.error.empty()) m_log.add(DiagnosticSource::play, started.error, m_frame);
        notice("Couldn't play the scene", reason);
        return false;
    }
    m_play = std::move(started.session);
    m_play_selection = m_scene->selection();
    m_scene->lock(play_lock);
    m_renaming.reset();
    m_snapshot.reset(); // picking and outlines belong to the authored scene
    if (const auto release = m_router.cancel()) m_capture_request = release; // stop flying the editor camera
    m_log.add(DiagnosticSource::play, "Playing" + std::string(m_play->camera() ? "" : " (the scene has no camera; showing the editor camera)"),
              m_frame);
    return true;
}

void EditorShell::stop_play() {
    if (!m_play) return;
    if (const auto release = m_router.cancel()) m_capture_request = release; // give the mouse back
    const auto ticks = m_play->clock().tick();
    m_play.reset(); // the play World, its systems, and the snapshot's leases on it go now
    m_snapshot.reset();
    m_frame_problems.clear();
    if (m_scene) {
        m_scene->unlock();
        m_scene->set_selection(m_play_selection); // entities that no longer exist are dropped
    }
    m_play_selection.clear();
    m_log.add(DiagnosticSource::play, "Stopped after " + std::to_string(ticks) + " ticks", m_frame);
}

void EditorShell::toggle_pause() {
    if (!m_play) return;
    if (m_play->clock().paused()) m_play->clock().resume();
    else m_play->clock().pause();
}

void EditorShell::step_play() {
    if (m_play && m_play->clock().paused()) m_play->clock().step();
}

void EditorShell::set_game_view(bool game) noexcept {
    if (game == m_game_view) return;
    m_game_view = game;
    if (const auto release = m_router.cancel()) m_capture_request = release;
    if (m_play) m_play->input().release_all();
}

void EditorShell::update_play(const RoutedInput& routed, float delta_time) {
    if (!m_play) return;
    // Only events the router gave the game reach it; everything else stays with the editor.
    m_play->input().feed(routed.game);
    if (routed.game_ended) m_play->input().release_all();
    const auto frame = m_play->update(delta_time);
    // Scripts report without stopping play: logs to Diagnostics; a failed instance also as a notice.
    for (const auto& message : frame.messages) {
        m_log.add(DiagnosticSource::script, message.text, m_frame);
        if (message.level == SimulationMessage::Level::error) notice("A script stopped", message.text);
    }
    if (!frame.error.empty()) {
        m_log.add(DiagnosticSource::play, frame.error, m_frame);
        stop_play();
        notice("Play stopped", frame.error);
    }
}

void EditorShell::draw_view_toggle() {
    // In the viewport's corner, where the editing tools are while not playing.
    ImGui::SetCursorScreenPos({m_layout.viewport_min.x + 10.0f, m_layout.viewport_min.y + 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {9.0f, 5.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0f, 2.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    auto hovered = false;
    const auto option = [&](const char* label, const char* key, bool active, const char* tip) {
        ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::color::rgb(0x7B8CFF, 60) : theme::color::rgb(0x0B0C0E, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::rgb(0x26282E, 230));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::color::text : theme::color::muted);
        const auto pressed = ImGui::Button(label);
        ImGui::PopStyleColor(3);
        m_layout.controls.push_back({key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tip);
            hovered = true;
        }
        ImGui::SameLine();
        return pressed;
    };
    const auto has_camera = m_play && m_play->camera();
    ImGui::BeginDisabled(!has_camera);
    if (option((std::string(icon::game_controller) + "  Game").c_str(), "view.game", m_game_view && has_camera,
               has_camera ? "Through the scene's camera; click the view to play" : "The scene has no camera"))
        set_game_view(true);
    ImGui::EndDisabled();
    if (option((std::string(icon::cube_focus) + "  Scene").c_str(), "view.scene", !m_game_view || !has_camera,
               "Through the editor camera; hold the right button to fly"))
        set_game_view(false);
    ImGui::PopStyleVar(3);
    ImGui::NewLine();
    if (hovered) m_viewport_hovered = false; // a click on the toggle is not a click on the game
}

void EditorShell::draw_play_controls() {
    const auto playing = m_play != nullptr;
    const auto paused = playing && m_play->clock().paused();
    const auto button = [&](const char* glyph, const char* id, bool enabled, bool active, const std::string& tip) {
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::color::rgb(0x7B8CFF, 40) : 0u);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::color::accent : enabled ? theme::color::muted : theme::color::faint);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f, 4.0f});
        ImGui::BeginDisabled(!enabled);
        const auto pressed = ImGui::Button(glyph);
        ImGui::EndDisabled();
        m_layout.controls.push_back({std::string("play.") + id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip.c_str());
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        return pressed;
    };
    if (button(playing ? icon::stop : icon::play, "play", m_scene != nullptr, playing,
               playing ? "Stop   \xE2\x8C\x98P" : "Play   \xE2\x8C\x98P")) {
        if (playing) stop_play();
        else start_play();
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (button(icon::pause, "pause", playing, paused, paused ? "Resume   \xE2\x87\xA7\xE2\x8C\x98P" : "Pause   \xE2\x87\xA7\xE2\x8C\x98P"))
        toggle_pause();
    ImGui::SameLine(0.0f, 2.0f);
    if (button(icon::skip_forward, "step", paused, false, "Step one tick   \xE2\x8C\xA5\xE2\x8C\x98P")) step_play();
}

} // namespace maya::editor
