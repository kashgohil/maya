#include "editor_shell.hpp"
#include "editor_icons.hpp"
#include "shell_detail.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace maya::editor {
using namespace detail;
namespace {
constexpr std::array<double, 4> window_background{0.06, 0.06, 0.07, 1.0};
constexpr float fallback_font_size = 13.0f; // ImGui's built-in pixel font is drawn for 13 px

/// ImGui asserts on data that is not a font, so check the TrueType/OpenType signature first.
bool font_signature(const std::string& data) {
    if (data.size() < 256) return false;
    const auto tag = data.substr(0, 4);
    return tag == std::string("\0\1\0\0", 4) || tag == "true" || tag == "OTTO";
}


ImGuiKey imgui_key(KeyCode key) {
    const auto code = static_cast<int>(key);
    if (code >= int(KeyCode::A) && code <= int(KeyCode::Z)) return ImGuiKey(ImGuiKey_A + (code - int(KeyCode::A)));
    if (code >= int(KeyCode::Num0) && code <= int(KeyCode::Num9)) return ImGuiKey(ImGuiKey_0 + (code - int(KeyCode::Num0)));
    if (code >= int(KeyCode::F1) && code <= int(KeyCode::F12)) return ImGuiKey(ImGuiKey_F1 + (code - int(KeyCode::F1)));
    switch (key) {
    case KeyCode::Space: return ImGuiKey_Space;
    case KeyCode::Apostrophe: return ImGuiKey_Apostrophe;
    case KeyCode::Comma: return ImGuiKey_Comma;
    case KeyCode::Minus: return ImGuiKey_Minus;
    case KeyCode::Period: return ImGuiKey_Period;
    case KeyCode::Slash: return ImGuiKey_Slash;
    case KeyCode::Semicolon: return ImGuiKey_Semicolon;
    case KeyCode::Equal: return ImGuiKey_Equal;
    case KeyCode::LeftBracket: return ImGuiKey_LeftBracket;
    case KeyCode::Backslash: return ImGuiKey_Backslash;
    case KeyCode::RightBracket: return ImGuiKey_RightBracket;
    case KeyCode::GraveAccent: return ImGuiKey_GraveAccent;
    case KeyCode::Escape: return ImGuiKey_Escape;
    case KeyCode::Enter: return ImGuiKey_Enter;
    case KeyCode::Tab: return ImGuiKey_Tab;
    case KeyCode::Backspace: return ImGuiKey_Backspace;
    case KeyCode::Insert: return ImGuiKey_Insert;
    case KeyCode::Delete: return ImGuiKey_Delete;
    case KeyCode::Right: return ImGuiKey_RightArrow;
    case KeyCode::Left: return ImGuiKey_LeftArrow;
    case KeyCode::Down: return ImGuiKey_DownArrow;
    case KeyCode::Up: return ImGuiKey_UpArrow;
    case KeyCode::PageUp: return ImGuiKey_PageUp;
    case KeyCode::PageDown: return ImGuiKey_PageDown;
    case KeyCode::Home: return ImGuiKey_Home;
    case KeyCode::End: return ImGuiKey_End;
    case KeyCode::LeftShift: return ImGuiKey_LeftShift;
    case KeyCode::LeftControl: return ImGuiKey_LeftCtrl;
    case KeyCode::LeftAlt: return ImGuiKey_LeftAlt;
    case KeyCode::LeftSuper: return ImGuiKey_LeftSuper;
    case KeyCode::RightShift: return ImGuiKey_RightShift;
    case KeyCode::RightControl: return ImGuiKey_RightCtrl;
    case KeyCode::RightAlt: return ImGuiKey_RightAlt;
    case KeyCode::RightSuper: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

const char* source_name(DiagnosticSource source) {
    switch (source) {
    case DiagnosticSource::scene: return "scene";
    case DiagnosticSource::viewport: return "viewport";
    case DiagnosticSource::renderer: return "renderer";
    case DiagnosticSource::gpu: return "gpu";
    case DiagnosticSource::ui: return "ui";
    case DiagnosticSource::edit: return "edit";
    case DiagnosticSource::project: return "project";
    case DiagnosticSource::asset: return "asset";
    case DiagnosticSource::play: return "play";
    case DiagnosticSource::script: return "script";
    }
    return "?";
}

} // namespace

PixelSize viewport_pixels(float width_points, float height_points, float scale) noexcept {
    const auto pixels = [scale](float points) -> uint32_t {
        const auto value = std::floor(points * scale);
        return std::isfinite(value) && value >= 1.0f ? static_cast<uint32_t>(std::min(value, 16384.0f)) : 0u;
    };
    if (!(scale > 0.0f)) return {};
    return {pixels(width_points), pixels(height_points)};
}

void DiagnosticLog::add(DiagnosticSource source, std::string message, uint64_t frame) {
    const auto found = std::ranges::find_if(m_entries, [&](const DiagnosticEntry& entry) {
        return entry.source == source && entry.message == message;
    });
    if (found != m_entries.end()) {
        ++found->count;
        found->last_frame = frame;
        return;
    }
    if (m_entries.size() == capacity) m_entries.pop_front();
    m_entries.push_back({source, std::move(message), 1, frame});
}

size_t DiagnosticLog::count(DiagnosticSource source) const {
    return size_t(std::ranges::count(m_entries, source, &DiagnosticEntry::source));
}

EditorShell::EditorShell(GraphicsDevice& device, std::string renderer_shader, std::string ui_shader,
                         PlatformServices services, EditorFonts fonts)
    : m_device(device), m_services(std::move(services)), m_font_data(std::move(fonts)),
      m_renderer(device, std::move(renderer_shader)),
      m_ui(device, std::move(ui_shader)), m_viewport(device, {Format::rgba8_unorm, false, "editor viewport"}),
      m_camera(EditorCamera::looking_at({3.0f, 2.2f, 4.5f}, {0.0f, 0.0f, 0.0f})) {
    auto* previous = ImGui::GetCurrentContext();
    m_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; // layout persistence belongs to project settings, not the working directory
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendPlatformName = "maya_desktop";
    io.BackendRendererName = "maya_rhi";
    theme::apply(ImGui::GetStyle());
    style_gizmo();
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = this;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        auto* self = static_cast<EditorShell*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        self->m_clipboard = self->m_services.get_clipboard ? self->m_services.get_clipboard() : std::string{};
        return self->m_clipboard.c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        auto* self = static_cast<EditorShell*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        if (self->m_services.set_clipboard) self->m_services.set_clipboard(text ? text : "");
    };
    m_viewport_texture = m_ui.add_texture();
    ImGui::SetCurrentContext(previous ? previous : m_context);
}

EditorShell::~EditorShell() {
    if (ImGui::GetCurrentContext() == m_context) ImGui::SetCurrentContext(nullptr);
    ImGui::DestroyContext(m_context);
}

void EditorShell::rebuild_fonts(float scale) {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Rasterize at framebuffer resolution; FontGlobalScale draws them at their size in points.
    const auto load = [&](std::string& data, float points, const char* name) -> ImFont* {
        if (font_signature(data)) {
            auto config = ImFontConfig{};
            config.FontDataOwnedByAtlas = false; // the shell keeps the TrueType data alive
            config.OversampleH = 2;
            config.OversampleV = 1;
            if (auto* font = io.Fonts->AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()),
                                                            std::round(points * scale), &config, theme::text_ranges))
                return font;
        }
        if (!data.empty())
            m_log.add(DiagnosticSource::ui, std::string("The ") + name + " font could not be loaded; using the built-in font", m_frame);
        auto config = ImFontConfig{};
        config.SizePixels = std::round(fallback_font_size * scale);
        return io.Fonts->AddFontDefault(&config);
    };
    // Icons are merged into the text fonts, so a label can mix both. Fixed advance keeps icon columns aligned.
    auto icons_ok = font_signature(m_font_data.icons);
    const auto merge_icons = [&](float points) {
        if (!icons_ok) return;
        auto config = ImFontConfig{};
        config.MergeMode = true;
        config.FontDataOwnedByAtlas = false;
        config.PixelSnapH = true;
        config.GlyphMinAdvanceX = std::round(points * 1.15f * scale);
        config.GlyphOffset = {0.0f, std::round(1.5f * scale)};
        if (!io.Fonts->AddFontFromMemoryTTF(m_font_data.icons.data(), static_cast<int>(m_font_data.icons.size()),
                                            std::round(points * 1.15f * scale), &config, icon::ranges))
            icons_ok = false;
    };
    m_fonts.body = load(m_font_data.regular, theme::body_size, "regular UI"); // first: ImGui's default font
    merge_icons(theme::body_size);
    m_fonts.strong = load(m_font_data.semibold, theme::body_size, "semibold UI");
    merge_icons(theme::body_size);
    m_fonts.caption = load(m_font_data.semibold, theme::caption_size, "caption");
    m_fonts.mono = load(m_font_data.mono, theme::mono_size, "monospace");
    if (!icons_ok && !m_font_data.icons.empty())
        m_log.add(DiagnosticSource::ui, "The icon font could not be loaded; panels show text only", m_frame);
    io.FontGlobalScale = 1.0f / scale;
    if (auto error = m_ui.upload_fonts(*io.Fonts)) m_log.add(DiagnosticSource::ui, error.message, m_frame);
    m_font_scale = scale;
}

