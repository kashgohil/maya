// Material files in the editor (docs/editor.md#materials): the files of loaded materials are watched,
// and one changed outside the editor is reloaded into the registry, so every view shows it next frame.

#include "editor_shell.hpp"
#include "shell_detail.hpp"

namespace maya::editor {
using namespace detail;

EditorShell::WatchedFile EditorShell::look_at(const std::filesystem::path& relative) const {
    auto file = WatchedFile{};
    auto error = std::error_code{};
    const auto path = m_project ? m_project->resolve(relative) : std::nullopt;
    file.present = path && std::filesystem::is_regular_file(*path, error);
    if (file.present) {
        file.stamp = std::filesystem::last_write_time(*path, error);
        file.size = std::filesystem::file_size(*path, error);
    }
    return file;
}

void EditorShell::note_material_file(AssetId id) {
    const auto info = m_assets ? m_assets->info(id) : std::nullopt;
    if (!info || info->record.kind != AssetKind::material) return;
    m_material_files.insert_or_assign(id, look_at(info->record.path));
}

void EditorShell::check_material_files() {
    if (!m_project || !m_assets) return;
    for (const auto& record : m_assets->records()) {
        if (record.kind != AssetKind::material) continue;
        // A material not loaded yet reads its file when first used; there is nothing to refresh.
        const auto info = m_assets->info(record.id);
        if (!info || info->state == AssetState::unloaded) {
            m_material_files.erase(record.id);
            continue;
        }
        const auto now = look_at(record.path);
        const auto [found, added] = m_material_files.try_emplace(record.id, now);
        if (added) continue; // first seen: the file is what was loaded
        auto& seen = found->second;
        if (now.present == seen.present && now.stamp == seen.stamp && now.size == seen.size) continue;
        const auto previous = seen.error;
        seen = now;
        const auto path = record.path.generic_string();
        // A failed reload (a missing file, or one caught half-written) keeps the last good version.
        if (const auto loaded = m_assets->reload(AssetRef<MaterialAsset>{record.id}); !loaded) {
            seen.error = loaded.diagnostic.message + "; the last version stays in use";
            if (seen.error != previous) m_log.add(DiagnosticSource::asset, seen.error, m_frame); // once per problem
            continue;
        } else if (m_scene) { // an edit not yet saved stays, and is shown again
            m_scene->material_file_changed(record.id, loaded.lease.value());
        }
        m_log.add(DiagnosticSource::asset, "Reloaded " + path + (previous.empty() ? "" : ", which loads again"), m_frame);
    }
}

} // namespace maya::editor
