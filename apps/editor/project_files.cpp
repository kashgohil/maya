// Projects, scene files, the unsaved-changes and save-as dialogs, and the asset browser.

#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

namespace maya::editor {
using namespace detail;
namespace {
constexpr size_t scene_scan_limit = 4096; // directory entries visited when listing a project's scenes
constexpr size_t notice_problems = 6; // scene problems shown in a notice; the rest are in Diagnostics
constexpr float dialog_width = 440.0f;

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

/// Scene problems as a notice: the first few, then how many more are in Diagnostics.
std::string summarize(const SceneDiagnostics& problems) {
    auto text = std::string{};
    for (size_t i = 0; i < problems.size() && i < notice_problems; ++i) text += (i ? "\n" : "") + problems[i].message;
    if (problems.size() > notice_problems)
        text += "\n" + std::to_string(problems.size() - notice_problems) + " more in Diagnostics.";
    return text;
}

bool has_component(const EntityRecord& record, ComponentId id) {
    return std::ranges::any_of(record.components, [&](const ComponentValue& value) { return component_id(value) == id; });
}

/// A dialog button that records where it is drawn; the primary one is filled with the accent.
bool dialog_button(EditorLayout& layout, const char* label, const char* key, bool primary = false) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::color::accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::rgb(0x909EFF));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::color::rgb(0x6A7BF0));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::rgb(0x0B0C0E));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14.0f, 6.0f});
    const auto pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    if (primary) ImGui::PopStyleColor(4);
    layout.controls.push_back({std::string("dialog.") + key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    return pressed;
}

/// Moves the cursor so that buttons of these labels end at the right edge of the dialog.
void align_buttons_right(std::initializer_list<const char*> labels) {
    const auto& style = ImGui::GetStyle();
    auto width = 0.0f;
    for (const auto* label : labels) width += ImGui::CalcTextSize(label, nullptr, true).x + 28.0f + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width + style.ItemSpacing.x));
}

void heading(const theme::Fonts& fonts, const char* glyph, ImU32 tone, const std::string& text) {
    icon_text(glyph, tone, 10.0f);
    ImGui::PushFont(fonts.strong);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
    ImGui::Dummy({0.0f, 2.0f});
}

void muted_text(const std::string& text, ImU32 tone = theme::color::muted) {
    ImGui::PushStyleColor(ImGuiCol_Text, tone);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + dialog_width - 40.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
} // namespace

// Projects and scene files --------------------------------------------------------------------------

std::string EditorShell::rename_collision_group(size_t index, const std::string& name) {
    if (!m_project) return "No project is open";
    if (index >= collision_group_names) return "Collision groups are numbered 0 to 15";
    if (!name.empty())
        if (auto error = validate_collision_group_name(name); !error.empty()) return error;
    auto& groups = m_project->settings.collision_groups;
    if (groups[index] == name) return {};
    const auto previous = groups[index];
    groups[index] = name;
    if (auto error = save_project(*m_project); !error.empty()) {
        groups[index] = previous;
        return error;
    }
    m_log.add(DiagnosticSource::project, "Collision group " + std::to_string(index) + " is now " +
              collision_group_label(groups, index), m_frame);
    return {};
}

