#include "editor_application.hpp"
#include "maya/platform/desktop_application.hpp"
#include <filesystem>
#include <iostream>
#include <optional>

int main(int argc, char** argv) {
    // The editor's own argument, a project file or folder, is taken out; the host parses the rest.
    auto arguments = maya::split_arguments(argc, argv);
    if (arguments.positional.size() > 1) {
        std::cerr << "[Editor] only one project can be opened: " << arguments.positional[1] << '\n';
        return 2;
    }
    auto project = std::optional<std::filesystem::path>{};
    if (!arguments.positional.empty()) // relative to where the editor was started
        project = std::filesystem::absolute(arguments.positional.front()).lexically_normal();
    // Escape belongs to the editor (it ends camera navigation and text entry), so it does not quit.
    // UI geometry shares frame upload memory with the scene, so the editor reserves more of it.
    return maya::run_desktop(arguments.host_count(), arguments.host.data(),
        maya::editor::create_editor_application(std::move(project)),
        {.title = "Maya Editor", .escape_closes = false, .usage = "[project]", .device = {3, size_t{16} << 20}});
}
