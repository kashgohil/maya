#include "maya/assets/asset.hpp"
#include "maya/core/model_loader.hpp"
#include <cmath>
#include <fstream>

namespace maya {
AssetLoadResult<MeshAsset> FileAssetProvider::load_mesh(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {{},{AssetError::device_unavailable,"Mesh provider's graphics session has ended"}};
    if (path.extension() != ".obj") return {{},{AssetError::invalid_data,"Initial mesh provider expects a triangulated .obj: " + path.string()}};
    auto loaded = ModelLoader::load_obj_checked(m_device,path.string());
    if (!loaded.mesh) return {{},{AssetError::load_failed,std::move(loaded.diagnostic)}};
    return {std::make_shared<const MeshAsset>(std::move(loaded.mesh)),{}};
}
AssetLoadResult<MaterialAsset> FileAssetProvider::load_material(const std::filesystem::path& path) {
    auto input = std::ifstream(path);
    if (!input) return {{},{AssetError::missing_file,"Cannot read material: " + path.string()}};
    std::string magic, color_key, metallic_key, roughness_key, extra;
    unsigned version=0;
    auto material = MaterialAsset{};
    auto& color = material.base_color;
    if (!(input >> magic >> version >> color_key >> color.x >> color.y >> color.z >> color.w
                >> metallic_key >> material.metallic >> roughness_key >> material.roughness) ||
        magic != "maya-material" || version != 1 || color_key != "base_color" ||
        metallic_key != "metallic" || roughness_key != "roughness" || input >> extra || input.bad())
        return {{},{AssetError::invalid_data,"Expected maya-material 1, base_color RGBA, metallic, roughness: " + path.string()}};
    for (const float value : {color.x,color.y,color.z,color.w,material.metallic,material.roughness})
        if (!std::isfinite(value) || value < 0 || value > 1)
            return {{},{AssetError::invalid_data,"Material factors must be finite in [0,1]: " + path.string()}};
    return {std::make_shared<const MaterialAsset>(material),{}};
}
const MaterialAsset& fallback_material() noexcept {
    static const auto material = MaterialAsset{{1,0,1,1},0,1};
    return material;
}
} // namespace maya
