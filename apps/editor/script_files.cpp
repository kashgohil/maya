// Scripts in the editor (docs/scripting.md#reload): each script's last good version, watching the
// files for changes, handing changed scripts to a playing session, and opening them elsewhere.

#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/simulation/script_assets.hpp"

namespace maya::editor {
using namespace detail;

ScriptLimits EditorShell::script_limits() const {
    return m_project ? project_script_settings(m_project->settings).limits : ScriptLimits{};
}

EditorShell::ScriptVersion& EditorShell::script_version(AssetId script) {
    const auto [found, added] = m_scripts.try_emplace(script);
    if (added) read_script(script, found->second);
    return found->second;
}

void EditorShell::read_script(AssetId script, ScriptVersion& version) {
    const auto info = m_assets ? m_assets->info(script) : std::nullopt;
    const auto path = info ? info->record.path.generic_string() : "script " + id_text(script.high, script.low);
    const auto previous = version.error;
    auto error = std::error_code{};
    const auto file = m_project && info ? m_project->resolve(info->record.path) : std::nullopt;
    version.present = file && std::filesystem::is_regular_file(*file, error);
    version.stamp = version.present ? std::filesystem::last_write_time(*file, error) : std::filesystem::file_time_type{};
    version.size = version.present ? std::filesystem::file_size(*file, error) : 0;
    if (!info || info->record.kind != AssetKind::script) {
        version.error = path + " is not a script in the project's catalog";
    } else if (!version.present) {
        version.error = path + " is missing from the content folder";
    } else if (const auto loaded = m_assets->reload(AssetRef<ScriptAsset>{script}); !loaded) {
        version.error = path + ": " + loaded.diagnostic.message;
    } else if (auto described = describe_script(path, loaded.lease.value().source, script_limits()); !described) {
        version.error = std::move(described.error);
    } else {
        version.good = ScriptSource{path, loaded.lease.value().source};
        version.description = std::move(described);
        version.error.clear();
    }
    if (!version.good) version.description = ScriptDescription{{}, {}, version.error};
    // Each new problem is reported once, before anything plays; so is the recovery.
    if (!version.error.empty() && version.error != previous) m_log.add(DiagnosticSource::script, version.error, m_frame);
    else if (version.error.empty() && !previous.empty()) m_log.add(DiagnosticSource::script, path + " compiles again", m_frame);
}

const ScriptDescription* EditorShell::script_description(AssetId script) {
    if (!m_assets || !script.valid()) return nullptr;
    return &script_version(script).description;
}

std::string EditorShell::script_error(AssetId script) {
    if (!m_assets || !script.valid()) return {};
    return script_version(script).error;
}

void EditorShell::reload_script(AssetId script) {
    auto& version = script_version(script);
    const auto before = version.good ? std::optional(version.good->text) : std::nullopt;
    read_script(script, version);
    if (!m_play || !m_play_reloads) return;
    if (!version.error.empty()) {
        notice("Couldn't reload a script", version.error + "\nThe running version stays.");
        return;
    }
    if (!version.good || version.good->text == before) return; // touched, not changed
    if (auto refused = m_play_reloads->offer(script, *version.good); !refused.empty()) {
        m_log.add(DiagnosticSource::script, refused, m_frame);
        notice("Couldn't reload a script", refused + "\nThe running version stays.");
    }
}

void EditorShell::check_script_files() {
    m_script_check_timer = 0.0f;
    if (!m_project || !m_assets) return;
    for (const auto& record : m_assets->records()) {
        if (record.kind != AssetKind::script) continue;
        auto& version = script_version(record.id);
        auto error = std::error_code{};
        const auto file = m_project->resolve(record.path);
        const auto present = file && std::filesystem::is_regular_file(*file, error);
        const auto stamp = present ? std::filesystem::last_write_time(*file, error) : std::filesystem::file_time_type{};
        const auto size = present ? std::filesystem::file_size(*file, error) : 0;
        if (present != version.present || stamp != version.stamp || size != version.size) reload_script(record.id);
    }
}

void EditorShell::open_script(AssetId script) {
    const auto info = m_assets ? m_assets->info(script) : std::nullopt;
    if (!info || !m_project) return;
    const auto path = info->record.path.generic_string();
    auto error = std::error_code{};
    const auto file = m_project->resolve(info->record.path);
    auto problem = !file || !std::filesystem::is_regular_file(*file, error) ? path + " is missing from the content folder"
                   : !m_services.open_file ? std::string("This editor cannot open files in other applications")
                                           : m_services.open_file(*file);
    if (!problem.empty()) {
        m_log.add(DiagnosticSource::script, "Couldn't open " + path + ": " + problem, m_frame);
        notice("Couldn't open the script", problem);
        return;
    }
    m_log.add(DiagnosticSource::script, "Opened " + path, m_frame);
}

} // namespace maya::editor
