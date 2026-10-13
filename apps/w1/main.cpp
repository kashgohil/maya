// maya_w1: assembles W1, the reference world (#1069, docs/acceptance.md#w1), from the fetched samples into a
// project folder.
//   maya_w1 [--compression astc|rgba8] <world samples folder> <render samples folder> <output folder>
// Exits 0 when written, 1 when assembling fails, 2 for bad arguments, and 77 when the samples are not fetched.
#include "w1.hpp"
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    auto options = maya::w1::Options{};
    auto positional = std::vector<std::string_view>{};
    for (int i = 1; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--compression") {
            const auto value = i + 1 < argc ? std::string_view(argv[++i]) : std::string_view{};
            if (value == "astc") options.compression = maya::TextureCompression::astc;
            else if (value == "rgba8") options.compression = maya::TextureCompression::rgba8;
            else {
                std::cerr << "[W1] --compression needs astc or rgba8\n";
                return 2;
            }
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 3) {
        std::cerr << "usage: maya_w1 [--compression astc|rgba8] <world samples folder> <render samples folder> <output folder>\n";
        return 2;
    }
    options.world_samples = std::filesystem::absolute(positional[0]).lexically_normal();
    options.render_samples = std::filesystem::absolute(positional[1]).lexically_normal();
    options.output = std::filesystem::absolute(positional[2]).lexically_normal();
    if (const auto missing = maya::w1::missing_samples(options.world_samples, options.render_samples); !missing.empty()) {
        std::cerr << "[W1] " << missing << "; run tools/fetch_world_samples.sh and tools/fetch_render_samples.sh\n";
        return 77;
    }
    const auto result = maya::w1::assemble(options);
    if (!result) {
        std::cerr << "[W1] " << result.error << '\n';
        return 1;
    }
    std::cout << "[W1] recipe " << maya::w1::recipe_version << " (" << maya::texture_compression_name(options.compression) << "): "
              << result.entities << " entities in " << result.cells << " cells, in " << options.output.string() << '\n';
    return 0;
}
