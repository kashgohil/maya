#include "editor_shell.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace maya::editor {
namespace {
constexpr std::array<double, 4> window_background{0.06, 0.06, 0.07, 1.0};
constexpr float base_font_size = 13.0f; // points

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
                         PlatformServices services)
    : m_device(device), m_services(std::move(services)), m_renderer(device, std::move(renderer_shader)),
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
    ImGui::StyleColorsDark();
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
    auto config = ImFontConfig{};
    config.SizePixels = std::round(base_font_size * scale); // rasterize at framebuffer resolution
    io.Fonts->AddFontDefault(&config);
    io.FontGlobalScale = 1.0f / scale; // and draw at the same size in points
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
    if (routed.navigation_started) ImGui::SetWindowFocus("Viewport");
    const auto dockspace = ImGui::GetID("EditorDockSpace");
    if (!m_layout_built) build_dock_layout(dockspace);
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
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
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Diagnostics", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
    m_layout_built = true;
}

void EditorShell::draw_hierarchy() {
    const auto open = ImGui::Begin("Hierarchy");
    if (open && !m_world) ImGui::TextDisabled("No scene is open.");
    if (open && m_world) {
        auto& world = *m_world;
        auto roots = std::vector<std::pair<EntityId, EntityHandle>>{};
        world.for_each_entity([&](EntityHandle entity) {
            if (!world.parent(entity)) roots.emplace_back(*world.persistent_id(entity), entity);
        });
        std::ranges::sort(roots, {}, &std::pair<EntityId, EntityHandle>::first);
        const auto draw = [&](const auto& self, EntityHandle entity) -> void {
            const auto id = *world.persistent_id(entity);
            auto label = id_text(id.high, id.low);
            world.with<NameComponent>(entity, [&](const NameComponent& name) { label = name.value; });
            const auto children = world.children(entity);
            auto flags = ImGuiTreeNodeFlags{ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen};
            if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            ImGui::PushID(static_cast<int>(id.low ^ (id.high << 7)));
            if (ImGui::TreeNodeEx("entity", flags, "%s", label.c_str())) {
                for (const auto child : children) self(self, child);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        for (const auto& [id, entity] : roots) draw(draw, entity);
    }
    ImGui::End();
}

void EditorShell::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    const auto open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
            const auto hint = m_router.navigating() ? "Flying: WASD move, Q/E down/up, Shift faster, Esc or release to stop"
                                                    : "Hold the right mouse button to fly; scroll to dolly";
            ImGui::GetWindowDrawList()->AddText({m_layout.viewport_min.x + 8.0f, m_layout.viewport_min.y + 6.0f},
                                                IM_COL32(230, 230, 230, 200), hint);
        }
    } else if (open) {
        ImGui::TextDisabled("No scene is open. See Diagnostics.");
    }
    ImGui::End();
    if (request.empty() && !m_viewport_request.empty() && m_router.navigating())
        if (const auto release = m_router.cancel()) m_capture_request = release; // the viewport disappeared
    m_viewport_request = request;
}

void EditorShell::draw_inspector() {
    if (ImGui::Begin("Inspector")) {
        ImGui::SeparatorText("Editor camera");
        ImGui::Text("Position %.2f, %.2f, %.2f", m_camera.position.x, m_camera.position.y, m_camera.position.z);
        ImGui::InputFloat("Speed (m/s)", &m_camera.speed, 0.0f, 0.0f, "%.2f");
        m_layout.camera_speed_min = ImGui::GetItemRectMin();
        m_layout.camera_speed_max = ImGui::GetItemRectMax();
        if (!std::isfinite(m_camera.speed)) m_camera.speed = 3.0f;
        m_camera.speed = std::clamp(m_camera.speed, 0.1f, 100.0f);
        auto fov = m_camera.camera.vertical_fov * 180.0f / math::PI;
        if (ImGui::SliderFloat("Vertical FOV", &fov, 20.0f, 120.0f, "%.0f deg"))
            m_camera.camera.vertical_fov = fov * math::PI / 180.0f;
        ImGui::SeparatorText("Selection");
        ImGui::TextDisabled("Selection and property editing arrive with #1000 and #1001.");
    }
    ImGui::End();
}

void EditorShell::draw_assets() {
    if (ImGui::Begin("Assets") && m_assets) {
        if (ImGui::BeginTable("assets", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Path");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();
            for (const auto& record : m_assets->records()) {
                const auto info = m_assets->info(record.id);
                static constexpr const char* states[] = {"unloaded", "loading", "ready", "failed"};
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(record.kind == AssetKind::mesh ? "mesh" : "material");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(id_text(record.id.high, record.id.low).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(record.path.generic_string().c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(info ? states[static_cast<int>(info->state)] : "?");
                if (info && info->diagnostic && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info->diagnostic.message.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorShell::draw_diagnostics() {
    if (ImGui::Begin("Diagnostics")) {
        const auto& io = ImGui::GetIO();
        const auto stats = m_device.stats();
        const auto& target = m_viewport;
        ImGui::Text("Viewport: %ux%u px (%.0fx%.0f pt at %.2gx), %llu allocations%s", m_viewport_request.width,
                    m_viewport_request.height, float(m_viewport_request.width) / io.DisplayFramebufferScale.x,
                    float(m_viewport_request.height) / io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.x,
                    static_cast<unsigned long long>(target.allocations()),
                    m_viewport_request.empty() ? " (hidden)" : m_viewport_error ? " (error)" : "");
        ImGui::Text("Scene: %zu drawn, %zu hidden, %zu skipped of %zu mesh renderers", m_extraction.mesh_renderers -
                    m_extraction.hidden - m_extraction.skipped, m_extraction.hidden, m_extraction.skipped,
                    m_extraction.mesh_renderers);
        ImGui::Text("Frames: %llu submitted, %llu completed, %llu waits; upload high water %zu KiB of %zu KiB, %llu failures",
                    static_cast<unsigned long long>(stats.submitted_frames), static_cast<unsigned long long>(stats.completed_frames),
                    static_cast<unsigned long long>(stats.frame_waits), stats.transient_high_water / 1024,
                    m_device.options().transient_bytes_per_frame / 1024,
                    static_cast<unsigned long long>(stats.transient_failures));
        ImGui::Text("Resources: %zu buffers, %zu textures, %zu pipelines, %zu awaiting GPU completion", stats.buffers,
                    stats.textures, stats.pipelines, stats.pending_retirements);
        if (!m_frame_problems.empty()) {
            ImGui::SeparatorText("Scene problems this frame");
            for (const auto& problem : m_frame_problems) ImGui::BulletText("%s", problem.message.c_str());
        }
        ImGui::SeparatorText("Log");
        if (ImGui::BeginTable("log", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Message");
            ImGui::TableHeadersRow();
            for (auto it = m_log.entries().rbegin(); it != m_log.entries().rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(source_name(it->source));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(it->count));
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", it->message.c_str());
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