void EditorShell::apply_input(const RoutedInput& routed) {
    auto& io = ImGui::GetIO();
    for (const auto& event : routed.ui) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, MouseMoveEvent>) {
                io.AddMousePosEvent(e.x, e.y);
            } else if constexpr (std::is_same_v<T, MouseButtonEvent>) {
                const int button = e.button == MouseButton::left ? 0 : e.button == MouseButton::right ? 1
                    : e.button == MouseButton::middle ? 2 : 3;
                io.AddMouseButtonEvent(button, e.down);
            } else if constexpr (std::is_same_v<T, KeyEvent>) {
                io.AddKeyEvent(ImGuiMod_Ctrl, has_modifier(e.modifiers, KeyModifiers::control) ||
                    (e.down && (e.key == KeyCode::LeftControl || e.key == KeyCode::RightControl)));
                io.AddKeyEvent(ImGuiMod_Shift, has_modifier(e.modifiers, KeyModifiers::shift) ||
                    (e.down && (e.key == KeyCode::LeftShift || e.key == KeyCode::RightShift)));
                io.AddKeyEvent(ImGuiMod_Alt, has_modifier(e.modifiers, KeyModifiers::alt) ||
                    (e.down && (e.key == KeyCode::LeftAlt || e.key == KeyCode::RightAlt)));
                io.AddKeyEvent(ImGuiMod_Super, has_modifier(e.modifiers, KeyModifiers::super) ||
                    (e.down && (e.key == KeyCode::LeftSuper || e.key == KeyCode::RightSuper)));
                if (const auto key = imgui_key(e.key); key != ImGuiKey_None) io.AddKeyEvent(key, e.down);
            } else if constexpr (std::is_same_v<T, TextEvent>) {
                io.AddInputCharacter(e.codepoint);
            } else if constexpr (std::is_same_v<T, ScrollEvent>) {
                io.AddMouseWheelEvent(e.x, e.y);
            } else if constexpr (std::is_same_v<T, FocusEvent>) {
                io.AddFocusEvent(e.focused);
            }
        }, event);
    }
}

