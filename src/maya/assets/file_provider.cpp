#include "maya/assets/asset.hpp"
#include "maya/assets/environment_cook.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/texture_cook.hpp"
#include "maya/core/model_loader.hpp"
#include <array>
#include <cmath>
#include <fstream>

namespace maya {
AssetLoadResult<MeshAsset> FileAssetProvider::load_mesh(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {{},{AssetError::device_unavailable,"Mesh provider's graphics session has ended"}};
    if (path.extension() != ".obj") return {{},{AssetError::invalid_data,"Initial mesh provider expects a triangulated .obj: " + path.string()}};
    auto loaded = ModelLoader::load_obj_checked(m_device,path.string());
    if (!loaded.mesh) return {{},{AssetError::load_failed,std::move(loaded.diagnostic)}};
    return {std::make_shared<const MeshAsset>(std::move(loaded.mesh), std::move(loaded.geometry)),{}};
}
AssetLoadResult<MaterialAsset> FileAssetProvider::load_material(const std::filesystem::path& path) {
    auto input = std::ifstream(path);
    if (!input) return {{},{AssetError::missing_file,"Cannot read material: " + path.string()}};
    auto read = read_material_file(input);
    if (!read) return {{},{AssetError::invalid_data,path.string() + ": " + read.error}};
    return {std::make_shared<const MaterialAsset>(read.material),{}};
}
namespace {
std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file) return std::nullopt;
    auto bytes = std::vector<std::byte>{};
    auto buffer = std::array<char, 1 << 16>{};
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        const auto* begin = reinterpret_cast<const std::byte*>(buffer.data());
        bytes.insert(bytes.end(), begin, begin + file.gcount());
    }
    if (file.bad()) return std::nullopt;
    return bytes;
}
std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return extension;
}
} // namespace

AssetLoadResult<TextureAsset> FileAssetProvider::load_texture(const std::filesystem::path& path) {
    const auto failed = [&](AssetError code, const std::string& message) -> AssetLoadResult<TextureAsset> {
        return {nullptr, {code, path.string() + ": " + message}};
    };
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Texture provider's graphics session has ended"}};
    auto input = std::ifstream(path);
    if (!input) return failed(AssetError::missing_file, "cannot read the texture file");
    const auto read = read_texture_settings(input);
    if (!read) return failed(AssetError::invalid_data, read.error);
    const auto& settings = read.settings;

    // The source sits at or below the descriptor's folder, symlinks included.
    std::error_code error;
    const auto folder = std::filesystem::canonical(path.parent_path(), error);
    if (error) return failed(AssetError::missing_file, "cannot resolve the texture file's folder");
    const auto source = std::filesystem::weakly_canonical(folder / settings.source, error);
    const auto relative = source.lexically_relative(folder);
    if (error || relative.empty() || *relative.begin() == "..")
        return failed(AssetError::invalid_path, "source '" + settings.source.generic_string() + "' leaves the texture file's folder");
    if (!std::filesystem::is_regular_file(source, error))
        return failed(AssetError::missing_file, "source '" + settings.source.generic_string() + "' is missing");
    const auto bytes = read_bytes(source);
    if (!bytes) return failed(AssetError::missing_file, "cannot read source '" + settings.source.generic_string() + "'");

    const auto& limits = m_device.limits();
    const auto extension = lowercase_extension(source);
    auto image = TextureImage{};
    if (extension == ".ktx2") {
        auto cooked = read_ktx2(*bytes);
        if (!cooked) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + cooked.error);
        image = std::move(cooked.image);
        // A cooked file must be what the settings describe.
        const auto expected = texture_format(settings.role, settings.compression);
        if (is_srgb_format(image.format) != is_srgb_format(expected) || is_compressed_format(image.format) != is_compressed_format(expected))
            return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "' is " + format_name(image.format) +
                          ", but usage " + texture_role_name(settings.role) + " with compression " +
                          texture_compression_name(settings.compression) + " needs " + format_name(expected));
        const auto full = full_mip_count(image.width, image.height);
        if (settings.mips ? image.mip_levels != full : image.mip_levels != 1)
            return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "' has " + std::to_string(image.mip_levels) +
                          " mip level(s); mips " + (settings.mips ? "on needs " + std::to_string(full) : std::string("off needs 1")));
        if (is_compressed_format(image.format) && !limits.astc)
            return failed(AssetError::load_failed, "source '" + settings.source.generic_string() + "' is ASTC, which this device cannot sample");
    } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
        auto decoded = decode_image(*bytes, limits.max_texture_dimension);
        if (!decoded) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + decoded.error);
        auto cooked = cook_texture(decoded.image, settings, {limits.astc, 0});
        if (!cooked) return failed(AssetError::load_failed, "cooking '" + settings.source.generic_string() + "' failed: " + cooked.error);
        image = std::move(cooked.image);
    } else {
        return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "' must be a .png, .jpg, .jpeg, or .ktx2 file");
    }

    const auto label = path.stem().string();
    auto texture = std::make_unique<Texture>(m_device, image.desc(label), image.data);
    if (!texture->valid()) return failed(AssetError::load_failed, texture->error().message);
    auto sampler_desc = settings.sampler;
    sampler_desc.label = label;
    if (image.mip_levels == 1) sampler_desc.mip_filter = MipFilter::none;
    sampler_desc.max_anisotropy = std::min(sampler_desc.max_anisotropy, limits.max_anisotropy);
    auto sampler = std::make_unique<Sampler>(m_device, sampler_desc);
    if (!sampler->valid()) return failed(AssetError::load_failed, sampler->error().message);
    return {std::make_shared<const TextureAsset>(std::move(texture), std::move(sampler), settings.role), {}};
}

