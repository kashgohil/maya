#pragma once
// Drives an EditorShell on the null device with synthetic input, as a person would.

#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/null_device.hpp"
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <unistd.h>

namespace maya::editor::testing {
namespace fs = std::filesystem;
/// Null backend that records UI encoding and can fail chosen resources.
class EditorDevice final : public NullGraphicsDevice {
public:
    explicit EditorDevice(DeviceOptions options = {3, size_t{16} << 20}) { REQUIRE(initialize(nullptr, options)); }
    ~EditorDevice() override { shutdown(); }

    std::function<bool(const TextureDesc&)> fail_texture;
    std::function<bool(const PipelineDesc&)> fail_pipeline;
    std::vector<ScissorRect> scissors;
    std::vector<uint32_t> sampled; // texture slots bound for sampling
    std::vector<PipelineDesc> pipelines;
    size_t indexed_draws = 0;

protected:
    RhiDiagnostic backend_create_texture(uint32_t slot, const TextureDesc& desc, const void* data) override {
        if (fail_texture && fail_texture(desc)) return {RhiError::out_of_memory, "injected texture failure: " + desc.label};
        return NullGraphicsDevice::backend_create_texture(slot, desc, data);
    }
    RhiDiagnostic backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) override {
        if (fail_pipeline && fail_pipeline(desc)) return {RhiError::shader_compilation, "injected pipeline failure: " + desc.label};
        pipelines.push_back(desc);
        return NullGraphicsDevice::backend_create_pipeline(slot, desc);
    }
    void backend_set_scissor(const ScissorRect& rect) override { scissors.push_back(rect); }
    void backend_set_texture(uint32_t, uint32_t slot) override { sampled.push_back(slot); }
    void backend_draw_indexed(uint32_t slot, IndexType type, uint32_t count, size_t offset, uint32_t instances) override {
        ++indexed_draws;
        NullGraphicsDevice::backend_draw_indexed(slot, type, count, offset, instances);
    }
};

inline std::filesystem::path sample_project() {
    const auto project = FileSystem::resolve("samples/basic_scene/project.maya");
    REQUIRE(project);
    return *project;
}

/// A shell on the null device with a 1280x720-point window at 2x (Retina) scale. It opens the sample
/// project unless told not to, and counts the close requests it makes to the host.
struct Harness {
    explicit Harness(bool open_sample = true, DeviceOptions options = {3, size_t{16} << 20})
        : device(options), shell(device, "renderer source", "ui source", {{}, {}, {}, [this] { ++close_requests; }}) {
        if (open_sample) REQUIRE(shell.open_project(sample_project()));
        resize_window(metrics);
    }
    void resize_window(const WindowMetrics& value) {
        metrics = value;
        if (window.valid()) device.destroy(window);
        if (value.framebuffer_width == 0 || value.framebuffer_height == 0) return;
        const auto created = device.create_texture({value.framebuffer_width, value.framebuffer_height,
            Format::bgra8_unorm, TextureUsage::render_target, "window"});
        REQUIRE(created);
        window = created.handle;
    }
    /// One host frame: route and build the UI, then render it into the window, as the editor does.
    void frame(std::vector<InputEvent> events = {}, float delta_time = 1.0f / 60.0f) {
        shell.update(delta_time, events, metrics);
        if (const auto capture = shell.take_capture_request()) captured = *capture;
        REQUIRE_FALSE(device.begin_frame());
        if (window.valid()) {
            const auto error = shell.render(window);
            INFO(error.message);
            REQUIRE_FALSE(error);
        }
        REQUIRE_FALSE(device.end_frame());
    }
    void frames(int count) { for (int i = 0; i < count; ++i) frame(); }
    ImVec2 viewport_center() const {
        const auto& layout = shell.layout();
        return {(layout.viewport_min.x + layout.viewport_max.x) / 2, (layout.viewport_min.y + layout.viewport_max.y) / 2};
    }
    template<class F> auto with_context(F&& inspect) {
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(shell.context());
        auto result = inspect();
        ImGui::SetCurrentContext(previous);
        return result;
    }

    WindowMetrics metrics{1280, 720, 2560, 1440};
    EditorDevice device;
    EditorShell shell;
    TextureHandle window;
    bool captured = false;
    int close_requests = 0;
};

/// Moves the pointer in one frame and presses a button in the next, as a person would. Hover state
/// is decided by the frame the pointer moved in.
inline void click(Harness& harness, ImVec2 at, MouseButton button = MouseButton::left) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    harness.frame({MouseButtonEvent{button, true, KeyModifiers::none}});
}

inline std::string active_text(Harness& harness) {
    return harness.with_context([] {
        const auto& g = *ImGui::GetCurrentContext();
        return g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId ? std::string(g.InputTextState.TextA.Data) : std::string{};
    });
}

inline std::vector<InputEvent> key(KeyCode code, bool down, uint32_t text = 0) {
    auto events = std::vector<InputEvent>{KeyEvent{code, down, KeyModifiers::none}};
    if (down && text) events.push_back(TextEvent{text});
    return events;
}

