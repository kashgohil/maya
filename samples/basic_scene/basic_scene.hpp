#pragma once

#include "maya/core/application.hpp"
#include <memory>

namespace maya::samples {
std::unique_ptr<Application> create_basic_scene();
}
