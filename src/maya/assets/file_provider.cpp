#include "maya/assets/asset.hpp"
#include "maya/assets/asset_cooker.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/material_file.hpp"
#include <array>
#include <fstream>

namespace maya {
FileAssetProvider::FileAssetProvider(GraphicsDevice& device, std::shared_ptr<CookCache> cache)
    : m_device(device), m_lifetime(device.resource_lifetime()), m_cooker(std::make_unique<AssetCooker>(cook_limits(device), std::move(cache))) {}
FileAssetProvider::~FileAssetProvider() = default;

namespace {
/// Loading is cooking (asset_cooker.hpp), then upload, both refused once the device session has ended.
template<class Asset, class Cooked, class Upload>
AssetLoadResult<Asset> loaded(CookResult<Cooked> cooked, Upload&& upload) {
    if (!cooked) return {nullptr, std::move(cooked.diagnostic)};
    return upload(*cooked.value);
}
} // namespace

AssetLoadResult<MeshAsset> FileAssetProvider::load_mesh(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {{},{AssetError::device_unavailable,"Mesh provider's graphics session has ended"}};
    m_cooker->set_limits(cook_limits(m_device));
    return loaded<MeshAsset>(m_cooker->mesh(path), [&](const CookedMesh& mesh) {
        auto result = upload_mesh(m_device, mesh);
        if (result.diagnostic) result.diagnostic.message = path.string() + ": " + result.diagnostic.message;
        return result;
    });
}
AssetLoadResult<MaterialAsset> FileAssetProvider::load_material(const std::filesystem::path& path) {
    auto input = std::ifstream(path);
    if (!input) return {{},{AssetError::missing_file,"Cannot read material: " + path.string()}};
    auto read = read_material_file(input);
    if (!read) return {{},{AssetError::invalid_data,path.string() + ": " + read.error}};
    return {std::make_shared<const MaterialAsset>(read.material),{}};
}
AssetLoadResult<TextureAsset> FileAssetProvider::load_texture(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Texture provider's graphics session has ended"}};
    m_cooker->set_limits(cook_limits(m_device));
    return loaded<TextureAsset>(m_cooker->texture(path), [&](const CookedTexture& texture) {
        auto result = upload_texture(m_device, texture, path.stem().string());
        if (result.diagnostic) result.diagnostic.message = path.string() + ": " + result.diagnostic.message;
        return result;
    });
}
AssetLoadResult<EnvironmentAsset> FileAssetProvider::load_environment(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Environment provider's graphics session has ended"}};
    m_cooker->set_limits(cook_limits(m_device));
    return loaded<EnvironmentAsset>(m_cooker->environment(path), [&](const CookedEnvironment& environment) {
        auto result = upload_environment(m_device, environment, path.stem().string());
        if (result.diagnostic) result.diagnostic.message = path.string() + ": " + result.diagnostic.message;
        return result;
    });
}
AssetLoadResult<MeshAsset> FileAssetProvider::load_imported_mesh(const std::filesystem::path& source, std::string_view part) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Mesh provider's graphics session has ended"}};
    m_cooker->set_limits(cook_limits(m_device));
    return loaded<MeshAsset>(m_cooker->imported_mesh(source, part), [&](const CookedMesh& mesh) {
        auto result = upload_mesh(m_device, mesh);
        if (result.diagnostic) result.diagnostic.message = source.string() + "#" + std::string(part) + ": " + result.diagnostic.message;
        return result;
    });
}
AssetLoadResult<TextureAsset> FileAssetProvider::load_imported_texture(const std::filesystem::path& source, std::string_view part) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Texture provider's graphics session has ended"}};
    m_cooker->set_limits(cook_limits(m_device));
    return loaded<TextureAsset>(m_cooker->imported_texture(source, part), [&](const CookedTexture& texture) {
        auto result = upload_texture(m_device, texture, source.stem().string() + " " + std::string(part));
        if (result.diagnostic) result.diagnostic.message = source.string() + "#" + std::string(part) + ": " + result.diagnostic.message;
        return result;
    });
}

std::span<const std::byte> placeholder_texture_pixels() noexcept {
    static const auto pixels = [] {
        auto texels = std::array<std::byte, placeholder_texture_size * placeholder_texture_size * 4>{};
        for (uint32_t y = 0; y < placeholder_texture_size; ++y)
            for (uint32_t x = 0; x < placeholder_texture_size; ++x) {
                const auto magenta = (x / 2 + y / 2) % 2 == 0; // 2x2-texel squares
                auto* texel = texels.data() + (size_t{y} * placeholder_texture_size + x) * 4;
                texel[0] = std::byte{magenta ? uint8_t{255} : uint8_t{0}};
                texel[1] = std::byte{0};
                texel[2] = texel[0];
                texel[3] = std::byte{255};
            }
        return texels;
    }();
    return pixels;
}

std::shared_ptr<const TextureAsset> make_placeholder_texture(GraphicsDevice& device) {
    auto texture = std::make_unique<Texture>(device, TextureDesc{placeholder_texture_size, placeholder_texture_size, Format::rgba8_srgb,
                                                                 TextureUsage::sampled, "texture placeholder"}, placeholder_texture_pixels());
    auto sampler = std::make_unique<Sampler>(device, SamplerDesc{Filter::nearest, Filter::nearest, AddressMode::repeat,
                                                                 AddressMode::repeat, "texture placeholder"});
    if (!texture->valid() || !sampler->valid()) return nullptr;
    return std::make_shared<const TextureAsset>(std::move(texture), std::move(sampler), TextureRole::color);
}

const MaterialAsset& fallback_material() noexcept {
    static const auto material = MaterialAsset{{1,0,1,1},0,1}; // magenta, rough, untextured
    return material;
}
} // namespace maya
