#pragma once

#include "editor_camera.hpp"
#include "editor_theme.hpp"
#include "input_router.hpp"
#include "ui_renderer.hpp"
#include "maya/platform/input.hpp"
#include "maya/renderer/renderer.hpp"
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;

namespace maya::editor {

/// TrueType data for the editor's typefaces. Any empty or unreadable entry uses ImGui's built-in font.
struct EditorFonts {
    std::string regular; // resources/fonts/Inter-Regular.ttf
    std::string semibold; // resources/fonts/Inter-SemiBold.ttf
    std::string mono; // resources/fonts/GeistMono-Regular.ttf
    std::string icons; // resources/fonts/Phosphor-Light.ttf, merged into the regular and semibold fonts
};

/// Framebuffer pixel size of a view.
struct PixelSize {
    uint32_t width = 0;
    uint32_t height = 0;
    bool empty() const noexcept { return width == 0 || height == 0; }
    auto operator<=>(const PixelSize&) const = default;
};
/// Pixel size for a panel area in points at a display scale: whole pixels, never stretched.
PixelSize viewport_pixels(float width_points, float height_points, float scale) noexcept;

/// Where the last frame placed interactive elements, in window points. Empty when not shown.
struct EditorLayout {
    ImVec2 viewport_min{0, 0}, viewport_max{0, 0};
    ImVec2 camera_speed_min{0, 0}, camera_speed_max{0, 0};
};

enum class DiagnosticSource { scene, viewport, renderer, gpu, ui };
struct DiagnosticEntry {
    DiagnosticSource source;
    std::string message;
    uint64_t count = 1; // repeated messages are merged
    uint64_t last_frame = 0;
};
/// Bounded log of editor problems and state changes, newest last.
class DiagnosticLog {
public:
    static constexpr size_t capacity = 200;
    void add(DiagnosticSource source, std::string message, uint64_t frame);
    const std::deque<DiagnosticEntry>& entries() const noexcept { return m_entries; }
    size_t count(DiagnosticSource source) const;

private:
    std::deque<DiagnosticEntry> m_entries;
};

/// The editor window's contents: a dockable layout with hierarchy, viewport, inspector, assets, and
/// diagnostics panels; input routing between the UI and the editor camera; and rendering of the
/// viewport and UI through the shared renderer. Owns its ImGui context. Main thread only.
class EditorShell {
public:
    EditorShell(GraphicsDevice& device, std::string renderer_shader, std::string ui_shader,
                PlatformServices services = {}, EditorFonts fonts = {});
    ~EditorShell();
    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    /// Opens a project catalog and scene for viewing. Problems go to the diagnostics panel; the
    /// previous scene stays open on failure. Returns whether the scene opened.
    bool open_scene(const std::filesystem::path& catalog, const std::filesystem::path& scene);

    /// Routes this frame's input and builds the UI. A zero-sized (minimized) window skips the frame.
    void update(float delta_time, const std::vector<InputEvent>& events, const WindowMetrics& metrics);
    /// Inside a device frame with no pass open: renders the viewport, then the UI into `destination`
    /// (normally the window surface). Viewport problems are reported in diagnostics, not returned;
    /// the returned error means the UI pass itself failed.
    RhiDiagnostic render(TextureHandle destination);
    /// The cursor capture state to request from the host, when it changed this frame.
    std::optional<bool> take_capture_request() noexcept { return std::exchange(m_capture_request, std::nullopt); }

    bool navigating() const noexcept { return m_router.navigating(); }
    bool ui_wants_text() const noexcept { return m_ui_wants_text; }
    bool viewport_hovered() const noexcept { return m_viewport_hovered; }
    const EditorCamera& camera() const noexcept { return m_camera; }
    EditorCamera& camera() noexcept { return m_camera; }
    PixelSize viewport_request() const noexcept { return m_viewport_request; }
    const RenderTarget& viewport() const noexcept { return m_viewport; }
    const EditorLayout& layout() const noexcept { return m_layout; }
    const DiagnosticLog& diagnostics() const noexcept { return m_log; }
    const UiRenderer& ui_renderer() const noexcept { return m_ui; }
    const RenderSnapshotStats& extraction() const noexcept { return m_extraction; }
    uint64_t frames() const noexcept { return m_frame; }
    /// For inspection in tests; make it current only between frames.
    ImGuiContext* context() const noexcept { return m_context; }

private:
    void apply_input(const RoutedInput& routed);
    void rebuild_fonts(float scale);
    void build_dock_layout(unsigned int dockspace);
    void draw_hierarchy();
    void draw_viewport();
    void draw_inspector();
    void draw_assets();
    void draw_diagnostics();
    void draw_top_bar();
    void draw_status_bar();
    void render_viewport();

    GraphicsDevice& m_device;
    PlatformServices m_services;
    std::string m_clipboard; // storage for ImGui's clipboard reads
    EditorFonts m_font_data; // the atlas borrows this data
    theme::Fonts m_fonts;
    ImGuiContext* m_context = nullptr;
    Renderer m_renderer;
    UiRenderer m_ui;
    RenderTarget m_viewport;
    ImTextureID m_viewport_texture = 0;
    std::unique_ptr<AssetRegistry> m_assets;
    std::unique_ptr<World> m_world;
    std::filesystem::path m_scene_path;
    EditorCamera m_camera;
    InputRouter m_router;
    std::optional<bool> m_capture_request;
    EditorLayout m_layout{};
    DiagnosticLog m_log;
    std::vector<RenderDiagnostic> m_frame_problems;
    RenderSnapshotStats m_extraction{};
    PixelSize m_viewport_request{};
    float m_font_scale = 0.0f;
    RhiStats m_shown_stats{}; // refreshed a few times a second so the numbers stay readable
    float m_stats_age = 1.0f;
    int m_cursor = -1; // last ImGuiMouseCursor sent to the host
    bool m_layout_built = false;
    bool m_frame_ready = false;
    bool m_minimized = false;
    bool m_viewport_hovered = false;
    bool m_ui_wants_text = false;
    bool m_viewport_error = false;
    uint64_t m_frame = 0;
};

} // namespace maya::editor
