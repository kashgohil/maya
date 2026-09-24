#include "editor_application.hpp"
#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/input.hpp"
#include <stdexcept>

namespace maya::editor {
namespace {

// Hosts the editor shell. Until project open/save lands (#1002), it opens the sample project's scene.
class EditorApplication final : public Application {
public:
    bool on_start(GraphicsDevice& device) override {
        auto renderer_shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        auto ui_shader = FileSystem::read_text("resources/shaders/metal/editor_ui.metal");
        if (renderer_shader.empty() || ui_shader.empty()) return false;
        m_shell = std::make_unique<EditorShell>(device, std::move(renderer_shader), std::move(ui_shader),
                                                Input::instance().services());
        // An editor without content still starts; the diagnostics panel explains what is missing.
        if (const auto catalog = FileSystem::resolve("samples/basic_scene/assets/catalog.maya"))
            m_shell->open_scene(*catalog, catalog->parent_path() / "basic.scene");
        return true;
    }

    void on_update(float delta_time, bool input_enabled) override {
        auto& input = Input::instance();
        static const auto no_events = std::vector<InputEvent>{};
        m_shell->update(delta_time, input_enabled ? input.events() : no_events, input.window_metrics());
        if (const auto capture = m_shell->take_capture_request()) input.request_cursor_capture(*capture);
    }

    void on_render(GraphicsDevice& device) override {
        const auto surface = device.acquire_surface();
        if (!surface) return; // hidden, zero-sized, or no drawable this frame
        if (auto error = m_shell->render(surface.target.texture)) throw std::runtime_error(error.message);
    }

    void on_stop() noexcept override {
        if (m_shell && m_shell->navigating()) Input::instance().request_cursor_capture(false);
        m_shell.reset();
    }

private:
    std::unique_ptr<EditorShell> m_shell;
};

} // namespace

std::unique_ptr<Application> create_editor_application() {
    return std::make_unique<EditorApplication>();
}

} // namespace maya::editor
