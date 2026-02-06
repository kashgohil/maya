#include "maya/core/engine.hpp"
#include <iostream>

int main() {
    maya::Engine engine;
    
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize Maya Engine!" << std::endl;
        return -1;
    }

    std::cout << "Maya Engine Initialized with RHI Metal Backend." << std::endl;
    engine.run();

    return 0;
}