void EditorShell::update(float delta_time, const std::vector<InputEvent>& events, const WindowMetrics& metrics) {
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_context);
    auto& io = ImGui::GetIO();
    if (metrics.width <= 0.0f || metrics.height <= 0.0f || metrics.framebuffer_width == 0 || metrics.framebuffer_height == 0) {
        if (!m_minimized) {
            m_minimized = true;
            m_log.add(DiagnosticSource::viewport, "Window minimized or zero-sized: viewport paused", m_frame);
            if (const auto release = m_router.cancel()) m_capture_request = release;
        }
        m_frame_ready = false;
        ImGui::SetCurrentContext(previous);
        return;
    }
    if (m_minimized) {
        m_minimized = false;
        m_log.add(DiagnosticSource::viewport, "Window restored: viewport resumed", m_frame);
    }
    const auto scale_x = float(metrics.framebuffer_width) / metrics.width;
    const auto scale_y = float(metrics.framebuffer_height) / metrics.height;
    if (scale_x != m_font_scale) rebuild_fonts(scale_x);
    io.DisplaySize = {metrics.width, metrics.height};
    io.DisplayFramebufferScale = {scale_x, scale_y};
    io.DeltaTime = std::isfinite(delta_time) && delta_time > 0.0f ? delta_time : 1.0f / 60.0f;
    // A few times a second, changed script files are reloaded (the player never watches files).
    if ((m_script_check_timer += io.DeltaTime) >= 0.25f) check_script_files();

    const auto routed = m_router.route(events, {m_viewport_hovered, showing_game()});
    if (routed.capture) m_capture_request = routed.capture;
    apply_input(routed);
    if (routed.navigation_started && routed.navigation_point)
        start_navigation(m_router.navigation(), {routed.navigation_point->x, routed.navigation_point->y});
    m_camera.update(routed.navigation, io.DeltaTime);
    if (const auto mode = m_router.navigation(); mode == NavigationMode::orbit || mode == NavigationMode::pan || mode == NavigationMode::zoom)
        m_pivot_distance = std::max((m_camera.pivot - m_camera.position).length(), 0.05f);
    update_play(routed, io.DeltaTime); // before the UI, so it shows this frame's ticks

    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_layout.controls.clear();
    // Focusing the viewport deactivates any text field, so it stops receiving keys.
    if (routed.navigation_started || routed.game_started) ImGui::SetWindowFocus(viewport_title.c_str());
    draw_top_bar();
    draw_status_bar();
    const auto dockspace = ImGui::GetID("EditorDockSpace");
    if (!m_layout_built) build_dock_layout(dockspace);
    // Tab bars are drawn here: taller tabs, labels muted until decorate_tabs brightens the visible ones.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, theme::tab_padding);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    draw_hierarchy();
    draw_viewport();
    draw_inspector();
    draw_assets();
    draw_diagnostics();
    // New windows take focus as they are created, and a focused docked window brings its tab to the
    // front. Start with the viewport focused, so the bottom panels open on Assets, not on Diagnostics
    // (the panel created last).
    if (std::exchange(m_focus_viewport, false)) ImGui::SetWindowFocus(viewport_title.c_str());
    draw_prompts();
    draw_collision_groups();
    handle_shortcuts();
    // An edit group whose control is no longer active (e.g. it was removed mid-drag) must not stay open.
    if (m_edit_group_open && !ImGui::IsAnyItemActive()) {
        m_scene->end_group();
        m_edit_group_open = false;
    }
    theme::decorate_tabs(panel_titles, IM_ARRAYSIZE(panel_titles)); // after every tab bar is drawn
    ImGui::Render();
    // io.WantTextInput describes the previous frame; this is whether a text field is active now.
    m_ui_wants_text = ImGui::GetCurrentContext()->WantTextInputNextFrame == 1;
    if (const auto cursor = ImGui::GetMouseCursor(); cursor != m_cursor && !m_router.navigating() && m_services.set_cursor) {
        m_cursor = cursor;
        m_services.set_cursor(cursor == ImGuiMouseCursor_TextInput ? CursorShape::text
            : cursor == ImGuiMouseCursor_Hand ? CursorShape::hand
            : cursor == ImGuiMouseCursor_ResizeEW ? CursorShape::resize_horizontal
            : cursor == ImGuiMouseCursor_ResizeNS ? CursorShape::resize_vertical : CursorShape::arrow);
    }
    m_frame_ready = true;
    ++m_frame;
    ImGui::SetCurrentContext(previous ? previous : m_context);
}

void EditorShell::build_dock_layout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
    auto center = dockspace;
    const auto left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const auto right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    const auto bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow(hierarchy_title.c_str(), left);
    ImGui::DockBuilderDockWindow(inspector_title.c_str(), right);
    ImGui::DockBuilderDockWindow(assets_title.c_str(), bottom);
    ImGui::DockBuilderDockWindow(diagnostics_title.c_str(), bottom);
    ImGui::DockBuilderDockWindow(viewport_title.c_str(), center);
    ImGui::DockBuilderFinish(dockspace);
    m_layout_built = true;
    m_focus_viewport = true;
}