inline bool same(const math::Vec3& a, const math::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline bool logged(const DiagnosticLog& log, DiagnosticSource source, std::string_view text) {
    return std::ranges::any_of(log.entries(), [&](const DiagnosticEntry& entry) {
        return entry.source == source && entry.message.find(text) != std::string::npos;
    });
}

inline void chord(Harness& harness, std::initializer_list<KeyCode> modifiers, KeyCode key) {
    auto mods = KeyModifiers::none;
    for (const auto modifier : modifiers) {
        const auto flag = modifier == KeyCode::LeftSuper ? KeyModifiers::super : KeyModifiers::shift;
        mods = static_cast<KeyModifiers>(static_cast<uint8_t>(mods) | static_cast<uint8_t>(flag));
        harness.frame({KeyEvent{modifier, true, mods}});
    }
    harness.frame({KeyEvent{key, true, mods}});
    harness.frame({KeyEvent{key, false, mods}});
    for (const auto modifier : modifiers) harness.frame({KeyEvent{modifier, false, KeyModifiers::none}});
}

/// A full click: move there in one frame, press in the next, release in the one after.
inline void press(Harness& harness, ImVec2 at, MouseButton button = MouseButton::left) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    harness.frame({MouseButtonEvent{button, true, KeyModifiers::none}});
    harness.frame({MouseButtonEvent{button, false, KeyModifiers::none}});
}

inline ImVec2 row_center(Harness& harness, EntityId id) {
    const auto* row = harness.shell.layout().row(id);
    REQUIRE(row);
    return {row->min.x + 60.0f, (row->min.y + row->max.y) / 2.0f};
}

inline EntityId find_named(SceneEditor& scene, const std::string& name) {
    for (const auto& [id, record] : capture_state(scene.world()).entities)
        if (scene.display_name(id) == name) return id;
    FAIL("no entity named " << name);
    return {};
}

/// Screen position (points) of a world point in the editor viewport, from the shell's own camera.
inline ImVec2 on_screen(Harness& harness, const math::Vec3& point) {
    const auto& layout = harness.shell.layout();
    const auto request = harness.shell.viewport_request();
    const auto view = make_render_view(harness.shell.camera().camera, harness.shell.camera().pose(), request.width, request.height);
    REQUIRE(view);
    const auto clip = view->matrices.view_projection * math::Vec4(point, 1.0f);
    return {layout.viewport_min.x + (clip.x / clip.w * 0.5f + 0.5f) * (layout.viewport_max.x - layout.viewport_min.x),
            layout.viewport_min.y + (0.5f - clip.y / clip.w * 0.5f) * (layout.viewport_max.y - layout.viewport_min.y)};
}

/// Presses at `from`, moves to `to` over several frames, and releases.
inline void drag(Harness& harness, ImVec2 from, ImVec2 to, int steps = 10) {
    harness.frame({MouseMoveEvent{from.x, from.y}});
    harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
    for (int step = 1; step <= steps; ++step) {
        const auto t = float(step) / float(steps);
        harness.frame({MouseMoveEvent{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}});
    }
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
}

/// A copy of the sample project in a scratch folder, named "Sample Game", with its content folder
/// under the given name. Removed afterwards.
struct ProjectCopy {
    fs::path root;
    fs::path folder;
    fs::path content;
    explicit ProjectCopy(const std::string& content_name = "assets", const std::string& startup = "basic.scene") {
        const auto scratch = fs::temp_directory_path() /
            ("maya-editor-project-" + std::to_string(::getpid()) + "-" + std::to_string(detail::next_lifetime_token()));
        fs::create_directories(scratch / "Sample Game");
        root = fs::canonical(scratch);
        folder = root / "Sample Game";
        content = folder / content_name;
        fs::copy(sample_project().parent_path() / "assets", content, fs::copy_options::recursive);
        auto project = std::ofstream(folder / "project.maya");
        project << "maya-project 1\ncontent \"" << content_name << "\"\ncatalog \"catalog.maya\"\n";
        if (!startup.empty()) project << "startup \"" << startup << "\"\n";
    }
    ~ProjectCopy() {
        std::error_code error;
        writable(content);
        fs::remove_all(root, error);
    }
    std::string read(const fs::path& relative) const {
        auto file = std::ifstream(content / relative, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }
    void write(const fs::path& relative, const std::string& text) const { std::ofstream(content / relative) << text; }
    static void read_only(const fs::path& path) {
        fs::permissions(path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
    }
    static void writable(const fs::path& path) {
        std::error_code error;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, error);
    }
};

/// Restores the working directory when it goes out of scope.
struct WorkingDirectory {
    fs::path previous = fs::current_path();
    explicit WorkingDirectory(const fs::path& path) { fs::current_path(path); }
    ~WorkingDirectory() { fs::current_path(previous); }
};

} // namespace maya::editor::testing
