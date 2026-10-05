// Importing models in the editor (docs/editor.md#importing-models): glTF files dropped on the window, or
// chosen from the Scene menu, are imported into the project (import_gltf), and a dropped one is placed in
// the open scene. An imported scene file drags into the viewport like a mesh.

#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/import/gltf_import.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace maya::editor {
using namespace detail;

namespace {
std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}
bool gltf_file(const std::filesystem::path& path) {
    const auto extension = lowercase(path.extension().string());
    return extension == ".gltf" || extension == ".glb";
}
/// A label from an import identity: "Panel/0" -> "Panel", "Panel/1" -> "Panel 1", "Albedo/color" -> "Albedo".
std::string identity_label(const std::string& identity, AssetKind kind) {
    const auto slash = identity.rfind('/');
    if (slash == std::string::npos) return identity;
    const auto name = identity.substr(0, slash), last = identity.substr(slash + 1);
    return kind == AssetKind::mesh && last != "0" ? name + " " + last : name;
}
} // namespace

std::string EditorShell::asset_label(const AssetRecord& record) const {
    if (const auto found = m_part_labels.find(record.id); found != m_part_labels.end()) return found->second;
    const auto split = split_asset_path(record.path);
    return split.part.empty() ? record.path.stem().string() : split.file.stem().string() + " " + split.part;
}

void EditorShell::read_part_labels() {
    m_part_labels.clear();
    // Imported sources, for watching: each model file with an import file beside it.
    m_imported_sources.clear();
    if (m_project)
        for (const auto& model : m_model_files) {
            const auto file = m_project->resolve(import_file_path(model));
            auto input = file ? std::ifstream(*file) : std::ifstream{};
            if (!input) continue;
            const auto imported = read_import_file(input);
            if (!imported) continue;
            // The source, its import file (whose settings may be edited), and the files the source names.
            auto watched = ImportedSource{model, {model, import_file_path(model)}, {}};
            for (const auto& named : imported.file.files) watched.files.push_back((model.parent_path() / named).lexically_normal());
            watched.stamps = stamp_files(watched.files);
            m_imported_sources.push_back(std::move(watched));
        }
    if (!m_project || !m_assets) return;
    auto read = std::set<std::filesystem::path>{};
    for (const auto& record : m_assets->records()) {
        const auto split = split_asset_path(record.path);
        if (split.part.empty() || !read.insert(split.file).second) continue;
        const auto file = m_project->resolve(import_file_path(split.file));
        auto input = file ? std::ifstream(*file) : std::ifstream{};
        if (!input) continue;
        const auto imported = read_import_file(input);
        if (!imported) continue;
        for (const auto& mesh : imported.file.meshes) m_part_labels.insert_or_assign(mesh.id, identity_label(mesh.identity, AssetKind::mesh));
        for (const auto& texture : imported.file.textures)
            m_part_labels.insert_or_assign(texture.id, identity_label(texture.identity, AssetKind::texture));
    }
}

GltfImportResult EditorShell::import_model(const std::filesystem::path& path, bool place, bool notify) {
    if (!m_project) return {.errors = {{"", "No project is open"}}};
    const auto shown = m_project->relative(path).empty() ? path.generic_string() : m_project->relative(path).generic_string();
    const auto result = import_gltf(*m_project, path);
    if (!result) {
        auto message = std::string{};
        for (const auto& problem : result.errors) message += gltf_problem_text(problem) + "\n";
        m_log.add(DiagnosticSource::asset, "Couldn't import " + shown + ": " + gltf_problem_text(result.errors.front()), m_frame);
        if (notify) notice("Couldn't import " + shown, message + "Nothing was changed.");
        return result;
    }
    const auto count = [&](AssetKind kind) { return std::ranges::count(result.records, kind, &AssetRecord::kind); };
    m_log.add(DiagnosticSource::asset, "Imported " + shown + ": " + std::to_string(count(AssetKind::mesh)) + " meshes, " +
        std::to_string(count(AssetKind::texture)) + " textures, " + std::to_string(count(AssetKind::material)) + " materials, scene " +
        result.scene.generic_string(), m_frame);
    for (const auto& warning : result.warnings) m_log.add(DiagnosticSource::asset, shown + ": " + gltf_problem_text(warning), m_frame);
    for (const auto& kept : result.kept)
        m_log.add(DiagnosticSource::asset, "Kept " + kept.generic_string() + ", which was edited since it was imported", m_frame);
    // What changed since the last import of the file.
    for (const auto& change : result.added) m_log.add(DiagnosticSource::asset, shown + ": added " + change.what + " " + change.identity, m_frame);
    for (const auto& change : result.renamed)
        m_log.add(DiagnosticSource::asset, shown + ": renamed " + change.what + " " + change.previous + " to " + change.identity +
                  ", keeping its ID", m_frame);
    for (const auto& change : result.removed)
        m_log.add(DiagnosticSource::asset, shown + ": removed " + change.what + " " + change.identity +
                  (change.what == "mesh" || change.what == "texture" ? "; scenes still using it show it as missing"
                   : change.what == "material" ? "; its file stays in the project" : ""), m_frame);
    refresh_project();
    // The import's scene, open and rewritten: shown again, unless it has unsaved edits.
    const auto scene_file = m_project->content_root / result.scene;
    if (m_scene && m_scene_path == scene_file && std::ranges::count(result.written, result.scene) > 0) {
        if (!m_scene->dirty()) open_scene(result.scene);
        else notice("The open scene was reimported", result.scene.generic_string() + " was written again by the import, but this "
                    "window has unsaved changes to it. Saving keeps your changes; later imports then keep the file as edited.");
    }
    if (place && m_scene) {
        const auto center = ImVec2{(m_layout.viewport_min.x + m_layout.viewport_max.x) * 0.5f,
                                   (m_layout.viewport_min.y + m_layout.viewport_max.y) * 0.5f};
        report(place_scene(result.scene, drop_point(center).value_or(math::Vec3{0.0f})), "Place " + result.scene.stem().string());
    }
    return result;
}

