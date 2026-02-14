#pragma once

#include "maya/core/mesh.hpp"
#include <string>
#include <memory>

namespace maya {

class ModelLoader {
public:
    static std::unique_ptr<Mesh> load_obj(GraphicsDevice& device, const std::string& path);
};

} // namespace maya