void EditorShell::draw_top_bar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color::background);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::BeginViewportSideBar("##top_bar", ImGui::GetMainViewport(), ImGuiDir_Up, 40.0f, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        const auto origin = ImGui::GetWindowPos();
        const auto height = ImGui::GetWindowHeight();
        // The Maya mark: two slanted panels, as in maya.svg.
        const auto x = origin.x + 16.0f, y = origin.y + height * 0.5f - 8.0f;
        const ImVec2 left[] = {{x, y + 2.3f}, {x + 4.3f, y}, {x + 4.1f, y + 13.7f}, {x, y + 11.6f}};
        const ImVec2 right[] = {{x + 5.3f, y + 2.8f}, {x + 9.2f, y + 4.9f}, {x + 9.2f, y + 14.3f}, {x + 5.7f, y + 16.0f}};
        draw->AddConvexPolyFilled(left, 4, theme::color::rgb(0xECE6DA));
        draw->AddConvexPolyFilled(right, 4, theme::color::rgb(0xECE6DA));
        ImGui::SetCursorPos({36.0f, (height - ImGui::GetTextLineHeight()) * 0.5f});
        ImGui::PushFont(m_fonts.strong);
        ImGui::TextUnformatted("Maya");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextUnformatted("/");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 14.0f);
        if (m_project) {
            // The project's name opens the project menu: its settings.
            ImGui::PushStyleColor(ImGuiCol_Button, 0u);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f, 3.0f});
            if (ImGui::Button((m_project->name() + "###project_menu_button").c_str())) ImGui::OpenPopup("project_menu");
            m_layout.controls.push_back({"project_menu", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::BeginPopup("project_menu")) {
                if (ImGui::MenuItem((std::string(icon::stack) + "  Collision groups").c_str())) m_groups_open = true;
                ImGui::EndPopup();
            }
            ImGui::SameLine(0.0f, 14.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
            ImGui::TextUnformatted("/");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 8.0f);
        }
        if (m_scene) {
            // The scene's name opens the scene menu: the project's scenes, New, Save, and Save as.
            const auto name = m_scene_path.empty() ? std::string("Untitled") : m_scene_path.filename().string();
            const auto label = std::string(icon::file) + "  " + name + "  " + icon::caret_down;
            ImGui::PushStyleColor(ImGuiCol_Button, 0u);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f, 3.0f});
            if (ImGui::Button((label + "###scene_menu_button").c_str())) ImGui::OpenPopup("scene_menu");
            m_layout.controls.push_back({"scene_menu", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !m_scene_path.empty() && m_project)
                ImGui::SetTooltip("%s", m_project->relative(m_scene_path).generic_string().c_str());
            draw_scene_menu();
            if (m_scene->dirty()) {
                ImGui::SameLine(0.0f, 10.0f);
                theme::pill(m_fonts, "modified", theme::color::warning, theme::color::rgb(0xF5B454, 28));
            }
            // Undo and redo, labelled with the action they will reverse or repeat.
            ImGui::SameLine(0.0f, 18.0f);
            const auto history_button = [&](const char* glyph, const char* id, bool enabled, const std::string& tip) {
                ImGui::PushStyleColor(ImGuiCol_Button, 0u);
                ImGui::PushStyleColor(ImGuiCol_Text, enabled ? theme::color::muted : theme::color::faint);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f, 3.0f});
                ImGui::PushID(id);
                ImGui::BeginDisabled(!enabled);
                const auto pressed = ImGui::Button(glyph);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip.c_str());
                ImGui::PopID();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                return pressed;
            };
            if (history_button(icon::undo, "undo", m_scene->can_undo(),
                               m_scene->can_undo() ? "Undo " + m_scene->undo_label() + "   \xE2\x8C\x98Z" : "Nothing to undo"))
                report(m_scene->undo(), "Undo");
            ImGui::SameLine(0.0f, 2.0f);
            if (history_button(icon::redo, "redo", m_scene->can_redo(),
                               m_scene->can_redo() ? "Redo " + m_scene->redo_label() + "   \xE2\x87\xA7\xE2\x8C\x98Z" : "Nothing to redo"))
                report(m_scene->redo(), "Redo");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
            ImGui::TextUnformatted("No scene open");
            ImGui::PopStyleColor();
        }
        // Play controls in the middle of the bar.
        if (m_scene) {
            ImGui::SameLine();
            const auto controls_width = 3.0f * (ImGui::GetFrameHeight() + 8.0f);
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - controls_width) * 0.5f));
            draw_play_controls();
        }
        const auto frame = format("%.1f ms", ImGui::GetIO().DeltaTime * 1000.0f);
        ImGui::PushFont(m_fonts.mono);
        const auto width = ImGui::CalcTextSize(frame.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - width - 16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextUnformatted(frame.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        // While playing, the bar's rule turns to the accent, so play is never mistaken for editing.
        draw->AddLine({origin.x, origin.y + height - 1.0f}, {origin.x + ImGui::GetWindowWidth(), origin.y + height - 1.0f},
                      m_play ? theme::color::accent : theme::color::border, m_play ? 2.0f : 1.0f);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void EditorShell::draw_status_bar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color::background);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::BeginViewportSideBar("##status_bar", ImGui::GetMainViewport(), ImGuiDir_Down, 26.0f, flags)) {
        const auto origin = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + ImGui::GetWindowWidth(), origin.y}, theme::color::border);
        ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetTextLineHeight()) * 0.5f);
        const auto problems = m_frame_problems.size() + m_log.count(DiagnosticSource::renderer) +
                              m_log.count(DiagnosticSource::gpu) + m_log.count(DiagnosticSource::ui);
        auto state = std::string("Ready");
        auto tone = theme::color::success;
        auto glyph = icon::check_circle;
        if (m_router.navigating()) {
            const auto mode = m_router.navigation();
            state = mode == NavigationMode::orbit ? "Orbiting" : mode == NavigationMode::pan ? "Panning"
                    : mode == NavigationMode::zoom ? "Zooming" : "Flying";
            tone = theme::color::accent;
            glyph = mode == NavigationMode::orbit ? icon::rotate : icon::arrows_move;
        }
        else if (m_router.game_has_input()) {
            state = "The game has the mouse and keyboard  \xC2\xB7  Esc to take them back";
            tone = theme::color::accent;
            glyph = icon::game_controller;
        } else if (m_play) {
            const auto paused = m_play->clock().paused();
            state = format("%s  \xC2\xB7  tick %llu  \xC2\xB7  %.2f s", paused ? "Paused" : "Playing",
                           static_cast<unsigned long long>(m_play->clock().tick()), m_play->clock().time());
            tone = theme::color::accent;
            glyph = paused ? icon::pause : icon::play;
        } else if (problems) {
            state = std::to_string(problems) + (problems == 1 ? " problem" : " problems");
            tone = theme::color::danger;
            glyph = icon::warning;
        }
        icon_text(glyph, tone, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextUnformatted(state.c_str());
        ImGui::PopStyleColor();
        const auto scale = ImGui::GetIO().DisplayFramebufferScale.x;
        const auto details = m_viewport_request.empty()
            ? format("%zu objects", m_extraction.mesh_renderers)
            : format("%zu objects   %u \xC3\x97 %u   %.0f\xC3\x97", m_extraction.mesh_renderers,
                     m_viewport_request.width, m_viewport_request.height, scale);
        ImGui::PushFont(m_fonts.mono);
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(details.c_str()).x - 16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextUnformatted(details.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void EditorShell::report(const EditResult& result, const std::string& action) {
    if (!result && result.error != "Nothing to change")
        m_log.add(DiagnosticSource::edit, action + " failed: " + result.error, m_frame);
}

void EditorShell::start_rename(EntityId id) {
    if (!m_scene || !m_scene->record(id) || m_scene->locked()) return;
    m_renaming = id;
    std::snprintf(m_rename_buffer, sizeof(m_rename_buffer), "%s", m_scene->display_name(id).c_str());
    m_rename_focus = true;
}

void EditorShell::draw_create_menu(std::optional<EntityId> parent) {
    const auto create = [&](const char* name, std::vector<ComponentValue> extra) {
        const auto result = m_scene->create(name, parent, std::move(extra));
        report(result, std::string("Create ") + name);
        if (result) start_rename(*m_scene->primary());
    };
    if (ImGui::MenuItem((std::string(icon::circle_dashed) + "  Empty").c_str())) create("Entity", {});
    if (ImGui::MenuItem((std::string(icon::video_camera) + "  Camera").c_str())) create("Camera", {CameraComponent{}});
    if (ImGui::MenuItem((std::string(icon::sun) + "  Directional light").c_str())) create("Light", {LightComponent{}});
}

void EditorShell::draw_hierarchy_row(EntityId id) {
    auto& scene = *m_scene;
    const auto* record = scene.record(id);
    if (!record) return;
    const auto has = [&](ComponentId component) {
        return std::ranges::any_of(record->components, [&](const ComponentValue& v) { return component_id(v) == component; });
    };
    // An icon marks what the entity is: camera, light, mesh, or other.
    const auto camera = has(ComponentId::camera), light = has(ComponentId::light), mesh = has(ComponentId::mesh_renderer);
    const auto* glyph = camera ? icon::video_camera : light ? icon::sun : mesh ? icon::cube : icon::circle_dashed;
    const auto tone = camera ? theme::color::accent : light ? theme::color::warning
        : mesh ? theme::color::rgb(0xA3A7B0) : theme::color::faint;
    const auto& children = scene.children(id);
    auto flags = ImGuiTreeNodeFlags{ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen |
                                    ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_OpenOnArrow};
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (scene.selected(id)) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(static_cast<int>(id.low ^ (id.high << 7)));
    if (m_reveal && scene.is_ancestor(id, *m_reveal)) ImGui::SetNextItemOpen(true);
    const auto opened = ImGui::TreeNodeEx("entity", flags, "%s", "");
    const auto row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
    m_layout.hierarchy_rows.push_back({id, row_min, row_max});
    const auto& io = ImGui::GetIO();
    if (m_reveal && id == *m_reveal) {
        ImGui::SetScrollHereY(0.5f);
        m_reveal.reset();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        scene.select(id, io.KeyCtrl ? SelectMode::toggle : io.KeyShift ? SelectMode::add : SelectMode::replace);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        start_rename(id);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !scene.selected(id)) scene.select(id);

    // Drag onto a row's upper or lower quarter to place before or after it; onto its middle to parent.
    if (!scene.locked() && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("MAYA_ENTITY", &id, sizeof(id));
        icon_text(glyph, tone);
        ImGui::TextUnformatted(scene.display_name(id).c_str());
        ImGui::EndDragDropSource();
    }
    if (const auto* dragged = ImGui::GetDragDropPayload(); dragged && dragged->IsDataType("MAYA_ASSET")) {
        if (ImGui::BeginDragDropTarget()) {
            accept_asset_drop(id); // assign the mesh or material to this entity
            ImGui::EndDragDropTarget();
        }
    } else if (ImGui::BeginDragDropTarget()) {
        const auto height = row_max.y - row_min.y;
        const auto y = io.MousePos.y - row_min.y;
        const auto placement = y < height * 0.25f ? Placement::before : y > height * 0.75f ? Placement::after : Placement::inside;
        auto* draw = ImGui::GetWindowDrawList();
        if (placement == Placement::inside) draw->AddRect(row_min, row_max, theme::color::accent, 4.0f);
        else {
            const auto line = placement == Placement::before ? row_min.y : row_max.y;
            draw->AddLine({row_min.x + 8.0f, line}, {row_max.x - 4.0f, line}, theme::color::accent, 2.0f);
        }
        if (const auto* payload = ImGui::AcceptDragDropPayload("MAYA_ENTITY", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            auto moved = EntityId{};
            std::memcpy(&moved, payload->Data, sizeof(moved));
            if (moved != id) report(scene.move(moved, id, placement), "Move " + scene.display_name(moved));
        }
        ImGui::EndDragDropTarget();
    }
    if (!scene.locked() && ImGui::BeginPopupContextItem("row")) {
        if (ImGui::MenuItem((std::string(icon::pencil) + "  Rename").c_str(), "F2")) start_rename(id);
        if (ImGui::MenuItem((std::string(icon::copy) + "  Duplicate").c_str(), "\xE2\x8C\x98" "D"))
            report(scene.duplicate_selection(), "Duplicate");
        if (ImGui::MenuItem((std::string(icon::unparent) + "  Move to root").c_str(), nullptr, false, record->parent.has_value()))
            report(scene.move(id, std::nullopt), "Move " + scene.display_name(id));
        if (has(ComponentId::transform) && ImGui::BeginMenu((std::string(icon::plus) + "  Create child").c_str())) {
            draw_create_menu(id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem((std::string(icon::trash) + "  Delete").c_str(), "\xE2\x8C\xAB"))
            report(scene.delete_selection(), "Delete");
        ImGui::EndPopup();
    }

    ImGui::SameLine(0.0f, 0.0f);
    icon_text(glyph, tone);
    if (m_renaming == id) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_rename_focus) {
            ImGui::SetKeyboardFocusHere();
            m_rename_focus = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f, 1.0f});
        const auto entered = ImGui::InputText("##rename", m_rename_buffer, sizeof(m_rename_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleVar();
        if (entered || ImGui::IsItemDeactivatedAfterEdit()) report(scene.rename(id, m_rename_buffer), "Rename");
        if (ImGui::IsItemDeactivated() || entered) m_renaming.reset();
    } else {
        ImGui::TextUnformatted(scene.display_name(id).c_str());
    }
    if (opened) {
        for (const auto child : std::vector<EntityId>(children)) draw_hierarchy_row(child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorShell::draw_hierarchy() {
    const auto open = begin_panel(hierarchy_title);
    m_layout.hierarchy_rows.clear();
    if (open && !m_scene) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextWrapped("No scene is open.");
        ImGui::PopStyleColor();
    }
    if (open && m_scene) {
        auto& scene = *m_scene;
        // Header: caption and entity count, with a create button on the right.
        ImGui::PushFont(m_fonts.caption);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("SCENE  %zu", scene.world().size());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
        ImGui::PushStyleColor(ImGuiCol_Button, 0u);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f, 3.0f});
        ImGui::BeginDisabled(scene.locked());
        if (ImGui::Button(icon::plus)) ImGui::OpenPopup("create");
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create an entity");
        if (ImGui::BeginPopup("create")) {
            const auto primary = scene.primary();
            draw_create_menu(std::nullopt);
            if (primary && ImGui::BeginMenu(("Child of " + scene.display_name(*primary)).c_str())) {
                draw_create_menu(primary);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f, 4.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0f, 2.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 14.0f);
        for (const auto id : std::vector<EntityId>(scene.roots())) draw_hierarchy_row(id);
        ImGui::PopStyleVar(3);

        // The empty space below the rows: click to clear the selection, drop to move to the root.
        const auto space = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##empty", {std::max(space.x, 1.0f), std::max(space.y, 24.0f)});
        if (ImGui::IsItemClicked()) scene.clear_selection();
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("MAYA_ENTITY")) {
                auto moved = EntityId{};
                std::memcpy(&moved, payload->Data, sizeof(moved));
                report(scene.move(moved, std::nullopt), "Move " + scene.display_name(moved));
            }
            accept_asset_drop({}); // a mesh dropped here is placed in view
            ImGui::EndDragDropTarget();
        }
        if (!scene.locked() && ImGui::BeginPopupContextItem("empty")) {
            draw_create_menu(std::nullopt);
            ImGui::EndPopup();
        }
        // Keys that act on the selection while the hierarchy has focus.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !m_renaming) {
            if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && !scene.locked())
                report(scene.delete_selection(), "Delete");
            if ((ImGui::IsKeyPressed(ImGuiKey_F2, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) && scene.primary())
                start_rename(*scene.primary());
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) scene.clear_selection();
        }
    }
    ImGui::End();
}

