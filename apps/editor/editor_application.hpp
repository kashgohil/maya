#pragma once

#include "maya/core/application.hpp"
#include <memory>

namespace maya::editor {
std::unique_ptr<Application> create_editor_application();
}
