#include "benchmark.hpp"
#include "maya/core/file_system.hpp"
#include <fstream>
#include <iostream>

// maya_benchmark <manifest> [results.json]: runs a benchmark manifest headless and offscreen, writes
// its JSON results (by default <manifest name>.results.json in the working directory), and prints a
// summary. Exit codes: 0 completed, 1 the benchmark failed (results record why), 2 usage or manifest.
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || std::string_view(argv[1]) == "--help") {
        std::cerr << "Usage: " << (argc ? argv[0] : "maya_benchmark") << " <manifest.benchmark> [results.json]\n";
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }
    maya::FileSystem::initialize(argc, argv);
    const auto loaded = maya::benchmark::load_manifest(argv[1]);
    if (!loaded) {
        std::cerr << "[Benchmark] " << loaded.error << '\n';
        return 2;
    }
    const auto& manifest = loaded.manifest;
    const auto output = argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path(manifest.name + ".results.json");
    const auto shader = maya::FileSystem::read_text("resources/shaders/metal/renderer.metal");
    if (shader.empty()) {
        std::cerr << "[Benchmark] the renderer shader was not found (resources/shaders/metal/renderer.metal)\n";
        return 1;
    }
    auto device = maya::GraphicsDevice::create_default();
    if (!device->initialize(nullptr, {3, size_t{manifest.upload_mib} << 20})) {
        std::cerr << "[Benchmark] the graphics device could not start\n";
        return 1;
    }
    const auto result = maya::benchmark::run(manifest, *device, shader);
    device->shutdown();
    auto file = std::ofstream(output);
    file << maya::benchmark::to_json(result);
    if (!file) {
        std::cerr << "[Benchmark] cannot write " << output.string() << '\n';
        return 1;
    }
    std::cout << maya::benchmark::to_text(result) << "  results: " << std::filesystem::absolute(output).string() << '\n';
    return result.failure.empty() ? 0 : 1;
}
