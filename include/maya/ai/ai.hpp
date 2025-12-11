#pragma once

#include "maya/math/types.hpp"

namespace maya {

class AISystem {
public:
    static AISystem& instance();

    bool initialize();
    void shutdown();

    void update(f32 delta_time);

private:
    AISystem() = default;
    ~AISystem() = default;

    AISystem(const AISystem&) = delete;
    AISystem& operator=(const AISystem&) = delete;
};

} // namespace maya