AssetLoadResult<EnvironmentAsset> FileAssetProvider::load_environment(const std::filesystem::path& path) {
    const auto failed = [&](AssetError code, const std::string& message) -> AssetLoadResult<EnvironmentAsset> {
        return {nullptr, {code, path.string() + ": " + message}};
    };
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Environment provider's graphics session has ended"}};
    auto input = std::ifstream(path);
    if (!input) return failed(AssetError::missing_file, "cannot read the environment file");
    const auto read = read_environment_settings(input);
    if (!read) return failed(AssetError::invalid_data, read.error);
    const auto& settings = read.settings;

    // The source sits at or below the file's folder, symlinks included, as a texture's does.
    std::error_code error;
    const auto folder = std::filesystem::canonical(path.parent_path(), error);
    if (error) return failed(AssetError::missing_file, "cannot resolve the environment file's folder");
    const auto source = std::filesystem::weakly_canonical(folder / settings.source, error);
    const auto relative = source.lexically_relative(folder);
    if (error || relative.empty() || *relative.begin() == "..")
        return failed(AssetError::invalid_path, "source '" + settings.source.generic_string() + "' leaves the environment file's folder");
    if (!std::filesystem::is_regular_file(source, error))
        return failed(AssetError::missing_file, "source '" + settings.source.generic_string() + "' is missing");
    if (lowercase_extension(source) != ".hdr")
        return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "' must be a Radiance .hdr file");
    const auto bytes = read_bytes(source);
    if (!bytes) return failed(AssetError::missing_file, "cannot read source '" + settings.source.generic_string() + "'");
    const auto decoded = decode_hdr_image(*bytes, std::min(m_device.limits().max_texture_dimension, 8192u));
    if (!decoded) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + decoded.error);

    const auto cooked = cook_environment(decoded.image, settings);
    const auto label = path.stem().string();
    auto background = std::make_unique<Texture>(m_device, TextureDesc{cooked.background_width, cooked.background_height, Format::rgba16_float,
        TextureUsage::sampled, label + " background", cooked.background_levels}, cooked.background);
    if (!background->valid()) return failed(AssetError::load_failed, background->error().message);
    auto specular = std::make_unique<Texture>(m_device, TextureDesc{cooked.specular_size, cooked.specular_size, Format::rgba16_float,
        TextureUsage::sampled, label + " specular", cooked.specular_levels, TextureType::cube}, cooked.specular);
    if (!specular->valid()) return failed(AssetError::load_failed, specular->error().message);
    auto sampler = std::make_unique<Sampler>(m_device, SamplerDesc{Filter::linear, Filter::linear, AddressMode::repeat,
        AddressMode::clamp_to_edge, label, MipFilter::linear, 1});
    if (!sampler->valid()) return failed(AssetError::load_failed, sampler->error().message);
    return {std::make_shared<const EnvironmentAsset>(std::move(background), std::move(specular), std::move(sampler),
                                                     cooked.irradiance, cooked.milliseconds), {}};
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
