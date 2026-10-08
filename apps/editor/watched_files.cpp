// Watched files in the editor (docs/editor.md#watched-files): the files of loaded materials, textures,
// and environments, and the source images textures and environments name, are watched; an asset whose
// file or source changed outside the editor is reloaded into the registry, so every view shows it next
// frame.

#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/assets/environment_cook.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/texture_data.hpp"
#include <fstream>

namespace maya::editor {
using namespace detail;

namespace {
bool watched(AssetKind kind) {
    return kind == AssetKind::material || kind == AssetKind::texture || kind == AssetKind::environment;
}
} // namespace

EditorShell::WatchedFile EditorShell::look_at(const AssetRecord& record) const {
    const auto stamp = [](const std::optional<std::filesystem::path>& path) {
        auto result = FileStamp{};
        auto error = std::error_code{};
        result.present = path && std::filesystem::is_regular_file(*path, error);
        if (result.present) {
            result.time = std::filesystem::last_write_time(*path, error);
            result.size = std::filesystem::file_size(*path, error);
        }
        return result;
    };
    auto watched = WatchedFile{};
    // A part of an imported file: the file, and its import file, which holds the cooking settings.
    if (const auto split = split_asset_path(record.path); !split.part.empty()) {
        watched.file = stamp(m_project ? m_project->resolve(split.file) : std::nullopt);
        watched.source = stamp(m_project ? m_project->resolve(import_file_path(split.file)) : std::nullopt);
        return watched;
    }
    const auto path = m_project ? m_project->resolve(record.path) : std::nullopt;
    watched.file = stamp(path);
    // A texture's or environment's file names its source image, beside or below it.
    if (watched.file.present && (record.kind == AssetKind::texture || record.kind == AssetKind::environment)) {
        auto input = std::ifstream(*path);
        auto source = std::optional<std::filesystem::path>{};
        if (record.kind == AssetKind::texture) {
            if (const auto read = read_texture_settings(input)) source = path->parent_path() / read.settings.source;
        } else if (const auto read = read_environment_settings(input)) {
            source = path->parent_path() / read.settings.source;
        }
        if (source) watched.source = stamp(source);
    }
    return watched;
}

void EditorShell::note_watched_file(AssetId id) {
    const auto info = m_assets ? m_assets->info(id) : std::nullopt;
    if (!info || !watched(info->record.kind)) return;
    m_watched_files.insert_or_assign(id, look_at(info->record));
}

void EditorShell::check_asset_files() {
    if (!m_project || !m_assets) return;
    for (const auto& record : m_assets->records()) {
        if (!watched(record.kind)) continue;
        // An asset not loaded yet reads its file when first used; there is nothing to refresh.
        const auto info = m_assets->info(record.id);
        if (!info || info->state == AssetState::unloaded) {
            m_watched_files.erase(record.id);
            continue;
        }
        const auto now = look_at(record);
        const auto [found, added] = m_watched_files.try_emplace(record.id, now);
        if (added) continue; // first seen: the file is what was loaded
        auto& seen = found->second;
        if (now.file == seen.file && now.source == seen.source) continue;
        const auto previous = seen.error;
        seen = now;
        // Reloaded in the background (docs/assets.md#asynchronous-loading): the current version stays in
        // use until the new one is ready, and finish_reloads reports it. A newer change supersedes a reload
        // in flight.
        auto request = AssetRequest{};
        if (record.kind == AssetKind::environment) request = m_assets->request_reload(AssetRef<EnvironmentAsset>{record.id});
        else if (record.kind == AssetKind::texture) request = m_assets->request_reload(AssetRef<TextureAsset>{record.id});
        else request = m_assets->request_reload(AssetRef<MaterialAsset>{record.id});
        m_reloads[record.id] = {std::move(request), record.kind, previous};
    }
}

void EditorShell::finish_reloads() {
    if (!m_assets) {
        m_reloads.clear();
        return;
    }
    for (auto it = m_reloads.begin(); it != m_reloads.end();) {
        auto& [id, reload] = *it;
        if (!reload.request.done()) {
            ++it;
            continue;
        }
        const auto info = m_assets->info(id);
        const auto path = info ? info->record.path.generic_string() : std::string("an asset");
        auto& seen = m_watched_files[id];
        // A failed reload (a missing file, or one caught half-written) keeps the last good version. (A
        // material's request includes its maps; only the material's own failure counts here.)
        const auto diagnostic = reload.kind == AssetKind::material && info ? info->diagnostic : reload.request.diagnostic();
        if (diagnostic) {
            seen.error = diagnostic.message + "; the last version stays in use";
            if (seen.error != reload.previous_error) m_log.add(DiagnosticSource::asset, seen.error, m_frame); // once per problem
        } else {
            seen.error.clear();
            if (reload.kind == AssetKind::material && m_scene) // an edit not yet saved stays, and is shown again
                if (const auto loaded = m_assets->try_acquire(AssetRef<MaterialAsset>{id})) m_scene->material_file_changed(id, loaded.lease.value());
            m_log.add(DiagnosticSource::asset, "Reloaded " + path + (reload.previous_error.empty() ? "" : ", which loads again"), m_frame);
        }
        it = m_reloads.erase(it);
    }
}

} // namespace maya::editor
