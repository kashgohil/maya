#include "maya/package/packager.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/build_info.hpp"
#include "maya/metrics/metrics.hpp"
#include "maya/properties/schema.hpp"
#include "maya/scene/scene_io.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace maya {
namespace fs = std::filesystem;
namespace {
std::optional<std::vector<std::byte>> read_file(const fs::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) return std::nullopt;
    auto bytes = std::vector<std::byte>{};
    std::transform(std::istreambuf_iterator<char>(input), {}, std::back_inserter(bytes), [](char c) { return std::byte(c); });
    if (input.bad()) return std::nullopt;
    return bytes;
}
bool write_file(const fs::path& path, std::span<const std::byte> bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    auto output = std::ofstream(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(output.flush());
}
bool write_text(const fs::path& path, const std::string& text) {
    return write_file(path, std::as_bytes(std::span(text.data(), text.size())));
}
std::string id_name(AssetId id) {
    char text[40];
    std::snprintf(text, sizeof(text), "%016llx-%016llx", static_cast<unsigned long long>(id.high), static_cast<unsigned long long>(id.low));
    return text;
}
/// A catalog path's file and, for a part of an imported file, what follows '#'.
std::pair<fs::path, std::string> split_part(const fs::path& path) {
    const auto text = path.generic_string();
    const auto hash = text.find('#');
    if (hash == std::string::npos) return {path, {}};
    return {fs::path(text.substr(0, hash)), text.substr(hash + 1)};
}
/// The bundle's fixed property list: the same for the same name, so packages are repeatable.
std::string info_plist(const std::string& name, const std::string& executable) {
    auto identifier = std::string("dev.maya.");
    for (const auto c : name) identifier += std::isalnum(static_cast<unsigned char>(c)) ? char(std::tolower(static_cast<unsigned char>(c))) : '-';
    const auto escape = [](const std::string& text) {
        auto out = std::string{};
        for (const auto c : text) {
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else out += c;
        }
        return out;
    };
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n"
           "    <key>CFBundleExecutable</key><string>" + escape(executable) + "</string>\n"
           "    <key>CFBundleIdentifier</key><string>" + escape(identifier) + "</string>\n"
           "    <key>CFBundleName</key><string>" + escape(name) + "</string>\n"
           "    <key>CFBundlePackageType</key><string>APPL</string>\n"
           "    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
           "    <key>LSMinimumSystemVersion</key><string>13.0</string>\n"
           "    <key>NSHighResolutionCapable</key><true/>\n"
           "</dict>\n</plist>\n";
}
uint64_t folder_bytes(const fs::path& folder) {
    auto total = uint64_t{0};
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
        if (it->is_regular_file(error)) total += it->file_size(error);
    return total;
}
} // namespace

