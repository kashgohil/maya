#include "maya/core/model_loader.hpp"
#include "maya/core/file_system.hpp"
#include <iostream>

namespace maya {
std::unique_ptr<Mesh> ModelLoader::load_obj(GraphicsDevice& device, const std::string& path) {
    const auto resolved = FileSystem::resolve(path);
    if (!resolved) { std::cerr << "Cannot resolve OBJ: " << path << '\n'; return {}; }
    auto result = load_obj_checked(device,resolved->string());
    if (!result.mesh) std::cerr << result.diagnostic << '\n';
    return std::move(result.mesh);
}
} // namespace maya
