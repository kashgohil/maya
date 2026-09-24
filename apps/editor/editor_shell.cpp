#include "editor_shell.hpp"
#include "editor_icons.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace maya::editor {
namespace {
constexpr std::array<double, 4> window_background{0.06, 0.06, 0.07, 1.0};
constexpr float fallback_font_size = 13.0f; // ImGui's built-in pixel font is drawn for 13 px

/// ImGui asserts on data that is not a font, so check the TrueType/OpenType signature first.
bool font_signature(const std::string& data) {
    if (data.size() < 256) return false;
    const auto tag = data.substr(0, 4);
    return tag == std::string("\0\1\0\0", 4) || tag == "true" || tag == "OTTO";
}

// Panel titles carry an icon; the part after ### is the stable window ID used by the dock layout.
const std::string hierarchy_title = std::string(icon::tree_structure) + "  Hierarchy###Hierarchy";
const std::string viewport_title = std::string(icon::cube_focus) + "  Viewport###Viewport";
const std::string inspector_title = std::string(icon::sliders) + "  Inspector###Inspector";
const std::string assets_title = std::string(icon::folder) + "  Assets###Assets";
const std::string diagnostics_title = std::string(icon::pulse) + "  Diagnostics###Diagnostics";

/// Draws an icon in a color, then continues on the same line.
void icon_text(const char* glyph, ImU32 color, float spacing = 8.0f) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, spacing);
}

std::string format(const char* pattern, auto... values) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, values...);
    return buffer;
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
    }
    return "?";
}

