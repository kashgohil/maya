#include "player_application.hpp"
#include "maya/platform/desktop_application.hpp"
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    // The player's own options, --record <file> and --replay <file>, are taken out first; the rest are a
    // project file or folder, then a scene inside its content folder, and the host's options.
    auto options = maya::player::PlayerOptions{};
    auto kept = std::vector<char*>{};
    for (int i = 0; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--record" || argument == "--replay") {
            if (i + 1 >= argc) {
                std::cerr << "[Player] " << argument << " needs a file\n";
                return 2;
            }
            (argument == "--record" ? options.record : options.replay) = std::filesystem::absolute(argv[++i]).lexically_normal();
            continue;
        }
        kept.push_back(argv[i]);
    }
    if (options.record && options.replay) {
        std::cerr << "[Player] --record and --replay cannot be used together\n";
        return 2;
    }
    auto arguments = maya::split_arguments(int(kept.size()), kept.data());
    if (arguments.positional.size() > 2) {
        std::cerr << "[Player] unexpected argument: " << arguments.positional[2] << '\n';
        return 2;
    }
    if (arguments.positional.size() > 0) // relative to where the player was started
        options.project = std::filesystem::absolute(arguments.positional[0]).lexically_normal();
    if (arguments.positional.size() > 1) options.scene = arguments.positional[1];
    return maya::run_desktop(arguments.host_count(), arguments.host.data(),
        maya::player::create_player_application(std::move(options)),
        {.title = "Maya Player", .capture_cursor = true, .usage = "[project [scene]] [--record file | --replay file]"});
}
