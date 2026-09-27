#include "player_application.hpp"
#include "maya/platform/desktop_application.hpp"

int main(int argc, char** argv) {
    // The sample project in the player: its spinning pyramid and flying camera are scene data
    // (maya.spin, maya.fly_control), not sample code.
    return maya::run_desktop(argc, argv, maya::player::create_player_application(),
        {.title = "Maya Sample | Basic Scene", .capture_cursor = true});
}
