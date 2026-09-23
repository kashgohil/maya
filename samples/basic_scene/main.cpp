#include "basic_scene.hpp"
#include "maya/platform/desktop_application.hpp"

int main(int argc, char** argv) {
    return maya::run_desktop(argc, argv, maya::samples::create_basic_scene(),
        {.title = "Maya Sample | Basic Scene", .capture_cursor = true});
}