std::string id_text(uint64_t high, uint64_t low) {
    auto text = std::ostringstream{};
    text << std::hex << high << ':' << low;
    return text.str();
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

bool EditorShell::open_scene(const std::filesystem::path& catalog_path, const std::filesystem::path& scene_path) {
    const auto fail = [&](const std::string& message) {
        m_log.add(DiagnosticSource::scene, message, m_frame);
        return false;
    };
    auto catalog_file = std::ifstream(catalog_path);
    if (!catalog_file) return fail("Cannot read asset catalog " + catalog_path.string());
    const auto catalog = read_asset_catalog(catalog_file);
    if (!catalog) return fail(catalog_path.string() + ": " + catalog.diagnostic.message);
    auto assets = std::unique_ptr<AssetRegistry>{};
    try {
        assets = std::make_unique<AssetRegistry>(catalog_path.parent_path(), std::make_unique<FileAssetProvider>(m_device));
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    for (const auto& record : catalog.records)
        if (const auto error = assets->register_asset(record)) return fail(error.message);
    auto opened = open_scene_file(scene_path, asset_property_context(*assets));
    for (const auto& problem : opened.diagnostics) m_log.add(DiagnosticSource::scene, problem.message, m_frame);
    if (!opened) return false;
    m_world = std::move(opened.world);
    m_assets = std::move(assets);
    m_scene_path = scene_path;
    m_log.add(DiagnosticSource::scene, "Opened " + scene_path.filename().string() + " (" +
        std::to_string(m_world->size()) + " entities)", m_frame);
    return true;
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
                                                            std::round(points * scale), &config))
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

    const auto routed = m_router.route(events, {m_viewport_hovered});
    if (routed.capture) m_capture_request = routed.capture;
    apply_input(routed);
    m_camera.update(routed.navigation, io.DeltaTime);

    ImGui::NewFrame();
    // Focusing the viewport deactivates any text field, so it stops receiving keys.
    if (routed.navigation_started) ImGui::SetWindowFocus(viewport_title.c_str());
    draw_top_bar();
    draw_status_bar();
    const auto dockspace = ImGui::GetID("EditorDockSpace");
    if (!m_layout_built) build_dock_layout(dockspace);
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_NoWindowMenuButton);
    draw_hierarchy();
    draw_viewport();
    draw_inspector();
    draw_assets();
    draw_diagnostics();
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
        if (m_world) {
            icon_text(icon::file, theme::color::muted, 6.0f);
            ImGui::TextUnformatted(m_scene_path.filename().string().c_str());
            ImGui::SameLine(0.0f, 10.0f);
            theme::pill(m_fonts, "read-only", theme::color::muted, theme::color::surface);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
            ImGui::TextUnformatted("No scene open");
            ImGui::PopStyleColor();
        }
        const auto frame = format("%.1f ms", ImGui::GetIO().DeltaTime * 1000.0f);
        ImGui::PushFont(m_fonts.mono);
        const auto width = ImGui::CalcTextSize(frame.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - width - 16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextUnformatted(frame.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        draw->AddLine({origin.x, origin.y + height - 1.0f}, {origin.x + ImGui::GetWindowWidth(), origin.y + height - 1.0f},
                      theme::color::border);
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
        if (m_router.navigating()) { state = "Flying"; tone = theme::color::accent; glyph = icon::arrows_move; }
        else if (problems) {
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

void EditorShell::draw_hierarchy() {
    const auto open = ImGui::Begin(hierarchy_title.c_str());
    if (open && !m_world) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextWrapped("No scene is open.");
        ImGui::PopStyleColor();
    }
    if (open && m_world) {
        auto& world = *m_world;
        auto roots = std::vector<std::pair<EntityId, EntityHandle>>{};
        world.for_each_entity([&](EntityHandle entity) {
            if (!world.parent(entity)) roots.emplace_back(*world.persistent_id(entity), entity);
        });
        std::ranges::sort(roots, {}, &std::pair<EntityId, EntityHandle>::first);
        theme::caption(m_fonts, "SCENE", std::to_string(world.size()).c_str());
        const auto draw = [&](const auto& self, EntityHandle entity) -> void {
            const auto id = *world.persistent_id(entity);
            auto label = id_text(id.high, id.low);
            world.with<NameComponent>(entity, [&](const NameComponent& name) { label = name.value; });
            // An icon marks what the entity is: camera, light, mesh, or other.
            const auto camera = world.has<CameraComponent>(entity), light = world.has<LightComponent>(entity);
            const auto mesh = world.has<MeshRendererComponent>(entity);
            const auto* glyph = camera ? icon::video_camera : light ? icon::sun : mesh ? icon::cube : icon::circle_dashed;
            const auto tone = camera ? theme::color::accent : light ? theme::color::warning
                : mesh ? theme::color::rgb(0xA3A7B0) : theme::color::faint;
            const auto children = world.children(entity);
            auto flags = ImGuiTreeNodeFlags{ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen |
                                            ImGuiTreeNodeFlags_FramePadding};
            if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            ImGui::PushID(static_cast<int>(id.low ^ (id.high << 7)));
            const auto opened = ImGui::TreeNodeEx("entity", flags, "%s", "");
            ImGui::SameLine(0.0f, 0.0f);
            icon_text(glyph, tone);
            ImGui::TextUnformatted(label.c_str());
            if (opened) {
                for (const auto child : children) self(self, child);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f, 4.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0f, 2.0f});
        for (const auto& [id, entity] : roots) draw(draw, entity);
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
}

void EditorShell::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    const auto open = ImGui::Begin(viewport_title.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    m_viewport_hovered = false;
    m_layout.viewport_min = m_layout.viewport_max = {0, 0};
    auto request = PixelSize{};
    // An appearing window (first frame, or a tab just shown) has no final size yet: skip it rather
    // than allocate a target for a size that is about to change.
    if (open && m_world && !ImGui::IsWindowAppearing()) {
        const auto available = ImGui::GetContentRegionAvail();
        const auto scale = ImGui::GetIO().DisplayFramebufferScale.x;
        request = viewport_pixels(available.x, available.y, scale);
        if (!request.empty()) {
            // Drawn at exactly one texel per framebuffer pixel, so the image is never stretched.
            ImGui::Image(m_viewport_texture, {float(request.width) / scale, float(request.height) / scale});
            m_viewport_hovered = ImGui::IsItemHovered();
            m_layout.viewport_min = ImGui::GetItemRectMin();
            m_layout.viewport_max = ImGui::GetItemRectMax();
            // Navigation hint: a quiet pill in the corner, brighter while flying.
            const auto flying = m_router.navigating();
            const auto hint = flying
                ? std::string(icon::arrows_move) + "  WASD move  \xC2\xB7  Q/E down/up  \xC2\xB7  Shift faster  \xC2\xB7  Esc stop"
                : std::string(icon::mouse_right) + "  Hold to fly     " + icon::mouse_scroll + "  Scroll to dolly";
            auto* draw = ImGui::GetWindowDrawList();
            const auto size = ImGui::CalcTextSize(hint.c_str());
            const auto corner = ImVec2{m_layout.viewport_min.x + 12.0f, m_layout.viewport_max.y - size.y - 22.0f};
            draw->AddRectFilled(corner, {corner.x + size.x + 20.0f, corner.y + size.y + 10.0f},
                                theme::color::rgb(0x0B0C0E, 190), 8.0f);
            draw->AddText({corner.x + 10.0f, corner.y + 5.0f}, flying ? theme::color::text : theme::color::muted, hint.c_str());
        }
    } else if (open) {
        ImGui::SetCursorPos({16.0f, 14.0f});
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextUnformatted(m_world ? "" : "No scene is open. Details are in Diagnostics.");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    if (request.empty() && !m_viewport_request.empty() && m_router.navigating())
        if (const auto release = m_router.cancel()) m_capture_request = release; // the viewport disappeared
    m_viewport_request = request;
}

void EditorShell::draw_inspector() {
    if (ImGui::Begin(inspector_title.c_str())) {
        theme::caption(m_fonts, "EDITOR CAMERA");
        if (theme::begin_properties("camera")) {
            theme::property("Position");
            ImGui::AlignTextToFramePadding();
            const auto& p = m_camera.position;
            theme::mono_text(m_fonts, format("%.2f %.2f %.2f", p.x, p.y, p.z).c_str());
            theme::property("Speed");
            ImGui::InputFloat("##speed", &m_camera.speed, 0.0f, 0.0f, "%.2f m/s");
            m_layout.camera_speed_min = ImGui::GetItemRectMin();
            m_layout.camera_speed_max = ImGui::GetItemRectMax();
            if (!std::isfinite(m_camera.speed)) m_camera.speed = 3.0f;
            m_camera.speed = std::clamp(m_camera.speed, 0.1f, 100.0f);
            theme::property("Field of view");
            auto fov = m_camera.camera.vertical_fov * 180.0f / math::PI;
            if (ImGui::InputFloat("##fov", &fov, 0.0f, 0.0f, "%.0f\xC2\xB0") && std::isfinite(fov))
                m_camera.camera.vertical_fov = std::clamp(fov, 20.0f, 120.0f) * math::PI / 180.0f;
            theme::end_properties();
        }
        ImGui::Dummy({0.0f, 10.0f});
        theme::caption(m_fonts, "SELECTION");
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextWrapped("Nothing selected.");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::TextWrapped("Entity properties will appear here.");
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void EditorShell::draw_assets() {
    if (ImGui::Begin(assets_title.c_str()) && m_assets) {
        const auto records = m_assets->records();
        theme::caption(m_fonts, "PROJECT", std::to_string(records.size()).c_str());
        constexpr auto flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("assets", 3, flags)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.2f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            for (const auto& record : records) {
                const auto info = m_assets->info(record.id);
                const auto state = info ? info->state : AssetState::unloaded;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                icon_text(record.kind == AssetKind::mesh ? icon::cube : icon::circle_half, theme::color::muted);
                ImGui::TextUnformatted(record.path.filename().string().c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", record.path.generic_string().c_str(), id_text(record.id.high, record.id.low).c_str());
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
                ImGui::TextUnformatted(record.kind == AssetKind::mesh ? "Mesh" : "Material");
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                static constexpr const char* names[] = {"unloaded", "loading", "ready", "failed"};
                const auto tone = state == AssetState::ready ? theme::color::success
                    : state == AssetState::failed ? theme::color::danger
                    : state == AssetState::loading ? theme::color::warning : theme::color::faint;
                theme::dot(tone, 3.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
                ImGui::TextUnformatted(names[static_cast<int>(state)]);
                ImGui::PopStyleColor();
                if (info && info->diagnostic && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info->diagnostic.message.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorShell::draw_diagnostics() {
    if (ImGui::Begin(diagnostics_title.c_str())) {
        const auto& io = ImGui::GetIO();
        m_stats_age += io.DeltaTime;
        if (m_stats_age >= 0.25f) {
            m_shown_stats = m_device.stats();
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

void EditorShell::render_viewport() {
    m_ui.set_texture(m_viewport_texture, {});
    m_viewport_error = false;
    if (!m_world || m_viewport_request.empty()) return;
    const auto allocations = m_viewport.allocations();
    if (auto error = m_viewport.resize(m_viewport_request.width, m_viewport_request.height)) {
        m_viewport_error = true;
        m_log.add(DiagnosticSource::viewport, "Viewport target: " + error.message, m_frame);
        return;
    }
    if (m_viewport.allocations() != allocations)
        m_log.add(DiagnosticSource::viewport, "Viewport target reallocated for a new panel size", m_frame);
    const auto view = make_render_view(m_camera.camera, m_camera.pose(), m_viewport.width(), m_viewport.height());
    if (!view) {
        m_viewport_error = true;
        m_log.add(DiagnosticSource::viewport, "The editor camera has no valid view", m_frame);
        return;
    }
    const auto snapshot = extract_render_snapshot(*m_world, *m_assets);
    m_extraction = snapshot.stats;
    m_frame_problems = snapshot.diagnostics;
    if (auto error = m_renderer.render(snapshot, *view, m_viewport)) {
        m_viewport_error = true;
        m_log.add(DiagnosticSource::renderer, error.message, m_frame);
    }
    m_ui.set_texture(m_viewport_texture, m_viewport.color());
}

RhiDiagnostic EditorShell::render(TextureHandle destination) {
    for (auto& error : m_device.take_gpu_errors()) {
        std::cerr << "[Editor] GPU: " << error.message << '\n';
        m_log.add(DiagnosticSource::gpu, std::move(error.message), m_frame);
    }
    if (!m_frame_ready) return {};
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_context);
    render_viewport();
    const auto* data = ImGui::GetDrawData();
    auto result = data ? m_ui.render(*data, destination, window_background) : RhiDiagnostic{};
    if (result) m_log.add(DiagnosticSource::ui, result.message, m_frame);
    ImGui::SetCurrentContext(previous ? previous : m_context);
    return result;
}

} // namespace maya::editor