PackageReport package_project(const PackageOptions& options) {
    auto report = PackageReport{};
    auto clock = Stopwatch{};
    const auto fail = [&](std::string why) {
        report.error = std::move(why);
        return report;
    };
    if (options.output.extension() != ".app") return fail("the package must be a macOS application bundle, <name>.app: " + options.output.string());
    std::error_code error;
    if (fs::exists(options.output, error) && !fs::is_regular_file(options.output / "Contents/Resources" / package_manifest_name, error))
        return fail(options.output.string() + " exists and is not a package; choose another place");
    if (!fs::is_regular_file(options.player, error)) return fail("the player was not found: " + options.player.string());
    const auto shader = read_file(options.shader);
    if (!shader) return fail("the renderer shader was not found: " + options.shader.string());

    // The project, its catalog, and the scenes to package.
    auto opened = open_project(options.project);
    if (!opened) return fail(opened.error);
    const auto& project = opened.project;
    auto catalog_input = std::ifstream(project.catalog);
    if (!catalog_input) return fail("cannot read the catalog " + project.catalog.string());
    auto catalog = read_asset_catalog(catalog_input);
    if (!catalog) return fail(project.catalog.string() + ": " + catalog.diagnostic.message);
    auto records = std::map<AssetId, AssetRecord>{};
    for (const auto& record : catalog.records) records.emplace(record.id, record);
    if (!project.startup_scene) return fail(project.file.string() + " has no startup scene to package");
    auto scenes = std::vector<fs::path>{project.relative(*project.startup_scene)};
    for (const auto& scene : options.scenes) {
        const auto resolved = project.resolve(scene);
        if (!resolved) return fail(scene.generic_string() + " is outside the project's content root");
        const auto relative = project.relative(*resolved);
        if (std::ranges::find(scenes, relative) == scenes.end()) scenes.push_back(relative);
    }

    // Everything the scenes reach, through the same references validation follows: their meshes,
    // materials, scripts, environments, skins, and clips, and the materials' textures.
    auto needed = std::set<AssetId>{};
    auto missing = std::string{};
    const auto context = PropertyValidationContext{[&](AssetId id, ReferenceKind kind) {
        const auto found = records.find(id);
        if (found == records.end()) return ReferenceStatus::missing;
        if (found->second.kind != reference_asset_kind(kind)) return ReferenceStatus::wrong_type;
        needed.insert(id);
        return ReferenceStatus::valid;
    }};
    auto scene_texts = std::vector<std::pair<fs::path, std::vector<std::byte>>>{};
    for (const auto& scene : scenes) {
        const auto path = project.content_root / scene;
        const auto loaded = load_scene_file(path, context);
        if (!loaded) return fail(scene.generic_string() + ": " + loaded.diagnostics.front().message);
        auto bytes = read_file(path);
        if (!bytes) return fail("cannot read " + scene.generic_string());
        scene_texts.emplace_back(scene, std::move(*bytes));
    }
    for (const auto id : std::vector<AssetId>(needed.begin(), needed.end())) {
        const auto& record = records.at(id);
        if (record.kind != AssetKind::material) continue;
        auto input = std::ifstream(project.content_root / record.path);
        if (!input) return fail("material " + record.path.generic_string() + " cannot be read");
        auto read = read_material_file(input);
        if (!read) return fail(record.path.generic_string() + ": " + read.error);
        if (const auto checked = validate_material(read.material, context); !checked)
            return fail(record.path.generic_string() + ": " + std::string(checked.message));
    }

    // Cooked into a new bundle beside the output, which replaces it only when everything is written.
    const auto partial = fs::path(options.output.string() + ".partial");
    fs::remove_all(partial, error);
    const auto resources = partial / "Contents/Resources";
    const auto content = resources / "project/content";
    auto cooker = AssetCooker(options.limits, std::make_shared<CookCache>(cook_cache_folder(project)));
    auto packaged = std::vector<AssetRecord>{};
    const auto abandon = [&](std::string why) {
        fs::remove_all(partial, error);
        return fail(std::move(why));
    };
    for (const auto id : needed) {
        const auto& record = records.at(id);
        const auto [file, part] = split_part(record.path);
        const auto source = project.content_root / file;
        const auto cooked_path = [&](const char* extension) { return fs::path("cooked") / (id_name(id) + extension); };
        auto payload = std::vector<std::byte>{};
        auto path = fs::path{};
        auto problem = AssetDiagnostic{};
        switch (record.kind) {
        case AssetKind::mesh: {
            auto cooked = part.empty() ? cooker.mesh(source) : cooker.imported_mesh(source, part);
            if (!cooked) problem = cooked.diagnostic;
            else payload = write_cooked_mesh(*cooked.value);
            path = cooked_path(cooked_mesh_extension);
            ++report.meshes;
            break;
        }
        case AssetKind::texture: {
            auto cooked = part.empty() ? cooker.texture(source) : cooker.imported_texture(source, part);
            if (!cooked) problem = cooked.diagnostic;
            else payload = write_cooked_texture(*cooked.value);
            path = cooked_path(cooked_texture_extension);
            ++report.textures;
            break;
        }
        case AssetKind::environment: {
            auto cooked = cooker.environment(source);
            if (!cooked) problem = cooked.diagnostic;
            else payload = write_cooked_environment(*cooked.value);
            path = cooked_path(cooked_environment_extension);
            ++report.environments;
            break;
        }
        case AssetKind::skin: {
            auto cooked = cooker.imported_skin(source, part);
            if (!cooked) problem = cooked.diagnostic;
            else payload = write_skin(*cooked.value);
            path = cooked_path(cooked_skin_extension);
            ++report.skins;
            break;
        }
        case AssetKind::animation: {
            auto cooked = cooker.imported_animation(source, part);
            if (!cooked) problem = cooked.diagnostic;
            else payload = write_animation(*cooked.value);
            path = cooked_path(cooked_animation_extension);
            ++report.animations;
            break;
        }
        case AssetKind::material:
        case AssetKind::script: {
            // Authored text, as the player reads it.
            auto bytes = read_file(source);
            if (!bytes) problem = {AssetError::missing_file, source.string() + ": cannot be read"};
            else payload = std::move(*bytes);
            path = record.path;
            ++(record.kind == AssetKind::material ? report.materials : report.scripts);
            break;
        }
        }
        if (problem) {
            // The reason, without the cooker's own copy of the source's absolute path.
            auto reason = problem.message;
            if (const auto root = project.content_root.string(); reason.starts_with(root))
                if (const auto colon = reason.find(": "); colon != std::string::npos) reason.erase(0, colon + 2);
            return abandon(std::string(asset_kind_name(record.kind)) + " " + record.path.generic_string() + ": " + reason);
        }
        // Cooked files go in the cook cache's checked envelope, so the player refuses a damaged one.
        const auto authored = record.kind == AssetKind::material || record.kind == AssetKind::script;
        if (!write_file(content / path, authored ? payload : wrap_cooked(payload))) return abandon("cannot write " + (content / path).string());
        packaged.push_back({id, record.kind, path});
    }
    for (const auto& [scene, bytes] : scene_texts)
        if (!write_file(content / scene, bytes)) return abandon("cannot write " + (content / scene).string());

    // The project and catalog as the package holds them, the shaders, the player, and its property list.
    auto settings = project.settings;
    settings.content = "content";
    settings.catalog = "catalog.maya";
    settings.startup_scene = scenes.front();
    auto project_text = std::ostringstream{};
    write_project(project_text, settings);
    auto catalog_text = std::ostringstream{};
    write_asset_catalog(catalog_text, packaged);
    const auto name = options.name.empty() ? project.file.parent_path().filename().string() : options.name;
    const auto executable = options.player.filename().string();
    if (!write_text(resources / "project/project.maya", project_text.str()) || !write_text(content / "catalog.maya", catalog_text.str()) ||
        !write_file(resources / "resources/shaders/metal/renderer.metal", *shader) ||
        !write_text(partial / "Contents/Info.plist", info_plist(name, executable)))
        return abandon("cannot write the package's files in " + partial.string());
    fs::create_directories(partial / "Contents/MacOS", error);
    if (!fs::copy_file(options.player, partial / "Contents/MacOS" / executable, fs::copy_options::overwrite_existing, error))
        return abandon("cannot copy the player: " + error.message());

    // The manifest: every file in Resources, sorted, with its size and digest.
    auto& manifest = report.manifest;
    manifest.name = name;
    const auto build = build_info();
    manifest.build = build.revision + " " + build.build_type + " " + build.compiler;
    manifest.scenes = scenes;
    for (auto it = fs::recursive_directory_iterator(resources, error); !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file()) continue;
        const auto bytes = read_file(it->path());
        if (!bytes) return abandon("cannot read back " + it->path().string());
        manifest.files.push_back({it->path().lexically_relative(resources), uint64_t(bytes->size()), sha256(*bytes)});
    }
    std::ranges::sort(manifest.files, {}, [](const PackageFile& file) { return file.path.generic_string(); });
    manifest.content = package_content_digest(manifest.files);
    auto manifest_text = std::ostringstream{};
    write_package_manifest(manifest_text, manifest);
    if (!write_text(resources / package_manifest_name, manifest_text.str())) return abandon("cannot write the manifest");

    fs::remove_all(options.output, error);
    fs::create_directories(options.output.parent_path(), error);
    fs::rename(partial, options.output, error);
    if (error) return abandon("cannot move the package into place at " + options.output.string() + ": " + error.message());
    report.bytes = folder_bytes(options.output);
    report.milliseconds = clock.milliseconds();
    return report;
}

} // namespace maya
