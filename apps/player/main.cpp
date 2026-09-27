#include "player_application.hpp"
#include "maya/platform/desktop_application.hpp"
#include <iostream>

int main(int argc, char** argv) {
    // The player's own arguments: a project file or folder, then a scene inside its content folder.
    auto arguments = maya::split_arguments(argc, argv);
    if (arguments.positional.size() > 2) {
        std::cerr << "[Player] unexpected argument: " << arguments.positional[2] << '\n';
        return 2;
    }
    auto options = maya::player::PlayerOptions{};
    if (arguments.positional.size() > 0) // relative to where the player was started
        options.project = std::filesystem::absolute(arguments.positional[0]).lexically_normal();
    if (arguments.positional.size() > 1) options.scene = arguments.positional[1];
    return maya::run_desktop(arguments.host_count(), arguments.host.data(),
        maya::player::create_player_application(std::move(options)),
        {.title = "Maya Player", .capture_cursor = true, .usage = "[project [scene]]"});
}
