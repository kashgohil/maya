#include "maya/core/engine.hpp"
#include "maya/core/file_system.hpp"
#include <iostream>
#include <optional>
#include <string_view>

int main(int argc, char** argv) {
    maya::FileSystem::initialize(argc, argv);

    auto parse_smoke_frames = [&](int argc_, char** argv_) -> std::optional<uint32_t> {
        for (int i = 1; i < argc_; ++i) {
            const std::string_view a(argv_[i]);
            if (a == "--smoke") {
                if (i + 1 >= argc_) {
                    return 120u;
                }
                try {
                    return static_cast<uint32_t>(std::stoul(argv_[i + 1]));
                } catch (...) {
                    return 120u;
                }
            }
        }
        return std::nullopt;
    };

    const std::optional<uint32_t> smoke_frames = parse_smoke_frames(argc, argv);

    maya::Engine engine;

    if (!engine.initialize()) {
        std::cerr << "Failed to initialize Maya Engine!\n";
        return 1;
    }

    if (smoke_frames) {
        std::cerr << "[Main] smoke: running " << *smoke_frames << " frames\n";
        engine.run_for_frames(*smoke_frames);
        engine.shutdown();
        return 0;
    }

    std::cout << "Maya Engine Initialized with RHI Metal Backend.\n";
    engine.run();

    return 0;
}