void EditorShell::draw_collision_groups() {
    if (!m_groups_open || !m_project) return;
    ImGui::SetNextWindowSize({320.0f, 0.0f}, ImGuiCond_Appearing);
    if (!ImGui::Begin((std::string(icon::stack) + "  Collision groups###collision_groups").c_str(), &m_groups_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
    ImGui::TextWrapped("Colliders choose a group and the groups they collide with. Names are saved in %s.",
                       m_project->file.filename().string().c_str());
    ImGui::PopStyleColor();
    const auto& groups = m_project->settings.collision_groups;
    if (theme::begin_properties("groups")) {
        for (size_t group = 0; group < collision_group_names; ++group) {
            auto& buffer = m_group_names[group];
            ImGui::PushID(int(group));
            theme::property(std::to_string(group).c_str());
            if (!ImGui::IsAnyItemActive()) // otherwise a field being typed in keeps its text
                std::snprintf(buffer.data(), buffer.size(), "%s", groups[group].c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##name", ("Group " + std::to_string(group)).c_str(), buffer.data(), buffer.size());
            m_layout.controls.push_back({"group." + std::to_string(group), ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            if (ImGui::IsItemDeactivatedAfterEdit()) m_groups_error = rename_collision_group(group, buffer.data());
            ImGui::PopID();
        }
        theme::end_properties();
    }
    if (!m_groups_error.empty()) {
        icon_text(icon::warning, theme::color::danger, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::danger);
        ImGui::TextWrapped("%s", m_groups_error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

std::string EditorShell::read_catalog(const Project& project, std::unique_ptr<AssetRegistry>& registry) {
    auto opened = open_project_assets(project, std::make_unique<FileAssetProvider>(m_device));
    registry = std::move(opened.registry);
    return opened.error;
}

bool EditorShell::open_project(const std::filesystem::path& path) {
    const auto fail = [&](const std::string& message) {
        m_log.add(DiagnosticSource::project, message, m_frame);
        notice("Couldn't open the project", message);
        return false;
    };
    auto opened = maya::open_project(path);
    if (!opened) return fail(opened.error);
    auto registry = std::unique_ptr<AssetRegistry>{};
    if (auto error = read_catalog(opened.project, registry); !error.empty()) return fail(error);
    m_project = std::move(opened.project);
    m_assets = std::move(registry);
    m_selected_asset.reset();
    scan_project();
    m_log.add(DiagnosticSource::project, "Opened project " + m_project->name() + " (" +
        std::to_string(m_assets->records().size()) + " assets, content in " + m_project->content_root.string() + ")", m_frame);
    m_scripts.clear();
    check_script_files(); // compile errors are reported now, before anything plays
    // A startup scene that cannot be opened leaves a new scene, with the reason in a notice.
    if (!m_project->startup_scene || !open_scene(*m_project->startup_scene)) new_scene();
    return true;
}

void EditorShell::replace_scene(std::unique_ptr<SceneEditor> scene, std::filesystem::path path) {
    stop_play(); // a play World belongs to the scene it was started from
    // References are checked against whichever catalog is current, including after a refresh.
    scene->set_validation_context({[this](AssetId id, ReferenceKind kind) {
        return m_assets ? asset_property_context(*m_assets).resolve_asset(id, kind) : ReferenceStatus::missing;
    }});
    m_scene = std::move(scene);
    m_scene_path = std::move(path);
    m_renaming.reset();
    m_edit_group_open = false; // the group belonged to the previous scene's history
    m_edit_error.clear();
    m_euler_entity = m_name_entity = EntityId{};
    m_snapshot.reset();
    m_pick_hits.clear();
    m_reveal.reset();
    m_frame_problems.clear();
}

bool EditorShell::open_scene(const std::filesystem::path& path) {
    const auto title = "Couldn't open " + path.filename().string();
    const auto fail = [&](const std::string& message) {
        m_log.add(DiagnosticSource::scene, message, m_frame);
        notice(title, message);
        return false;
    };
    if (!m_project || !m_assets) return fail("No project is open");
    const auto full = m_project->resolve(path);
    if (!full) return fail(path.generic_string() + " is outside the project's content root " + m_project->content_root.string());
    const auto context = asset_property_context(*m_assets);
    auto loaded = load_scene_file(*full, context);
    // Roots keep the file's order; the World has none.
    auto roots = std::vector<EntityId>{};
    for (const auto& entity : loaded.document.entities) if (!entity.parent) roots.push_back(entity.id);
    auto built = loaded ? instantiate_scene(std::move(loaded.document), context)
                        : SceneWorldResult{nullptr, std::move(loaded.diagnostics)};
    if (!built) {
        for (const auto& problem : built.diagnostics) m_log.add(DiagnosticSource::scene, problem.message, m_frame);
        notice(title, summarize(built.diagnostics));
        return false;
    }
    replace_scene(std::make_unique<SceneEditor>(std::move(built.world), roots), *full); // history starts empty
    m_log.add(DiagnosticSource::scene, "Opened " + m_project->relative(*full).generic_string() + " (" +
        std::to_string(m_scene->world().size()) + " entities)", m_frame);
    return true;
}

bool EditorShell::new_scene() {
    if (!m_project) {
        notice("Couldn't create a scene", "No project is open");
        return false;
    }
    auto camera = TransformComponent{};
    camera.translation = {0.0f, 1.5f, 6.0f};
    camera.rotation = math::Quat::from_axis_angle({1.0f, 0.0f, 0.0f}, -0.2f);
    auto sun = TransformComponent{};
    sun.translation = {0.0f, 3.0f, 0.0f};
    sun.rotation = math::Quat(-0.5161719f, 0.2429044f, 0.0f, 0.82131857f); // down and to the side
    auto document = SceneDocument{};
    document.entities.push_back({EntityId::generate(), std::nullopt, {NameComponent{"Camera"}, camera, CameraComponent{}}});
    document.entities.push_back({EntityId::generate(), std::nullopt, {NameComponent{"Sun"}, sun, LightComponent{}}});
    const auto roots = std::vector<EntityId>{document.entities[0].id, document.entities[1].id};
    auto built = instantiate_scene(std::move(document), {});
    if (!built) {
        notice("Couldn't create a scene", summarize(built.diagnostics));
        return false;
    }
    replace_scene(std::make_unique<SceneEditor>(std::move(built.world), roots), {});
    m_log.add(DiagnosticSource::scene, "New scene", m_frame);
    return true;
}

std::string EditorShell::save_scene(const std::filesystem::path& path) {
    const auto fail = [&](std::string message) {
        m_log.add(DiagnosticSource::scene, "Save failed: " + message, m_frame);
        return message;
    };
    if (!m_scene || !m_project || !m_assets) return fail("No scene is open");
    const auto& wanted = path.empty() ? m_scene_path : path;
    if (wanted.empty()) return fail("The scene has no file yet; choose one with Save as");
    const auto target = m_project->resolve(wanted);
    if (!target) return fail(wanted.generic_string() + " is outside the project's content root");
    if (target->extension() != ".scene") return fail("Scene files must end in .scene");
    auto error = std::error_code{};
    if (std::filesystem::is_directory(*target, error)) return fail(wanted.generic_string() + " is a folder");
    std::filesystem::create_directories(target->parent_path(), error);
    if (error) return fail("Cannot create the folder " + m_project->relative(target->parent_path()).generic_string() +
                           ": " + error.message());
    const auto problems = save_scene_file(*target, m_scene->document(), asset_property_context(*m_assets));
    if (!problems.empty()) {
        // The summary names the first few; the rest go to the log on their own.
        for (auto i = notice_problems; i < problems.size(); ++i) m_log.add(DiagnosticSource::scene, problems[i].message, m_frame);
        return fail(summarize(problems));
    }
    m_scene->mark_saved();
    m_scene_path = *target;
    const auto relative = m_project->relative(*target);
    if (std::ranges::find(m_scene_files, relative) == m_scene_files.end()) {
        m_scene_files.push_back(relative);
        std::ranges::sort(m_scene_files, {}, [](const std::filesystem::path& p) { return p.generic_string(); });
        m_shown_stale = true;
    }
    m_log.add(DiagnosticSource::scene, "Saved " + relative.generic_string(), m_frame);
    return {};
}

void EditorShell::scan_project() {
    m_scene_files.clear();
    m_missing_files.clear();
    if (!m_project) return;
    const auto& root = m_project->content_root;
    auto error = std::error_code{};
    auto entries = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    size_t visited = 0;
    for (; !error && entries != std::filesystem::recursive_directory_iterator{}; entries.increment(error)) {
        if (++visited > scene_scan_limit) {
            m_log.add(DiagnosticSource::project, "Stopped listing scene files after " + std::to_string(scene_scan_limit) +
                " entries in " + root.string(), m_frame);
            break;
        }
        const auto& entry = *entries;
        auto entry_error = std::error_code{};
        if (entry.path().filename().string().starts_with('.')) { // hidden files and folders
            if (entry.is_directory(entry_error)) entries.disable_recursion_pending();
            continue;
        }
        if (entry.is_regular_file(entry_error) && entry.path().extension() == ".scene")
            m_scene_files.push_back(entry.path().lexically_relative(root));
    }
    std::ranges::sort(m_scene_files, {}, [](const std::filesystem::path& p) { return p.generic_string(); });
    m_asset_rows.clear();
    if (m_assets)
        for (auto& record : m_assets->records()) {
            const auto full = m_project->resolve(record.path);
            const auto missing = !full || !std::filesystem::is_regular_file(*full, error);
            if (missing) m_missing_files.push_back(record.id);
            auto search = lowercase(record.path.generic_string());
            m_asset_rows.push_back({std::move(record), std::move(search), missing});
        }
    m_shown_stale = true;
}

void EditorShell::refresh_project() {
    if (!m_project) return;
    auto registry = std::unique_ptr<AssetRegistry>{};
    if (auto error = read_catalog(*m_project, registry); !error.empty()) {
        m_log.add(DiagnosticSource::project, error, m_frame);
        notice("Couldn't reload the catalog", error + "\nThe previous catalog stays in use.");
    } else {
        m_assets = std::move(registry); // loaded versions are reloaded from their files on next use
        m_scripts.clear();
        check_script_files();
    }
    scan_project();
    m_log.add(DiagnosticSource::project, "Refreshed: " + std::to_string(m_assets->records().size()) + " assets, " +
        std::to_string(m_scene_files.size()) + " scenes", m_frame);
}

// Unsaved changes and dialogs -----------------------------------------------------------------------

void EditorShell::notice(std::string title, std::string message) {
    m_prompt = EditorPrompt::notice;
    m_prompt_opening = true;
    m_prompt_title = std::move(title);
    m_prompt_message = std::move(message);
    m_prompt_caution = false;
}

void EditorShell::ask_save_as() {
    m_prompt = EditorPrompt::save_as;
    m_prompt_opening = true;
    m_prompt_message.clear();
    m_prompt_caution = false;
    m_replace_confirmed.clear();
    const auto current = m_project && !m_scene_path.empty() ? m_project->relative(m_scene_path)
                                                            : std::filesystem::path("untitled.scene");
    std::snprintf(m_save_as_buffer, sizeof(m_save_as_buffer), "%s", current.generic_string().c_str());
}

void EditorShell::save_or_ask() {
    if (!m_scene) return;
    if (m_scene_path.empty()) return ask_save_as();
    if (auto error = save_scene(); !error.empty()) notice("Couldn't save " + m_scene_path.filename().string(), error);
}

void EditorShell::request(Pending action, std::filesystem::path path) {
    m_pending = action;
    m_pending_path = std::move(path);
    if (m_scene && m_scene->dirty()) {
        m_prompt = EditorPrompt::unsaved_changes;
        m_prompt_opening = true;
        m_prompt_message.clear();
        return;
    }
    perform_pending();
}

void EditorShell::request_open_scene(const std::filesystem::path& path) { request(Pending::open, path); }
void EditorShell::request_new_scene() { request(Pending::create, {}); }

bool EditorShell::request_close() {
    if (m_close_confirmed || !m_scene || !m_scene->dirty()) return true;
    // Closing supersedes whatever the prompt was waiting for, and replaces any other dialog.
    if (m_prompt == EditorPrompt::unsaved_changes) m_pending = Pending::close;
    else request(Pending::close, {});
    return false;
}

void EditorShell::perform_pending() {
    switch (std::exchange(m_pending, Pending::none)) {
    case Pending::none: break;
    case Pending::open: open_scene(m_pending_path); break;
    case Pending::create: new_scene(); break;
    case Pending::close:
        m_close_confirmed = true;
        if (m_services.request_close) m_services.request_close();
        break;
    }
}

void EditorShell::draw_prompts() {
    if (m_prompt == EditorPrompt::none) return;
    const char* id = m_prompt == EditorPrompt::unsaved_changes ? "Unsaved changes###prompt_unsaved"
                   : m_prompt == EditorPrompt::save_as ? "Save scene as###prompt_save_as" : "Notice###prompt_notice";
    const auto opening = std::exchange(m_prompt_opening, false);
    if (opening) ImGui::OpenPopup(id);
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.42f},
                            ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({dialog_width, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0f, 18.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoSavedSettings;
    const auto open = ImGui::BeginPopupModal(id, nullptr, flags);
    ImGui::PopStyleVar(2);
    if (!open) {
        m_prompt = EditorPrompt::none; // closed from outside, e.g. another popup replaced it
        m_pending = Pending::none;
        return;
    }
    const auto close = [&] {
        ImGui::CloseCurrentPopup();
        m_prompt = EditorPrompt::none;
    };
    const auto enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    const auto escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const auto scene_name = m_scene_path.empty() ? std::string("Untitled") : m_scene_path.filename().string();

    if (m_prompt == EditorPrompt::unsaved_changes) {
        heading(m_fonts, icon::warning, theme::color::warning, "Save changes to " + scene_name + "?");
        const auto next = m_pending == Pending::open ? "before opening " + m_pending_path.filename().string()
                        : m_pending == Pending::create ? std::string("before creating a new scene")
                        : std::string("before closing the editor");
        muted_text("Your changes will be lost if you don't save them " + next + ".");
        if (!m_prompt_message.empty()) {
            ImGui::Dummy({0.0f, 4.0f});
            muted_text(m_prompt_message, theme::color::danger);
        }
        ImGui::Dummy({0.0f, 10.0f});
        const auto discard = dialog_button(m_layout, "Don't save", "discard");
        ImGui::SameLine();
        align_buttons_right({"Cancel", "Save"});
        const auto cancel = dialog_button(m_layout, "Cancel", "cancel") || escape;
        ImGui::SameLine();
        const auto save = dialog_button(m_layout, "Save", "save", true) || enter;
        if (cancel) {
            close();
            m_pending = Pending::none;
        } else if (discard) {
            close();
            perform_pending();
        } else if (save && m_scene_path.empty()) {
            close();
            ask_save_as(); // the pending action continues once it is saved
        } else if (save) {
            if (auto error = save_scene(); error.empty()) {
                close();
                perform_pending();
            } else {
                m_prompt_message = "Couldn't save: " + error;
            }
        }
    } else if (m_prompt == EditorPrompt::save_as) {
        heading(m_fonts, icon::floppy_disk, theme::color::accent, "Save scene as");
        // Paths are relative to the content folder, e.g. "basic_scene/assets/".
        auto content = m_project ? (std::filesystem::path(m_project->name()) /
            m_project->content_root.lexically_relative(m_project->file.parent_path())).lexically_normal().generic_string()
            : std::string{};
        if (!content.empty() && content.back() != '/') content += '/';
        muted_text("A path inside " + content, theme::color::faint);
        ImGui::Dummy({0.0f, 2.0f});
        if (opening) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto entered = ImGui::InputText("##path", m_save_as_buffer, sizeof(m_save_as_buffer),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        m_layout.controls.push_back({"dialog.path", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        if (!m_prompt_message.empty()) {
            ImGui::Dummy({0.0f, 2.0f});
            muted_text(m_prompt_message, m_prompt_caution ? theme::color::warning : theme::color::danger);
        }
        ImGui::Dummy({0.0f, 10.0f});
        align_buttons_right({"Cancel", "Save"});
        const auto cancel = dialog_button(m_layout, "Cancel", "cancel") || (escape && !ImGui::IsAnyItemActive());
        ImGui::SameLine();
        const auto save = dialog_button(m_layout, "Save", "save", true) || entered || (enter && !ImGui::IsAnyItemActive());
        if (cancel) {
            close();
            m_pending = Pending::none; // cancelling the save cancels what was waiting for it
        } else if (save) {
            auto relative = std::filesystem::path(trim(m_save_as_buffer));
            if (!relative.empty() && !relative.has_extension()) relative += ".scene";
            const auto target = relative.empty() || !m_project ? std::nullopt : m_project->resolve(relative);
            auto error = std::error_code{};
            m_prompt_caution = false;
            if (relative.empty()) {
                m_prompt_message = "Enter a file name, such as levels/intro.scene.";
            } else if (!target) {
                m_prompt_message = "Choose a path inside the project's content folder.";
            } else if (*target != m_scene_path && m_replace_confirmed != *target && std::filesystem::exists(*target, error)) {
                m_replace_confirmed = *target;
                m_prompt_caution = true;
                m_prompt_message = relative.generic_string() + " already exists. Save again to replace it.";
            } else if (auto failure = save_scene(*target); !failure.empty()) {
                m_prompt_message = failure;
            } else {
                close();
                perform_pending();
            }
        }
    } else {
        heading(m_fonts, icon::x_circle, theme::color::danger, m_prompt_title);
        muted_text(m_prompt_message);
        ImGui::Dummy({0.0f, 10.0f});
        align_buttons_right({"OK"});
        if (dialog_button(m_layout, "OK", "ok", true) || enter || escape) close();
    }
    ImGui::EndPopup();
}

void EditorShell::draw_scene_menu() {
    ImGui::SetNextWindowSizeConstraints({260.0f, 0.0f}, {520.0f, 520.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f, 10.0f});
    const auto open = ImGui::BeginPopup("scene_menu");
    ImGui::PopStyleVar();
    if (!open) return;
    theme::caption(m_fonts, "SCENES", std::to_string(m_scene_files.size()).c_str());
    if (m_scene_files.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextUnformatted("No scene files in the project yet");
        ImGui::PopStyleColor();
    }
    for (const auto& relative : m_scene_files) {
        const auto current = m_project && m_project->content_root / relative == m_scene_path;
        const auto label = std::string(current ? icon::check : icon::file) + "  " + relative.generic_string();
        if (ImGui::MenuItem(label.c_str(), nullptr, false, !current)) request_open_scene(relative);
    }
    ImGui::Separator();
    if (ImGui::MenuItem((std::string(icon::file_plus) + "  New scene").c_str(), "\xE2\x8C\x98N")) request_new_scene();
    if (ImGui::MenuItem((std::string(icon::floppy_disk) + "  Save").c_str(), "\xE2\x8C\x98S")) save_or_ask();
    if (ImGui::MenuItem((std::string(icon::pencil) + "  Save as\xE2\x80\xA6").c_str(), "\xE2\x87\xA7\xE2\x8C\x98S")) ask_save_as();
    ImGui::Separator();
    if (ImGui::MenuItem((std::string(icon::arrows_clockwise) + "  Refresh project").c_str())) refresh_project();
    ImGui::Separator();
    // Recorded Play (docs/play.md#recording-and-replay).
    const auto recorded = m_last_recording.has_value();
    if (ImGui::MenuItem((std::string(icon::play) + "  Play and record").c_str(), nullptr, false, !m_play)) start_play(true);
    if (ImGui::MenuItem((std::string(icon::skip_forward) + "  Replay the last recording").c_str(), nullptr, false, recorded && !m_play))
        replay_last_play();
    if (ImGui::MenuItem((std::string(icon::floppy_disk) + "  Save the last recording").c_str(), nullptr, false, recorded)) save_recording();
    ImGui::EndPopup();
}

// Placing and assigning assets ----------------------------------------------------------------------

std::string EditorShell::asset_name(AssetId asset) const {
    const auto info = m_assets ? m_assets->info(asset) : std::nullopt;
    return info ? info->record.path.stem().string() : std::string("asset");
}

EditResult EditorShell::place_mesh(AssetId mesh, const math::Vec3& position) {
    if (!m_scene || !m_assets) return {false, "No scene is open"};
    const auto info = m_assets->info(mesh);
    if (!info || info->record.kind != AssetKind::mesh) return {false, "That is not a mesh in the project's catalog"};
    auto transform = TransformComponent{};
    transform.translation = position;
    auto renderer = MeshRendererComponent{};
    renderer.mesh = {mesh};
    return m_scene->create(info->record.path.stem().string(), std::nullopt, {transform, renderer});
}

EditResult EditorShell::assign_asset(EntityId entity, AssetId asset) {
    if (!m_scene || !m_assets) return {false, "No scene is open"};
    const auto info = m_assets->info(asset);
    if (!info) return {false, "That asset is not in the project's catalog"};
    const auto* record = m_scene->record(entity);
    if (!record) return {false, "The entity no longer exists"};
    if (info->record.kind == AssetKind::script) {
        // Values the new script declares, with the same type, carry over; the rest belonged to the old one.
        auto script = ScriptComponent{AssetRef<ScriptAsset>{asset}, {}};
        const auto existing = std::ranges::find(record->components, ComponentId::script,
                                                [](const ComponentValue& value) { return component_id(value); });
        if (existing != record->components.end()) {
            const auto& previous = std::get<ScriptComponent>(*existing);
            const auto* description = script_description(asset);
            for (const auto& value : previous.values)
                if (previous.script.id == asset || (description && std::ranges::any_of(description->properties, [&](const auto& declared) {
                                                        return declared.name == value.name && declared.type == value.type;
                                                    })))
                    script.values.push_back(value);
        }
        return m_scene->set_component(entity, script);
    }
    auto renderer = MeshRendererComponent{};
    const auto existing = std::ranges::find(record->components, ComponentId::mesh_renderer,
                                            [](const ComponentValue& value) { return component_id(value); });
    if (existing != record->components.end()) renderer = std::get<MeshRendererComponent>(*existing);
    else if (!has_component(*record, ComponentId::transform))
        return {false, m_scene->display_name(entity) + " has no transform, so it cannot show a mesh"};
    if (info->record.kind == AssetKind::mesh) renderer.mesh = {asset};
    else renderer.material = {asset};
    return m_scene->set_component(entity, renderer);
}

std::optional<Ray> EditorShell::viewport_ray(ImVec2 point) const {
    const auto min = m_layout.viewport_min, max = m_layout.viewport_max;
    if (max.x <= min.x || max.y <= min.y || m_viewport_request.empty()) return std::nullopt;
    const auto view = make_render_view(m_camera.camera, m_camera.pose(), m_viewport_request.width, m_viewport_request.height);
    if (!view) return std::nullopt;
    const auto x = (point.x - min.x) / (max.x - min.x) * 2.0f - 1.0f;
    const auto y = 1.0f - (point.y - min.y) / (max.y - min.y) * 2.0f;
    return view_ray(*view, m_camera.pose(), m_camera.camera.vertical_fov, x, y);
}

std::optional<math::Vec3> EditorShell::drop_point(ImVec2 point, AssetId mesh) const {
    const auto ray = viewport_ray(point);
    if (!ray) return std::nullopt;
    auto distance = std::optional<float>{};
    if (m_snapshot)
        if (const auto hits = pick_meshes(*m_snapshot, *ray); !hits.empty()) distance = hits.front().distance;
    if (!distance && ray->direction.y < -1e-3f) // the ground plane, y = 0, within reach
        if (const auto t = -ray->origin.y / ray->direction.y; t > 0.0f && t < 500.0f) distance = t;
    if (!distance) return ray->origin + ray->direction * 5.0f; // in front of the camera
    auto position = ray->origin + ray->direction * *distance;
    // Rest the mesh on the surface: lift it by how far its geometry reaches below its origin.
    if (mesh.valid() && m_assets)
        if (const auto loaded = m_assets->acquire(AssetRef<MeshAsset>{mesh}))
            if (const auto& geometry = loaded.lease.value().geometry(); !geometry.positions.empty()) position.y -= geometry.min.y;
    return position;
}

std::optional<EntityId> EditorShell::mesh_at(ImVec2 point) const {
    const auto ray = viewport_ray(point);
    if (!ray || !m_snapshot) return std::nullopt;
    const auto hits = pick_meshes(*m_snapshot, *ray);
    return hits.empty() ? std::nullopt : std::optional{hits.front().entity};
}

void EditorShell::place_in_view(AssetId mesh) {
    const auto center = ImVec2{(m_layout.viewport_min.x + m_layout.viewport_max.x) * 0.5f,
                               (m_layout.viewport_min.y + m_layout.viewport_max.y) * 0.5f};
    const auto at = drop_point(center, mesh).value_or(math::Vec3{0.0f});
    const auto name = asset_name(mesh);
    if (const auto placed = place_mesh(mesh, at); !placed) report(placed, "Place " + name);
    else m_reveal = m_scene->primary(); // show it in the hierarchy
}

void EditorShell::assign_to_selection(AssetId asset) {
    if (!m_scene || m_scene->selection().empty()) return;
    const auto name = asset_name(asset);
    m_scene->begin_group("Assign " + name);
    for (const auto id : std::vector<EntityId>(m_scene->selection())) report(assign_asset(id, asset), "Assign " + name);
    m_scene->end_group();
}

void EditorShell::accept_asset_drop(EntityId target) {
    const auto* dragged = ImGui::GetDragDropPayload();
    if (!dragged || !dragged->IsDataType("MAYA_ASSET")) return;
    auto asset = AssetPayload{};
    std::memcpy(&asset, dragged->Data, sizeof(asset));
    if (!target.valid() && asset.kind != AssetKind::mesh) return; // a material needs an entity
    if (target.valid())
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::color::accent, 4.0f);
    if (!ImGui::AcceptDragDropPayload("MAYA_ASSET", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) return;
    if (!target.valid()) return place_in_view(asset.id);
    const auto name = asset_name(asset.id);
    report(assign_asset(target, asset.id), "Assign " + name);
}

void EditorShell::accept_viewport_drop() {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const auto* dragged = ImGui::GetDragDropPayload(); dragged && dragged->IsDataType("MAYA_ASSET")) {
        auto asset = AssetPayload{};
        std::memcpy(&asset, dragged->Data, sizeof(asset));
        const auto point = ImGui::GetIO().MousePos;
        // Meshes land where they are dropped; materials and scripts go to the object under the pointer.
        const auto target = asset.kind != AssetKind::mesh ? mesh_at(point) : std::nullopt;
        if (ImGui::AcceptDragDropPayload("MAYA_ASSET", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const auto name = asset_name(asset.id);
            if (asset.kind == AssetKind::mesh) {
                report(place_mesh(asset.id, drop_point(point, asset.id).value_or(math::Vec3{0.0f})), "Place " + name);
                if (m_scene && m_scene->primary()) m_reveal = m_scene->primary();
            } else if (target) {
                report(assign_asset(*target, asset.id), "Assign " + name);
            }
        }
        if (asset.kind == AssetKind::mesh || target)
            ImGui::GetWindowDrawList()->AddRect(m_layout.viewport_min, m_layout.viewport_max, theme::color::accent, 0.0f, 0, 2.0f);
    }
    ImGui::EndDragDropTarget();
}

// Asset browser -------------------------------------------------------------------------------------

void EditorShell::draw_asset_row(const AssetRow& row) {
    const auto& record = row.record;
    const auto info = m_assets->info(record.id);
    const auto mesh = record.kind == AssetKind::mesh;
    const auto script = record.kind == AssetKind::script;
    // Scripts are watched, so their state is current; the others are as the last scan found them.
    const auto* version = script ? &script_version(record.id) : nullptr;
    const auto missing = version ? !version->present : row.missing;
    const auto problem = version ? version->error : info && info->diagnostic ? info->diagnostic.message : std::string{};
    // Materials are small CPU data, so they load for their swatch (or the reason they cannot); meshes
    // load when first drawn.
    auto swatch = std::optional<ImU32>{};
    if (record.kind == AssetKind::material)
        if (const auto material = m_assets->acquire(AssetRef<MaterialAsset>{record.id})) {
            const auto& color = material.lease.value().base_color;
            const auto channel = [](float linear) { // linear to display (sRGB-like) 8-bit
                return int(std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f + 0.5f);
            };
            swatch = IM_COL32(channel(color.x), channel(color.y), channel(color.z), 255);
        }
    const auto state = info ? info->state : AssetState::unloaded;
    const auto failed = version ? !missing && !problem.empty() : state == AssetState::failed || (info && info->diagnostic);
    const auto* glyph = mesh ? icon::cube : script ? icon::file_code : icon::circle_half;
    ImGui::PushID(static_cast<int>(record.id.low ^ (record.id.high << 7)));
    const auto selected = m_selected_asset == record.id;
    if (ImGui::Selectable("##asset", selected, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, ImGui::GetFrameHeight()}))
        m_selected_asset = record.id;
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    m_layout.controls.push_back({"asset." + record.path.generic_string(), min, max});
    const auto hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (mesh) place_in_view(record.id);
        else if (script) open_script(record.id);
        else assign_to_selection(record.id);
    }
    if (ImGui::BeginDragDropSource()) {
        const auto payload = AssetPayload{record.id, record.kind};
        ImGui::SetDragDropPayload("MAYA_ASSET", &payload, sizeof(payload));
        icon_text(glyph, theme::color::muted);
        ImGui::TextUnformatted(record.path.stem().string().c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextUnformatted(mesh ? "Drop in the viewport to place it" : script ? "Drop on an object to attach it" : "Drop on an object to assign it");
        ImGui::PopStyleColor();
        ImGui::EndDragDropSource();
    }
    const auto selection = m_scene ? m_scene->selection().size() : 0;
    if (ImGui::BeginPopupContextItem("asset")) {
        if (mesh && ImGui::MenuItem((std::string(icon::plus) + "  Place in scene").c_str(), nullptr, false, m_scene != nullptr))
            place_in_view(record.id);
        if (script && ImGui::MenuItem((std::string(icon::file_code) + "  Open").c_str(), nullptr, false, !missing))
            open_script(record.id);
        const auto assign = std::string(icon::arrows_move) + "  Assign to selection" +
                            (selection ? " (" + std::to_string(selection) + ")" : std::string{});
        if (ImGui::MenuItem(assign.c_str(), nullptr, false, selection > 0)) assign_to_selection(record.id);
        ImGui::Separator();
        if (script && ImGui::MenuItem((std::string(icon::arrows_clockwise) + "  Reload").c_str())) {
            reload_script(record.id);
        } else if (!script && ImGui::MenuItem((std::string(icon::arrows_clockwise) + "  Reload").c_str())) {
            const auto diagnostic = mesh ? m_assets->reload(AssetRef<MeshAsset>{record.id}).diagnostic
                                         : m_assets->reload(AssetRef<MaterialAsset>{record.id}).diagnostic;
            m_log.add(DiagnosticSource::asset, diagnostic ? record.path.generic_string() + ": " + diagnostic.message
                                                          : "Reloaded " + record.path.generic_string(), m_frame);
            m_rescan = true; // after the rows are drawn: rescanning replaces them
        }
        if (ImGui::MenuItem((std::string(icon::copy) + "  Copy ID").c_str()))
            ImGui::SetClipboardText(id_text(record.id.high, record.id.low).c_str());
        ImGui::EndPopup();
    }
    if (hovered && !ImGui::GetDragDropPayload()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(record.path.generic_string().c_str());
        theme::mono_text(m_fonts, id_text(record.id.high, record.id.low).c_str(), true);
        if (missing) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::warning);
            ImGui::TextUnformatted("The file is missing from the content folder.");
            ImGui::PopStyleColor();
        }
        if (!missing && !problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::danger);
            ImGui::PushTextWrapPos(420.0f);
            ImGui::TextUnformatted(problem.c_str());
            if (version && version->good) ImGui::TextUnformatted("The last version that compiled stays in use.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndTooltip();
    }
    // Contents: icon or swatch, name, and a status on the right.
    auto* draw = ImGui::GetWindowDrawList();
    const auto middle = (min.y + max.y) * 0.5f;
    const auto text_y = middle - ImGui::GetTextLineHeight() * 0.5f;
    if (swatch) {
        draw->AddCircleFilled({min.x + 12.0f, middle}, 5.5f, *swatch);
        draw->AddCircle({min.x + 12.0f, middle}, 5.5f, theme::color::rgb(0xFFFFFF, 40));
    } else {
        draw->AddText({min.x + 5.0f, text_y}, theme::color::muted, glyph);
    }
    const auto tone = failed ? theme::color::danger : missing ? theme::color::warning : theme::color::text;
    const auto status = missing ? "missing" : failed ? (script ? "error" : "failed") : state == AssetState::ready ? "" : mesh ? "not loaded" : "";
    const auto status_width = ImGui::CalcTextSize(status).x;
    draw->PushClipRect(min, {max.x - status_width - 12.0f, max.y}, true);
    draw->AddText({min.x + 26.0f, text_y}, tone, record.path.stem().string().c_str());
    draw->PopClipRect();
    draw->AddText({max.x - status_width - 6.0f, text_y}, failed ? theme::color::danger : missing ? theme::color::warning
                  : theme::color::faint, status);
    ImGui::PopID();
}

void EditorShell::draw_assets() {
    // No bottom padding: the columns' lists run to the panel's bottom edge and scroll on their own.
    const auto padding = ImGui::GetStyle().WindowPadding;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padding.x, 0.0f});
    const auto open = begin_panel(assets_title, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + padding.y); // the usual space at the top
    if (open && !m_project) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextWrapped("No project is open.");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextWrapped("Start the editor with a project file or folder: maya_editor path/to/project.maya");
        ImGui::PopStyleColor();
    }
    if (open && m_project && m_assets) {
        // A filter and refresh; the project's name is already in the top bar.
        ImGui::SetNextItemWidth(std::min(240.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 6.0f));
        ImGui::InputTextWithHint("##filter", (std::string(icon::magnifying_glass) + "  Filter").c_str(),
                                 m_asset_filter, sizeof(m_asset_filter));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, 0u);
        if (ImGui::Button(icon::arrows_clockwise)) refresh_project();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Refresh: reread the catalog and list scene files\n%s", m_project->content_root.string().c_str());
        ImGui::Dummy({0.0f, 2.0f});

        // Filtering runs when the filter or the lists change, not every frame.
        if (auto filter = lowercase(trim(m_asset_filter)); m_shown_stale || filter != m_shown_filter) {
            for (auto& shown : m_shown_rows) shown.clear();
            const auto passes = [&](const std::string& text) { return filter.empty() || text.find(filter) != std::string::npos; };
            for (size_t i = 0; i < m_scene_files.size(); ++i)
                if (passes(lowercase(m_scene_files[i].generic_string()))) m_shown_rows[0].push_back(i);
            for (size_t i = 0; i < m_asset_rows.size(); ++i) {
                const auto kind = m_asset_rows[i].record.kind;
                if (passes(m_asset_rows[i].search))
                    m_shown_rows[kind == AssetKind::mesh ? 1 : kind == AssetKind::material ? 2 : 3].push_back(i);
            }
            m_shown_filter = std::move(filter);
            m_shown_stale = false;
        }
        constexpr auto table_flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("asset_columns", 4, table_flags, ImGui::GetContentRegionAvail())) {
            // Each column scrolls on its own and draws only its visible rows.
            const auto column = [&](const char* id, const char* caption, const std::vector<size_t>& shown, auto&& row) {
                ImGui::TableNextColumn();
                theme::caption(m_fonts, caption, std::to_string(shown.size()).c_str());
                // Through the cell's bottom padding, to the table's edge.
                const auto height = ImGui::GetContentRegionAvail().y + ImGui::GetStyle().CellPadding.y;
                ImGui::BeginChild(id, {0.0f, height}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                auto clipper = ImGuiListClipper{};
                clipper.Begin(int(shown.size()));
                while (clipper.Step())
                    for (auto i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) row(shown[size_t(i)]);
                ImGui::EndChild();
            };
            column("scenes", "SCENES", m_shown_rows[0], [&](size_t index) {
                const auto relative = m_scene_files[index]; // a copy: opening a scene may rescan the list
                const auto full = m_project->content_root / relative;
                const auto current = full == m_scene_path;
                ImGui::PushID(relative.generic_string().c_str());
                ImGui::Selectable("##scene", current, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, ImGui::GetFrameHeight()});
                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                m_layout.controls.push_back({"scene." + relative.generic_string(), min, max});
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !current)
                    request_open_scene(relative);
                if (ImGui::BeginPopupContextItem("scene")) {
                    if (ImGui::MenuItem((std::string(icon::folder_open) + "  Open").c_str(), nullptr, false, !current))
                        request_open_scene(relative);
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", current ? "Open now" : "Double-click to open");
                auto* draw = ImGui::GetWindowDrawList();
                const auto y = (min.y + max.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f;
                draw->AddText({min.x + 5.0f, y}, current ? theme::color::accent : theme::color::muted, icon::file);
                draw->AddText({min.x + 26.0f, y}, theme::color::text, relative.generic_string().c_str());
                ImGui::PopID();
            });
            column("meshes", "MESHES", m_shown_rows[1], [&](size_t index) { draw_asset_row(m_asset_rows[index]); });
            column("materials", "MATERIALS", m_shown_rows[2], [&](size_t index) { draw_asset_row(m_asset_rows[index]); });
            column("scripts", "SCRIPTS", m_shown_rows[3], [&](size_t index) { draw_asset_row(m_asset_rows[index]); });
            ImGui::EndTable();
        }
        if (std::exchange(m_rescan, false)) scan_project();
    }
    ImGui::End();
}

} // namespace maya::editor
