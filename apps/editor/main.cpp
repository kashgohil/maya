#include "editor_application.hpp"
#include "maya/platform/desktop_application.hpp"

int main(int argc, char** argv) {
    return maya::run_desktop(argc, argv, maya::editor::create_editor_application(),
        {.title = "Maya Editor"});
}
