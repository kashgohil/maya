// Play mode: a separate play World for the open scene, its controls, and who gets the input.

#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/project_recording.hpp"
#include "maya/simulation/script_assets.hpp"
#include <fstream>

namespace maya::editor {
using namespace detail;
namespace {
constexpr auto play_lock = "Stop playing to edit the scene";
} // namespace

bool EditorShell::start_play(bool record) {
    if (m_play || !m_scene || !m_assets) return false;
    return begin_play(m_scene->document(), nullptr, record);
}

// The assets a scene plays with, hashed; scripts by the last version that compiled, which is what plays.
std::vector<RecordedAsset> EditorShell::played_assets(const SceneDocument& document) {
    return recorded_assets(*m_assets, document, [this](AssetId script) -> std::optional<std::string> {
        const auto& version = script_version(script);
        return version.good ? std::optional(version.good->text) : std::nullopt;
    });
}

bool EditorShell::replay_last_play() {
    if (m_play || !m_scene || !m_assets || !m_last_recording) return false;
    auto read = read_scene(m_last_recording->scene, asset_property_context(*m_assets));
    if (!read) {
        notice("Couldn't replay", "The recorded scene no longer loads: " + (read.diagnostics.empty() ? std::string() : read.diagnostics.front().message));
        return false;
    }
    if (auto refused = replay_refusal(*m_last_recording, played_assets(read.document), {}); !refused.empty()) {
        m_log.add(DiagnosticSource::play, "Couldn't replay: " + refused, m_frame);
        notice("Couldn't replay", refused);
        return false;
    }
    const auto recording = *m_last_recording; // a replay records nothing
    return begin_play(read.document, &recording, false);
}

std::string EditorShell::save_recording() {
    if (!m_last_recording || !m_project) return "Nothing has been recorded yet";
    const auto stem = m_scene_path.empty() ? std::string("untitled") : m_scene_path.stem().string();
    const auto path = m_project->content_root / "recordings" / (stem + ".recording");
    auto error = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), error);
    auto file = std::ofstream(path);
    write_recording(file, *m_last_recording);
    file.flush();
    if (error || !file) {
        const auto reason = "Cannot write " + path.string();
        m_log.add(DiagnosticSource::play, reason, m_frame);
        notice("Couldn't save the recording", reason);
        return reason;
    }
    m_log.add(DiagnosticSource::play, "Saved the recording of " + std::to_string(m_last_recording->inputs.size()) + " ticks to " +
        m_project->relative(path).generic_string(), m_frame);
    return {};
}

bool EditorShell::begin_play(const SceneDocument& document, const PlayRecording* replay, bool record) {
    if (m_scene->group_open()) m_scene->end_group(); // a control mid-drag finishes its step first
    m_edit_group_open = false;
    // Scripts play their last good versions; changed files reach the session through the reloads.
    auto scripts = m_project ? project_script_settings(m_project->settings) : ScriptSettings{};
    scripts.reloads = std::make_shared<ScriptReloads>(scripts.limits);
    if (replay) scripts.seed = replay->seed;
    const auto sources = [this](AssetId script) -> ScriptSourceResult {
        const auto& version = script_version(script);
        if (version.good) return {*version.good, {}};
        // Never compiled: the session gets the file as it is and reports why, under the script's name.
        return m_assets ? registry_script_sources(*m_assets)(script) : ScriptSourceResult{std::nullopt, version.error};
    };
    auto started = PlaySession::start(document, asset_property_context(*m_assets), play_systems(sources, scripts));
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
    m_play_reloads = scripts.reloads;
    // A replay checks itself against its recording; a recorded Play records.
    m_play_recording.reset();
    m_replay_final.reset();
    if (replay) {
        m_play->start_replay(replay->inputs, replay->checkpoints);
        m_replay_final = replay->final_state;
    } else if (record) {
        const auto name = m_scene_path.empty() || !m_project ? std::string("an unsaved scene") : m_project->relative(m_scene_path).generic_string();
        m_play_recording = begin_recording(name, document, played_assets(document), scripts.seed, {}, {});
        m_play->start_recording();
    }
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
    if (m_play_recording) {
        finish_recording(*m_play_recording, *m_play);
        if (m_play_reloads)
            for (const auto& reload : m_play_reloads->applied()) m_play_recording->reloads.push_back({reload.tick, reload.script, reload.name});
        m_last_recording = std::move(m_play_recording);
        m_play_recording.reset();
    }
    m_play.reset(); // the play World, its systems, and the snapshot's leases on it go now
    m_play_reloads.reset();
    m_shown_physics.reset();
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
    // Diagnostics' physics counts, four times a second, whether or not the panel is showing.
    m_physics_age += delta_time;
    if (!m_shown_physics || m_physics_age >= 0.25f) {
        m_shown_physics = m_play->physics().stats();
        m_physics_age = 0.0f;
    }
    // Scripts report without stopping play: logs to Diagnostics; a failed instance also as a notice.
    for (const auto& message : frame.messages) {
        m_log.add(DiagnosticSource::script, message.text, m_frame);
        if (message.level == SimulationMessage::Level::error) notice("A script stopped", message.text);
    }
    if (!frame.error.empty()) {
        m_log.add(DiagnosticSource::play, frame.error, m_frame);
        stop_play();
        notice("Play stopped", frame.error);
        return;
    }
    // A replay that has run all its ticks says whether it matched, and stays paused on its last frame.
    const auto& replay = m_play->replay();
    if (replay.replaying && replay.finished && m_replay_final) {
        const auto final = *m_replay_final;
        m_replay_final.reset();
        if (replay.first_difference || m_play->full_state_hash() != final) {
            const auto where = replay.first_difference ? "at tick " + std::to_string(*replay.first_difference) : std::string("in its final state");
            m_log.add(DiagnosticSource::play, "The replay differs from the recording " + where, m_frame);
            notice("The replay differs", "The replay differs from the recording " + where + ".");
        } else {
            m_log.add(DiagnosticSource::play, "The replay matches the recording: " + std::to_string(m_play->clock().tick()) + " ticks, " +
                std::to_string(replay.checked) + " checkpoints, and the final state", m_frame);
        }
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
    ImGui::SameLine(0.0f, 8.0f);
    if (option(icon::eye, "tool.physics-debug", m_preferences.physics_debug.any(), "Physics debug views")) ImGui::OpenPopup("physics-debug");
    ImGui::PopStyleVar(3);
    ImGui::NewLine();
    draw_physics_debug_menu();
    if (ImGui::IsPopupOpen("physics-debug")) hovered = true; // the menu's clicks are not the game's
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
