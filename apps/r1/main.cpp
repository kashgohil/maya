// maya_r1: assembles R1, the realistic reference environment (#1040, docs/acceptance.md#r1), from the fetched
// samples into a project folder.
//   maya_r1 [--compression astc|rgba8] <samples folder> <output folder>
// Exits 0 when written, 1 when assembling fails, 2 for bad arguments, and 77 when the samples are not fetched.
#include "r1.hpp"
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    auto options = maya::r1::Options{};
    auto positional = std::vector<std::string_view>{};
    for (int i = 1; i < argc; ++i) {
        const auto argument = std::string_view(argv[i]);
        if (argument == "--compression") {
            const auto value = i + 1 < argc ? std::string_view(argv[++i]) : std::string_view{};
            if (value == "astc") options.compression = maya::TextureCompression::astc;
            else if (value == "rgba8") options.compression = maya::TextureCompression::rgba8;
            else {
                std::cerr << "[R1] --compression needs astc or rgba8\n";
                return 2;
            }
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 2) {
        std::cerr << "usage: maya_r1 [--compression astc|rgba8] <samples folder> <output folder>\n";
        return 2;
    }
    options.samples = std::filesystem::absolute(positional[0]).lexically_normal();
    options.output = std::filesystem::absolute(positional[1]).lexically_normal();
    if (!maya::r1::samples_present(options.samples)) {
        std::cerr << "[R1] the samples are not in " << options.samples.string() << "; run tools/fetch_render_samples.sh\n";
        return 77;
    }
    const auto result = maya::r1::assemble(options);
    if (!result) {
        std::cerr << "[R1] " << result.error << '\n';
        return 1;
    }
    std::cout << "[R1] recipe " << maya::r1::recipe_version << " (" << maya::texture_compression_name(options.compression) << ") in "
              << options.output.string() << '\n';
    return 0;
}