EditResult EditorShell::place_scene(const std::filesystem::path& relative, math::Vec3 at) {
    if (!m_scene || !m_assets || !m_project) return {false, "No scene is open"};
    const auto file = m_project->resolve(relative);
    if (!file) return {false, relative.generic_string() + " is outside the project's content"};
    const auto loaded = load_scene_file(*file, asset_property_context(*m_assets));
    if (!loaded) return {false, relative.generic_string() + ": " + loaded.diagnostics.front().message};
    const auto placed = m_scene->insert(loaded.document, "Place " + relative.stem().string(), at);
    if (placed && m_scene->primary()) m_reveal = m_scene->primary();
    return placed;
}

void EditorShell::accept_dropped_files() {
    if (m_dropped_files.empty()) return;
    const auto dropped = std::exchange(m_dropped_files, {});
    if (!m_project) return notice("No project is open", "Open a project before dropping files on the editor.");
    for (const auto& name : dropped) {
        const auto path = std::filesystem::path(name);
        if (!gltf_file(path)) {
            m_log.add(DiagnosticSource::asset, "Not imported: " + path.filename().string() + " is not a .gltf or .glb file", m_frame);
            continue;
        }
        // A file inside the content root is imported where it is; one from elsewhere is copied into
        // models/ first, a .gltf with the files it names.
        auto inside = path;
        if (m_project->relative(path).empty()) {
            const auto copied = copy_into_project(path);
            if (!copied) continue;
            inside = *copied;
        }
        import_model(inside, m_scene != nullptr);
    }
}

std::optional<std::filesystem::path> EditorShell::copy_into_project(const std::filesystem::path& source) {
    const auto folder = m_project->content_root / "models";
    const auto failed = [&](const std::string& why) -> std::optional<std::filesystem::path> {
        m_log.add(DiagnosticSource::asset, "Couldn't copy " + source.filename().string() + " into the project: " + why, m_frame);
        notice("Couldn't import " + source.filename().string(), why + "\nNothing was changed.");
        return std::nullopt;
    };
    auto files = std::vector<std::filesystem::path>{source.filename()};
    if (lowercase(source.extension().string()) == ".gltf") {
        const auto opened = GltfFile::open(source);
        if (!opened) return failed(gltf_problem_text(opened.errors.front()));
        for (const auto& named : opened.file->document().files) {
            const auto relative = std::filesystem::path(named).lexically_normal();
            if (relative.is_absolute() || relative.empty() || *relative.begin() == "..")
                return failed("it names " + named + ", outside its folder, which an import cannot copy");
            files.push_back(relative);
        }
    }
    // A .glb goes in models/; a .gltf and its files in a folder of their own.
    const auto target = lowercase(source.extension().string()) == ".glb" ? folder : folder / source.stem();
    std::error_code error;
    for (const auto& file : files)
        if (std::filesystem::exists(target / file, error))
            return failed(m_project->relative(target / file).generic_string() + " already exists; import that file, or rename one of them");
    for (const auto& file : files) {
        std::filesystem::create_directories((target / file).parent_path(), error);
        if (!std::filesystem::copy_file(source.parent_path() / file, target / file, error) || error)
            return failed("copying " + file.generic_string() + " failed: " + error.message());
    }
    m_log.add(DiagnosticSource::asset, "Copied " + source.filename().string() + " into " +
        m_project->relative(target).generic_string(), m_frame);
    return target / source.filename();
}
std::vector<EditorShell::FileStamp> EditorShell::stamp_files(const std::vector<std::filesystem::path>& files) const {
    auto stamps = std::vector<FileStamp>{};
    for (const auto& file : files) {
        auto stamp = FileStamp{};
        auto error = std::error_code{};
        const auto path = m_project ? m_project->resolve(file) : std::nullopt;
        stamp.present = path && std::filesystem::is_regular_file(*path, error);
        if (stamp.present) {
            stamp.time = std::filesystem::last_write_time(*path, error);
            stamp.size = std::filesystem::file_size(*path, error);
        }
        stamps.push_back(stamp);
    }
    return stamps;
}

void EditorShell::check_imported_sources() {
    if (!m_project) return;
    auto changed = std::vector<std::filesystem::path>{};
    for (auto& watched : m_imported_sources) {
        auto now = stamp_files(watched.files);
        if (now == watched.stamps) continue;
        watched.stamps = std::move(now);
        // A source that is gone, or is being written (an empty file), waits for its next change.
        if (!watched.stamps.front().present || watched.stamps.front().size == 0) continue;
        changed.push_back(watched.source);
    }
    // Importing rescans the project, which rebuilds the list: what changed was copied out first.
    for (const auto& source : changed) {
        m_log.add(DiagnosticSource::asset, source.generic_string() + " changed; importing it again", m_frame);
        import_model(source, false, false); // a failure is logged; the last import stays in use
    }
}
} // namespace maya::editor
