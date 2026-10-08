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

namespace {
/// The finalize step for a cooked result: upload on the owner thread, unless the device session ended.
template<class Asset, class Cooked, class Upload>
PreparedAsset upload_later(CookResult<Cooked> cooked, size_t bytes, std::weak_ptr<const GraphicsResourceLifetime> lifetime, Upload upload) {
    if (!cooked) return {std::move(cooked.diagnostic), 0, {}};
    auto value = std::make_shared<Cooked>(std::move(*cooked.value));
    return {{}, bytes, [value, lifetime, upload = std::move(upload)]() -> std::pair<AssetValue, AssetDiagnostic> {
        if (lifetime.expired()) return {std::shared_ptr<const Asset>{}, {AssetError::device_unavailable, "the graphics session has ended"}};
        auto result = upload(*value);
        return {std::move(result.value), std::move(result.diagnostic)};
    }};
}
template<class Asset> PreparedAsset ready_now(AssetLoadResult<Asset> loaded) {
    if (loaded.diagnostic) return {std::move(loaded.diagnostic), 0, {}};
    return {{}, 0, [value = std::move(loaded.value)]() -> std::pair<AssetValue, AssetDiagnostic> { return {value, {}}; }};
}
size_t cooked_bytes(const CookedMesh& mesh) {
    return mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * sizeof(uint32_t) + mesh.skin.size() * sizeof(SkinVertex);
}
} // namespace

PreparedAsset FileAssetProvider::prepare(const AssetLoadRequest& request) {
    if (m_lifetime.expired()) return {{AssetError::device_unavailable, "The asset provider's graphics session has ended"}, 0, {}};
    const auto lock = std::lock_guard(m_cook_mutex);
    m_cooker->set_limits(cook_limits(m_device));
    m_cooker->set_tier(request.tier);
    const auto& path = request.path;
    const auto label = request.part.empty() ? path.string() : path.string() + "#" + request.part;
    const auto name = request.part.empty() ? path.stem().string() : path.stem().string() + " " + request.part;
    if (request.stopped()) return {{AssetError::cancelled, label + ": the load was cancelled"}, 0, {}}; // while waiting its turn
    auto& device = m_device;
    const auto mesh = [&](CookResult<CookedMesh> cooked) {
        const auto bytes = cooked ? cooked_bytes(*cooked.value) : 0;
        return upload_later<MeshAsset>(std::move(cooked), bytes, m_lifetime, [&device, label](const CookedMesh& value) {
            auto result = upload_mesh(device, value);
            if (result.diagnostic) result.diagnostic.message = label + ": " + result.diagnostic.message;
            return result;
        });
    };
    const auto texture = [&](CookResult<CookedTexture> cooked) {
        const auto bytes = cooked ? cooked.value->image.data.size() : 0;
        return upload_later<TextureAsset>(std::move(cooked), bytes, m_lifetime, [&device, label, name](const CookedTexture& value) {
            auto result = upload_texture(device, value, name);
            if (result.diagnostic) result.diagnostic.message = label + ": " + result.diagnostic.message;
            return result;
        });
    };
    switch (request.kind) {
    case AssetKind::mesh: return mesh(request.part.empty() ? m_cooker->mesh(path) : m_cooker->imported_mesh(path, request.part));
    case AssetKind::texture: return texture(request.part.empty() ? m_cooker->texture(path) : m_cooker->imported_texture(path, request.part));
    case AssetKind::environment: {
        auto cooked = m_cooker->environment(path);
        const auto bytes = cooked ? cooked.value->background.size() + cooked.value->specular.size() : 0;
        return upload_later<EnvironmentAsset>(std::move(cooked), bytes, m_lifetime, [&device, label, name](const CookedEnvironment& value) {
            auto result = upload_environment(device, value, name);
            if (result.diagnostic) result.diagnostic.message = label + ": " + result.diagnostic.message;
            return result;
        });
    }
    case AssetKind::material: return ready_now(load_material(path));
    case AssetKind::script: return ready_now(load_script(path));
    case AssetKind::skin: return ready_now(request.part.empty() ? load_skin(path) : load_imported_skin(path, request.part));
    case AssetKind::animation: return ready_now(request.part.empty() ? load_animation(path) : load_imported_animation(path, request.part));
    }
    return {{AssetError::wrong_type, "Unsupported asset kind"}, 0, {}};
}

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

AssetLoadResult<SkinAsset> FileAssetProvider::load_imported_skin(const std::filesystem::path& source, std::string_view part) {
    auto cooked = m_cooker->imported_skin(source, part);
    if (!cooked) return {nullptr, std::move(cooked.diagnostic)};
    return {std::make_shared<const SkinAsset>(std::move(*cooked.value)), {}};
}
AssetLoadResult<AnimationAsset> FileAssetProvider::load_imported_animation(const std::filesystem::path& source, std::string_view part) {
    auto cooked = m_cooker->imported_animation(source, part);
    if (!cooked) return {nullptr, std::move(cooked.diagnostic)};
    return {std::make_shared<const AnimationAsset>(std::move(*cooked.value)), {}};
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
