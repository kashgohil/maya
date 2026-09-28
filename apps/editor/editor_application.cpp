#include "editor_application.hpp"
#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/input.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace maya::editor {
namespace {

// Hosts the editor shell and opens its project.
class EditorApplication final : public Application {
public:
    explicit EditorApplication(std::optional<std::filesystem::path> project) : m_project(std::move(project)) {}

    bool on_start(GraphicsDevice& device) override {
        auto renderer_shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        auto ui_shader = FileSystem::read_text("resources/shaders/metal/editor_ui.metal");
        if (renderer_shader.empty() || ui_shader.empty()) return false;
        m_shell = std::make_unique<EditorShell>(device, std::move(renderer_shader), std::move(ui_shader),
                                                Input::instance().services(),
                                                EditorFonts{read_file("resources/fonts/Inter-Regular.ttf"),
                                                            read_file("resources/fonts/Inter-SemiBold.ttf"),
                                                            read_file("resources/fonts/GeistMono-Regular.ttf"),
                                                            read_file("resources/fonts/Phosphor-Light.ttf")});
        // An editor without a project still starts; the Assets panel says how to open one, and a project
        // that fails to open explains why.
        if (!m_project) m_project = FileSystem::resolve("samples/basic_scene/project.maya");
        if (!m_project) std::cerr << "[Editor] no project given, and the sample project was not found\n";
        else if (!m_shell->open_project(*m_project)) std::cerr << "[Editor] " << m_shell->prompt_message() << '\n';
        else std::cerr << "[Editor] project " << m_shell->project()->file.string() << ", scene "
                       << (m_shell->scene_path().empty() ? std::string("(new)") : m_shell->scene_path().filename().string()) << '\n';
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

    void on_frame_timing(const FrameTiming& timing) override {
        if (m_shell) m_shell->record_frame(timing);
    }

    bool on_close_requested() override { return !m_shell || m_shell->request_close(); }

    void on_stop() noexcept override {
        if (m_shell && m_shell->navigating()) Input::instance().request_cursor_capture(false);
        m_shell.reset();
    }

private:
    static std::string read_file(const std::string& relative) {
        const auto path = FileSystem::resolve(relative);
        auto file = path ? std::ifstream(*path, std::ios::binary) : std::ifstream{};
        return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string{};
    }

    std::optional<std::filesystem::path> m_project;
    std::unique_ptr<EditorShell> m_shell;
};

} // namespace

std::unique_ptr<Application> create_editor_application(std::optional<std::filesystem::path> project) {
    return std::make_unique<EditorApplication>(std::move(project));
}

} // namespace maya::editor
