#include "player_application.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/desktop_application.hpp"
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    // The player's own options (--record <file>, --replay <file>, --debug-physics, --debug-skeletons, and --debug-view <name>) are taken out
    // first; the rest are a project file or folder, then a scene inside its content folder, and the
    // host's options.
    auto options = maya::player::PlayerOptions{};
    auto kept = std::vector<char*>{};
    for (int i = 0; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--debug-physics") {
            options.debug_physics = true;
            continue;
        }
        if (argument == "--debug-skeletons") {
            options.debug_skeletons = true;
            continue;
        }
        if (argument == "--debug-view") {
            const auto view = i + 1 < argc ? maya::debug_view_named(argv[i + 1]) : std::nullopt;
            if (!view) {
                std::cerr << "[Player] --debug-view needs a view: none, luminance, false-color, base-color, normals, shading-normals, "
                             "metallic, roughness, occlusion, emissive, direct-light, environment-light, lighting, cascades, or texels\n";
                return 2;
            }
            options.debug_view = *view;
            ++i;
            continue;
        }
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
    maya::FileSystem::initialize(argc, argv); // finds a package, as run_desktop does again
    if (maya::FileSystem::package_resources() && arguments.positional.size() == 1) {
        options.scene = arguments.positional[0]; // a package runs its own project: the one argument is one of its scenes
    } else {
        if (arguments.positional.size() > 0) // relative to where the player was started
            options.project = std::filesystem::absolute(arguments.positional[0]).lexically_normal();
        if (arguments.positional.size() > 1) options.scene = arguments.positional[1];
    }
    return maya::run_desktop(arguments.host_count(), arguments.host.data(),
        maya::player::create_player_application(std::move(options)),
        {.title = "Maya Player", .capture_cursor = true, .usage = "[project [scene]] [--record file | --replay file] [--debug-physics] [--debug-skeletons] [--debug-view name]"});
}
