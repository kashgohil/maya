#pragma once

#include "maya/math/types.hpp"

namespace maya {

class Application {
public:
    virtual ~Application() = default;

    virtual bool on_initialize() { return true; }
    virtual void on_shutdown() {}

    virtual void on_update(f32 delta_time) = 0;
    virtual void on_render() {}

    virtual void on_resize(u32 /*width*/, u32 /*height*/) {}
};

} // namespace maya
