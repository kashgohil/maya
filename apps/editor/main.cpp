#include "editor_application.hpp"
#include "maya/platform/desktop_application.hpp"

int main(int argc, char** argv) {
    // Escape belongs to the editor (it ends camera navigation and text entry), so it does not quit.
    // UI geometry shares frame upload memory with the scene, so the editor reserves more of it.
    return maya::run_desktop(argc, argv, maya::editor::create_editor_application(),
        {.title = "Maya Editor", .escape_closes = false, .device = {3, size_t{16} << 20}});
}
