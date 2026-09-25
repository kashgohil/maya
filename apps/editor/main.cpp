#include "editor_application.hpp"
#include "maya/platform/desktop_application.hpp"
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    // The editor's own argument, a project file or folder, is taken out; the host parses the rest.
    auto project = std::optional<std::filesystem::path>{};
    auto host_arguments = std::vector<char*>{argv, argv + (argc > 0 ? 1 : 0)};
    for (int i = 1; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument.starts_with("-")) {
            host_arguments.push_back(argv[i]);
            // --smoke takes an optional frame count.
            const auto count = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view{};
            if (argument == "--smoke" && !count.empty() && count.find_first_not_of("0123456789-+") == std::string_view::npos)
                host_arguments.push_back(argv[++i]);
            continue;
        }
        if (project) {
            std::cerr << "[Editor] only one project can be opened: " << argument << '\n';
            return 2;
        }
        project = std::filesystem::absolute(argv[i]).lexically_normal(); // relative to where the editor was started
    }
    host_arguments.push_back(nullptr);
    // Escape belongs to the editor (it ends camera navigation and text entry), so it does not quit.
    // UI geometry shares frame upload memory with the scene, so the editor reserves more of it.
    return maya::run_desktop(int(host_arguments.size() - 1), host_arguments.data(),
        maya::editor::create_editor_application(std::move(project)),
        {.title = "Maya Editor", .escape_closes = false, .usage = "[project]", .device = {3, size_t{16} << 20}});
}
