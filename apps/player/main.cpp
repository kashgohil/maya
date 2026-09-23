#include "basic_scene.hpp"
#include "maya/platform/desktop_application.hpp"

int main(int argc, char** argv) {
    // Initial native project selection belongs to the executable, never the runtime.
    // Serialized scene/project selection will replace this in the player milestone.
    return maya::run_desktop(argc, argv, maya::samples::create_basic_scene(),
        {.title = "Maya Player | Basic Scene", .capture_cursor = true});
}
