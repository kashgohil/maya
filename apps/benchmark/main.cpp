#include "benchmark.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/window.hpp"
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <fstream>
#include <iostream>
#include <pthread.h>

// maya_benchmark <manifest> [results.json]: runs a benchmark manifest headless and offscreen (or, with
// `present on`, presenting to a window), writes its JSON results (by default <manifest name>.results.json
// in the working directory), and prints a summary. Exit codes: 0 completed, 1 the benchmark failed
// (results record why), 2 usage or manifest.
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || std::string_view(argv[1]) == "--help") {
        std::cerr << "Usage: " << (argc ? argv[0] : "maya_benchmark") << " <manifest.benchmark> [results.json]\n";
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }
    maya::FileSystem::initialize(argc, argv);
    // Measure as foreground work. Started from a script or another tool, the process could otherwise
    // be scheduled as background work partway through a long run (efficiency cores, lower clocks).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
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
    // Presenting runs show the view 1:1: the window's framebuffer is the manifest's resolution.
    auto window = std::unique_ptr<maya::Window>{};
    if (manifest.present) {
        window = std::make_unique<maya::Window>(int(manifest.width), int(manifest.height), "Maya benchmark: " + manifest.name);
        if (!window->get_native_handle()) {
            std::cerr << "[Benchmark] 'present on' needs a window, and none could be opened\n";
            return 1;
        }
        const auto [framebuffer_width, framebuffer_height] = window->framebuffer_size();
        const auto [points_width, points_height] = window->window_size();
        if (framebuffer_width > 0 && framebuffer_width != int(manifest.width))
            window->set_size(int(manifest.width) * points_width / framebuffer_width, int(manifest.height) * points_height / framebuffer_height);
        // A hidden window's frames are never shown: keep it in front for the run.
        window->set_floating(true);
        window->poll_events();
    }
    // A display that sleeps shows nothing: keep it awake while presenting, released on exit.
    auto awake = IOPMAssertionID{kIOPMNullAssertionID};
    if (manifest.present)
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                    CFSTR("Maya benchmark presenting to a window"), &awake);
    auto device = maya::GraphicsDevice::create_default();
    if (!device->initialize(window ? window->get_native_handle() : nullptr, {3, size_t{manifest.upload_mib} << 20})) {
        std::cerr << "[Benchmark] the graphics device could not start\n";
        return 1;
    }
    if (window) {
        const auto [width, height] = window->framebuffer_size();
        device->resize(uint32_t(width), uint32_t(height));
    }
    const auto poll = std::function<void()>([&] { if (window) window->poll_events(); });
    const auto result = maya::benchmark::run(manifest, *device, shader, poll);
    device->shutdown();
    window.reset();
    if (awake != kIOPMNullAssertionID) IOPMAssertionRelease(awake);
    auto file = std::ofstream(output);
    file << maya::benchmark::to_json(result);
    if (!file) {
        std::cerr << "[Benchmark] cannot write " << output.string() << '\n';
        return 1;
    }
    std::cout << maya::benchmark::to_text(result) << "  results: " << std::filesystem::absolute(output).string() << '\n';
    return result.failure.empty() ? 0 : 1;
}