void EditorShell::handle_shortcuts() {
    if (!m_scene || ImGui::GetIO().WantTextInput || m_renaming || m_prompt != EditorPrompt::none) return;
    constexpr auto global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) {
        if (m_play) stop_play();
        else start_play();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, global)) toggle_pause();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P, global)) step_play();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) save_or_ask();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) ask_save_as();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) request_new_scene();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) report(m_scene->undo(), "Undo");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global))
        report(m_scene->redo(), "Redo");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) report(m_scene->duplicate_selection(), "Duplicate");
}

void EditorShell::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    const auto open = begin_panel(viewport_title, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    m_viewport_hovered = false;
    m_layout.viewport_min = m_layout.viewport_max = {0, 0};
    auto request = PixelSize{};
    // An appearing window (first frame, or a tab just shown) has no final size yet: skip it rather
    // than allocate a target for a size that is about to change.
    if (open && m_scene && !ImGui::IsWindowAppearing()) {
        const auto available = ImGui::GetContentRegionAvail();
        const auto scale = ImGui::GetIO().DisplayFramebufferScale.x;
        request = viewport_pixels(available.x, available.y, scale);
        if (!request.empty()) {
            // Drawn at exactly one texel per framebuffer pixel, so the image is never stretched.
            ImGui::Image(m_viewport_texture, {float(request.width) / scale, float(request.height) / scale});
            m_viewport_hovered = ImGui::IsItemHovered();
            m_layout.viewport_min = ImGui::GetItemRectMin();
            m_layout.viewport_max = ImGui::GetItemRectMax();
            if (!m_play) accept_viewport_drop(); // meshes are placed where they are dropped, materials assigned
            // Hint: a quiet pill in the corner, brighter while flying or while the game has the input.
            const auto flying = m_router.navigating() || m_router.game_has_input();
            const auto game = showing_game();
            const auto mode = m_router.navigation();
            auto hint = std::vector<HintItem>{};
            if (m_router.game_has_input()) hint = {{"", icon::game_controller, "The game has the mouse and keyboard  \xC2\xB7  Esc to take them back"}};
            else if (game) hint = {{"", icon::game_controller, "Click to play with the mouse and keyboard"}};
            else if (mode == NavigationMode::fly) hint = {{"", icon::arrows_move, "WASD move  \xC2\xB7  Q/E down/up  \xC2\xB7  Shift faster  \xC2\xB7  Esc stop"}};
            else if (m_router.navigating())
                hint = {{"", mode == NavigationMode::orbit ? icon::rotate : icon::arrows_move,
                         std::string("Drag to ") + (mode == NavigationMode::orbit ? "orbit" : mode == NavigationMode::pan ? "pan" : "zoom") +
                             "  \xC2\xB7  Esc stop"}};
            else {
                if (!m_play) hint.push_back({"", nullptr, "Click to select"});
                hint.insert(hint.end(), {{"", icon::mouse_right, "Fly"}, {"Alt", icon::mouse_left, "Orbit"},
                                         {"", icon::mouse_middle, "Pan"}, {"", icon::mouse_scroll, "Dolly"}});
            }
            auto* draw = ImGui::GetWindowDrawList();
            const auto size = ImVec2{hint_line(nullptr, {}, hint, 0), ImGui::GetTextLineHeight()};
            const auto corner = ImVec2{m_layout.viewport_min.x + 12.0f, m_layout.viewport_max.y - size.y - 22.0f};
            draw->AddRectFilled(corner, {corner.x + size.x + 24.0f, corner.y + size.y + 10.0f},
                                theme::color::rgb(0x0B0C0E, 190), 8.0f);
            hint_line(draw, {corner.x + 12.0f, corner.y + 5.0f}, hint, flying ? theme::color::text : theme::color::muted);
            if (m_play) {
                draw_view_toggle();
            } else if (const auto view = make_render_view(m_camera.camera, m_camera.pose(), request.width, request.height)) {
                draw_viewport_tools(*view, m_layout.viewport_min, m_layout.viewport_max);
            }
        }
    } else if (open) {
        ImGui::SetCursorPos({16.0f, 14.0f});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextUnformatted(m_scene ? "" : "No scene is open. Details are in Diagnostics.");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    if (request.empty() && !m_viewport_request.empty() && m_router.navigating())
        if (const auto release = m_router.cancel()) m_capture_request = release; // the viewport disappeared
    m_viewport_request = request;
}

void EditorShell::draw_diagnostics() {
    if (begin_panel(diagnostics_title)) {
        const auto& io = ImGui::GetIO();
        m_stats_age += io.DeltaTime;
        if (m_stats_age >= 0.25f) {
            m_shown_stats = m_device.stats();
            auto& shown = m_shown_performance;
            const auto& p = m_performance;
            shown.interval = p.interval.summary();
            shown.update = p.update.summary();
            shown.wait = p.wait.summary();
            shown.render = p.render.summary();
            shown.submit = p.submit.summary();
            shown.extract = p.extract.summary();
            shown.view = p.view.summary();
            shown.ui = p.ui.summary();
            shown.gpu = p.gpu.summary();
            shown.gpu_reported = m_device.reported_memory();
            shown.process = process_memory();
            shown.assets = m_assets ? m_assets->residency() : AssetResidency{};
            m_stats_age = 0.0f;
        }
        const auto& stats = m_shown_stats;
        const auto scale = io.DisplayFramebufferScale.x;
        theme::caption(m_fonts, "FRAME");
        if (theme::begin_properties("frame")) {
            const auto row = [&](const char* label, const std::string& value) {
                theme::property(label);
                ImGui::AlignTextToFramePadding();
                theme::mono_text(m_fonts, value.c_str());
            };
            row("Viewport", m_viewport_request.empty() ? std::string("hidden")
                : format("%u \xC3\x97 %u px   %.0f\xC3\x97   %llu alloc%s", m_viewport_request.width, m_viewport_request.height,
                         scale, static_cast<unsigned long long>(m_viewport.allocations()), m_viewport_error ? "   error" : ""));
            row("Scene", format("%zu drawn   %zu hidden   %zu skipped", m_extraction.mesh_renderers -
                                m_extraction.hidden - m_extraction.skipped, m_extraction.hidden, m_extraction.skipped));
            row("Frames", format("%llu submitted   %llu waits", static_cast<unsigned long long>(stats.submitted_frames),
                                 static_cast<unsigned long long>(stats.frame_waits)));
            row("Upload", format("%zu / %zu KiB   %llu failed", stats.transient_high_water / 1024,
                                 m_device.options().transient_bytes_per_frame / 1024,
                                 static_cast<unsigned long long>(stats.transient_failures)));
            row("Resources", format("%zu buffers   %zu textures   %zu pending", stats.buffers, stats.textures,
                                    stats.pending_retirements));
            // Slowdown is never hidden: wall time the clock refused and ticks it dropped are shown.
            if (m_play) {
                const auto& clock = m_play->clock();
                row("Play", format("tick %llu   %.2f s rejected   %llu ticks dropped", static_cast<unsigned long long>(clock.tick()),
                                   clock.total_rejected_time(), static_cast<unsigned long long>(clock.total_discarded_ticks())));
                row("Scripts", format("%.0f KiB   %zu reloads", double(play_script_memory()) / 1024.0,
                                      m_play_reloads ? m_play_reloads->applied().size() : size_t{0}));
            }
            theme::end_properties();
        }
        // Performance over the last few seconds: the frame interval, where the CPU time went, the GPU's
        // own execution time, what was drawn, and memory, tracked and platform-reported kept apart.
        ImGui::Dummy({0.0f, 6.0f});
        theme::caption(m_fonts, "PERFORMANCE", format("last %zu frames", m_performance.interval.size()).c_str());
        if (theme::begin_properties("performance")) {
            const auto& shown = m_shown_performance;
            const auto row = [&](const char* label, const std::string& value, const char* tip = nullptr) {
                theme::property(label);
                ImGui::AlignTextToFramePadding();
                theme::mono_text(m_fonts, value.c_str());
                if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };
            const auto mib = [](size_t bytes) { return double(bytes) / (1024.0 * 1024.0); };
            const auto& frame = shown.interval;
            row("Frame", frame.count ? format("%.2f ms   P95 %.2f   P99 %.2f   %.0f fps", frame.mean, frame.p95, frame.p99,
                                              frame.mean > 0.0 ? 1000.0 / frame.mean : 0.0) : std::string("waiting"),
                "Wall time between frames, including waiting for the display. Percentiles are nearest-rank.");
            row("CPU", format("update %.2f   wait %.2f   render %.2f   submit %.2f", shown.update.mean, shown.wait.mean,
                              shown.render.mean, shown.submit.mean), "Mean milliseconds of each part of a frame.");
            row("Render", format("extract %.2f   view %.2f   UI %.2f", shown.extract.mean, shown.view.mean, shown.ui.mean),
                "Mean milliseconds: building the render snapshot, encoding the viewport, encoding the UI.");
            row("GPU", !m_device.gpu_timing_supported() ? std::string("unavailable on this device")
                : shown.gpu.count ? format("%.2f ms   P95 %.2f   P99 %.2f", shown.gpu.mean, shown.gpu.p95, shown.gpu.p99)
                : std::string("waiting for frames"),
                "The GPU's execution time per frame, from its own timestamps when frames complete; never CPU time.");
            row("Drawn", format("%llu draws   %llu instances   %llu triangles   %u passes",
                                static_cast<unsigned long long>(stats.frame_draws), static_cast<unsigned long long>(stats.frame_instances),
                                static_cast<unsigned long long>(stats.frame_triangles), stats.frame_passes));
            row("Tracked", format("buffers %.1f   textures %.1f   upload %.1f   pending %.1f MiB", mib(stats.buffer_bytes),
                                  mib(stats.texture_bytes), mib(stats.upload_bytes), mib(stats.pending_retirement_bytes)),
                "Sizes from resource descriptors: what the engine allocated, not what the platform reports.");
            row("Reported", format("GPU %s   process %s",
                                   shown.gpu_reported ? format("%.1f MiB", mib(*shown.gpu_reported)).c_str() : "unavailable",
                                   shown.process ? format("%.1f MiB", mib(shown.process->footprint)).c_str() : "unavailable"),
                "Platform-reported: the device's allocated size and the process's physical footprint. On unified memory "
                "they overlap; they are not added together.");
            row("Assets", format("%zu meshes (%.1f MiB)   %zu materials   %zu leased", shown.assets.meshes,
                                 mib(shown.assets.mesh_gpu_bytes), shown.assets.materials, shown.assets.leased));
            theme::end_properties();
        }
        if (!m_frame_problems.empty()) {
            ImGui::Dummy({0.0f, 6.0f});
            theme::caption(m_fonts, "SCENE PROBLEMS", std::to_string(m_frame_problems.size()).c_str());
            for (const auto& problem : m_frame_problems) {
                icon_text(icon::warning, theme::color::warning);
                ImGui::TextWrapped("%s", problem.message.c_str());
            }
        }
        ImGui::Dummy({0.0f, 6.0f});
        theme::caption(m_fonts, "LOG", std::to_string(m_log.entries().size()).c_str());
        if (ImGui::BeginTable("log", 3, ImGuiTableFlags_SizingFixedFit)) { // the panel scrolls, not the table
            ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed);
            for (auto it = m_log.entries().rbegin(); it != m_log.entries().rend(); ++it) {
                const auto problem = it->source == DiagnosticSource::renderer || it->source == DiagnosticSource::gpu ||
                                     it->source == DiagnosticSource::ui;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                icon_text(problem ? icon::x_circle : icon::info, problem ? theme::color::danger : theme::color::faint, 6.0f);
                theme::pill(m_fonts, source_name(it->source), problem ? theme::color::danger : theme::color::muted,
                            problem ? theme::color::rgb(0xF2616B, 30) : theme::color::surface);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", it->message.c_str());
                ImGui::TableNextColumn();
                if (it->count > 1) theme::mono_text(m_fonts, format("\xC3\x97%llu", static_cast<unsigned long long>(it->count)).c_str(), true);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorShell::record_frame(const FrameTiming& timing) {
    auto& p = m_performance;
    if (timing.interval > 0.0) p.interval.add(timing.interval);
    p.update.add(timing.update);
    p.wait.add(timing.wait);
    p.render.add(timing.render);
    p.submit.add(timing.submit);
}

void EditorShell::render_viewport() {
    m_ui.set_texture(m_viewport_texture, {});
    m_viewport_error = false;
    if (!m_scene || m_viewport_request.empty()) return;
    const auto allocations = m_viewport.allocations();
    if (auto error = m_viewport.resize(m_viewport_request.width, m_viewport_request.height)) {
        m_viewport_error = true;
        m_log.add(DiagnosticSource::viewport, "Viewport target: " + error.message, m_frame);
        return;
    }
    if (m_viewport.allocations() != allocations)
        m_log.add(DiagnosticSource::viewport, "Viewport target reallocated for a new panel size", m_frame);
    // While playing, the play World is shown, through its camera in the game view.
    const auto& world = m_play ? m_play->world() : m_scene->world();
    // While playing, poses between the last two ticks; the player draws the same way.
    const auto poses = m_play ? m_play->presentation() : PresentationPoses{};
    auto view = std::optional<RenderView>{};
    if (showing_game())
        if (const auto camera = world.find(*m_play->camera()))
            view = extract_render_view(world, *camera, m_viewport.width(), m_viewport.height(), &poses);
    if (!view) view = make_render_view(m_camera.camera, m_camera.pose(), m_viewport.width(), m_viewport.height());
    if (!view) {
        m_viewport_error = true;
        m_log.add(DiagnosticSource::viewport, "The camera has no valid view", m_frame);
        return;
    }
    auto clock = Stopwatch{};
    auto options = RenderExtractOptions{};
    options.poses = &poses;
    auto snapshot = extract_render_snapshot(world, *m_assets, options);
    m_performance.extract.add(clock.milliseconds());
    m_extraction = snapshot.stats;
    m_frame_problems = snapshot.diagnostics;
    clock.restart();
    const auto rendered = m_renderer.render(snapshot, *view, m_viewport);
    m_performance.view.add(clock.milliseconds());
    if (auto error = rendered) {
        m_viewport_error = true;
        m_log.add(DiagnosticSource::renderer, error.message, m_frame);
    }
    m_ui.set_texture(m_viewport_texture, m_viewport.color());
    if (!m_play) m_snapshot = std::move(snapshot); // for picking and outlines next frame, while editing
}

RhiDiagnostic EditorShell::render(TextureHandle destination) {
    for (auto& error : m_device.take_gpu_errors()) {
        std::cerr << "[Editor] GPU: " << error.message << '\n';
        m_log.add(DiagnosticSource::gpu, std::move(error.message), m_frame);
    }
    auto dropped = uint64_t{0};
    for (const auto& timing : m_device.take_gpu_timings(&dropped)) { // frames that completed since last time
        m_performance.gpu.add(timing.milliseconds);
        ++m_performance.gpu_frames;
    }
    m_performance.gpu_dropped += dropped;
    if (!m_frame_ready) return {};
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_context);
    render_viewport();
    const auto* data = ImGui::GetDrawData();
    auto clock = Stopwatch{};
    auto result = data ? m_ui.render(*data, destination, window_background) : RhiDiagnostic{};
    m_performance.ui.add(clock.milliseconds());
    if (result) m_log.add(DiagnosticSource::ui, result.message, m_frame);
    ImGui::SetCurrentContext(previous ? previous : m_context);
    return result;
}

} // namespace maya::editor
