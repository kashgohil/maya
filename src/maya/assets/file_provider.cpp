#include "maya/assets/asset.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/environment_cook.hpp"
#include "maya/assets/gltf.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/texture_cook.hpp"
#include "maya/core/model_loader.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>

namespace maya {
struct FileAssetProvider::OpenGltf {
    std::filesystem::path source;
    std::filesystem::file_time_type time;
    uintmax_t size = 0;
    std::shared_ptr<const GltfFile> file;
};
FileAssetProvider::FileAssetProvider(GraphicsDevice& device, std::shared_ptr<CookCache> cache)
    : m_device(device), m_lifetime(device.resource_lifetime()), m_cache(std::move(cache)) {}
FileAssetProvider::~FileAssetProvider() = default;

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
/// What changes a cooked texture besides its source: its settings and whether the device samples ASTC.
std::string texture_cook_settings(const TextureSettings& settings, const RhiLimits& limits) {
    return std::string("role ") + texture_role_name(settings.role) + "\ncompression " + texture_compression_name(settings.compression) +
           "\nmips " + (settings.mips ? "on" : "off") + "\nastc " + (limits.astc ? "on" : "off") + "\n";
}
template<class T> void put(std::vector<std::byte>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
template<class T> bool take(std::span<const std::byte>& in, T& value) {
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&value, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
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
        // Cooked before with these settings: read back from the cache.
        auto key = CookKey{"texture", texture_cook_version, {}, texture_cook_settings(settings, limits)};
        if (m_cache) {
            key.source = m_cache->source_digest(source, *bytes);
            if (const auto entry = m_cache->read(key))
                if (auto cached = read_ktx2(*entry)) image = std::move(cached.image);
        }
        if (image.data.empty()) {
            auto decoded = decode_image(*bytes, limits.max_texture_dimension);
            if (!decoded) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + decoded.error);
            auto cooked = cook_texture(decoded.image, settings, {limits.astc, 0});
            if (!cooked) return failed(AssetError::load_failed, "cooking '" + settings.source.generic_string() + "' failed: " + cooked.error);
            image = std::move(cooked.image);
            if (m_cache) m_cache->write(key, write_ktx2(image));
        }
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
    const auto max_dimension = std::min(m_device.limits().max_texture_dimension, 8192u);
    auto key = CookKey{"environment", environment_cook_version, {}, "specular_size " + std::to_string(settings.specular_size) +
                       "\nsamples " + std::to_string(settings.samples) + "\nmax_dimension " + std::to_string(max_dimension) + "\n"};
    auto cooked = std::optional<CookedEnvironment>{};
    if (m_cache) {
        key.source = m_cache->source_digest(source, *bytes);
        if (const auto entry = m_cache->read(key)) cooked = read_cooked_environment(*entry);
    }
    if (!cooked) {
        const auto decoded = decode_hdr_image(*bytes, max_dimension);
        if (!decoded) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + decoded.error);
        cooked = cook_environment(decoded.image, settings);
        if (m_cache) m_cache->write(key, write_cooked_environment(*cooked));
    }
    const auto label = path.stem().string();
    auto background = std::make_unique<Texture>(m_device, TextureDesc{cooked->background_width, cooked->background_height, Format::rgba16_float,
        TextureUsage::sampled, label + " background", cooked->background_levels}, cooked->background);
    if (!background->valid()) return failed(AssetError::load_failed, background->error().message);
    auto specular = std::make_unique<Texture>(m_device, TextureDesc{cooked->specular_size, cooked->specular_size, Format::rgba16_float,
        TextureUsage::sampled, label + " specular", cooked->specular_levels, TextureType::cube}, cooked->specular);
    if (!specular->valid()) return failed(AssetError::load_failed, specular->error().message);
    auto sampler = std::make_unique<Sampler>(m_device, SamplerDesc{Filter::linear, Filter::linear, AddressMode::repeat,
        AddressMode::clamp_to_edge, label, MipFilter::linear, 1});
    if (!sampler->valid()) return failed(AssetError::load_failed, sampler->error().message);
    return {std::make_shared<const EnvironmentAsset>(std::move(background), std::move(specular), std::move(sampler),
                                                     cooked->irradiance, cooked->milliseconds), {}};
}

namespace {
/// The numbers and last word of a part: "mesh/0/1" or "texture/2/color".
bool parse_part(std::string_view part, std::string_view kind, uint32_t& first, std::string& rest) {
    if (!part.starts_with(kind) || part.size() <= kind.size() || part[kind.size()] != '/') return false;
    part.remove_prefix(kind.size() + 1);
    const auto slash = part.find('/');
    if (slash == std::string_view::npos) return false;
    const auto parsed = std::from_chars(part.data(), part.data() + slash, first);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + slash) return false;
    rest = std::string(part.substr(slash + 1));
    return !rest.empty();
}
std::string part_name(const std::filesystem::path& source, std::string_view part) { return source.string() + "#" + std::string(part); }
} // namespace

std::shared_ptr<const GltfFile> FileAssetProvider::open_gltf(const std::filesystem::path& source, std::string& error) {
    std::error_code failed;
    const auto time = std::filesystem::last_write_time(source, failed);
    const auto size = failed ? 0 : std::filesystem::file_size(source, failed);
    if (failed) {
        error = "cannot read the file";
        return nullptr;
    }
    if (m_gltf && m_gltf->source == source && m_gltf->time == time && m_gltf->size == size) return m_gltf->file;
    m_gltf.reset(); // one file's data at a time
    auto opened = GltfFile::open(source);
    if (!opened) {
        error = gltf_problem_text(opened.errors.front());
        if (opened.errors.size() > 1) error += " (and " + std::to_string(opened.errors.size() - 1) + " more problems)";
        return nullptr;
    }
    m_gltf = std::make_unique<OpenGltf>(OpenGltf{source, time, size, std::shared_ptr<const GltfFile>(std::move(opened.file))});
    return m_gltf->file;
}

std::optional<Sha256Digest> FileAssetProvider::imported_digest(const std::filesystem::path& source) {
    if (!m_cache) return std::nullopt;
    auto input = std::ifstream(import_file_path(source));
    if (!input) return std::nullopt;
    const auto imported = read_import_file(input);
    if (!imported) return std::nullopt;
    const auto digest = m_cache->source_digest(source);
    if (!digest) return std::nullopt;
    auto combined = Sha256{};
    combined.update(sha256_text(*digest));
    for (const auto& named : imported.file.files) {
        const auto file = m_cache->source_digest(source.parent_path() / named);
        if (!file) return std::nullopt;
        combined.update("\n" + named.generic_string() + " " + sha256_text(*file));
    }
    return combined.finish();
}

AssetLoadResult<MeshAsset> FileAssetProvider::load_imported_mesh(const std::filesystem::path& source, std::string_view part) {
    const auto failed = [&](AssetError code, const std::string& message) -> AssetLoadResult<MeshAsset> {
        return {nullptr, {code, part_name(source, part) + ": " + message}};
    };
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Mesh provider's graphics session has ended"}};
    uint32_t mesh = 0, primitive = 0;
    auto rest = std::string{};
    if (!parse_part(part, "mesh", mesh, rest) || std::from_chars(rest.data(), rest.data() + rest.size(), primitive).ptr != rest.data() + rest.size())
        return failed(AssetError::invalid_path, "a mesh part is 'mesh/<mesh>/<primitive>'");
    // Cooked before: the welded vertices and indices, read without parsing the file.
    auto geometry = GltfGeometry{};
    auto key = CookKey{"mesh", imported_mesh_cook_version, {}, "part " + std::string(part) + "\n"};
    if (m_cache)
        if (const auto digest = imported_digest(source)) {
            key.source = *digest;
            if (const auto entry = m_cache->read(key)) {
                auto in = std::span<const std::byte>(*entry);
                uint64_t vertices = 0, indices = 0;
                if (take(in, vertices) && take(in, indices) && in.size() == vertices * sizeof(Vertex) + indices * sizeof(uint32_t)) {
                    geometry.vertices.resize(vertices, Vertex{{}, {}, {}});
                    geometry.indices.resize(indices);
                    std::memcpy(geometry.vertices.data(), in.data(), vertices * sizeof(Vertex));
                    std::memcpy(geometry.indices.data(), in.data() + vertices * sizeof(Vertex), indices * sizeof(uint32_t));
                }
            }
        }
    if (geometry.vertices.empty()) {
        auto error = std::string{};
        const auto file = open_gltf(source, error);
        if (!file) return failed(AssetError::invalid_data, error);
        geometry = file->primitive(mesh, primitive);
        if (!geometry) return failed(AssetError::invalid_data, geometry.error);
        if (m_cache && key.source != Sha256Digest{}) {
            auto entry = std::vector<std::byte>{};
            put(entry, uint64_t(geometry.vertices.size()));
            put(entry, uint64_t(geometry.indices.size()));
            const auto* vertices = reinterpret_cast<const std::byte*>(geometry.vertices.data());
            const auto* indices = reinterpret_cast<const std::byte*>(geometry.indices.data());
            entry.insert(entry.end(), vertices, vertices + geometry.vertices.size() * sizeof(Vertex));
            entry.insert(entry.end(), indices, indices + geometry.indices.size() * sizeof(uint32_t));
            m_cache->write(key, entry);
        }
    }
    auto loaded = std::make_unique<Mesh>(m_device, geometry.vertices, geometry.indices);
    if (!loaded->valid()) return failed(AssetError::load_failed, "GPU mesh allocation failed or the device session is unavailable");
    return {std::make_shared<const MeshAsset>(std::move(loaded), MeshGeometry::from(geometry.vertices, geometry.indices)), {}};
}

AssetLoadResult<TextureAsset> FileAssetProvider::load_imported_texture(const std::filesystem::path& source, std::string_view part) {
    const auto failed = [&](AssetError code, const std::string& message) -> AssetLoadResult<TextureAsset> {
        return {nullptr, {code, part_name(source, part) + ": " + message}};
    };
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Texture provider's graphics session has ended"}};
    uint32_t index = 0;
    auto role_name = std::string{};
    if (!parse_part(part, "texture", index, role_name)) return failed(AssetError::invalid_path, "a texture part is 'texture/<texture>/<role>'");
    auto settings = TextureSettings{};
    if (role_name == "color") settings.role = TextureRole::color;
    else if (role_name == "data") settings.role = TextureRole::data;
    else if (role_name == "normal") settings.role = TextureRole::normal;
    else return failed(AssetError::invalid_path, "the role '" + role_name + "' is not color, data, or normal");
    // The import's settings, when the import file is there; the defaults otherwise.
    if (auto input = std::ifstream(import_file_path(source))) {
        const auto read = read_import_file(input);
        if (!read) return failed(AssetError::invalid_data, import_file_path(source).filename().string() + ": " + read.error);
        settings.compression = read.file.settings.compression;
        settings.mips = read.file.settings.mips;
    }
    const auto& limits = m_device.limits();
    // Cooked before: the sampler's description, then the KTX2 file, read without parsing the source.
    auto image = TextureImage{};
    auto key = CookKey{"imported-texture", imported_texture_cook_version, {}, "part " + std::string(part) + "\n" +
                       texture_cook_settings(settings, limits)};
    if (m_cache)
        if (const auto digest = imported_digest(source)) {
            key.source = *digest;
            if (const auto entry = m_cache->read(key)) {
                auto in = std::span<const std::byte>(*entry);
                auto sampler = std::array<uint32_t, 6>{};
                if (take(in, sampler))
                    if (auto cached = read_ktx2(in)) {
                        image = std::move(cached.image);
                        settings.sampler = {Filter(sampler[0]), Filter(sampler[1]), AddressMode(sampler[2]), AddressMode(sampler[3]), {},
                                            MipFilter(sampler[4]), sampler[5]};
                    }
            }
        }
    if (image.data.empty()) {
        auto error = std::string{};
        const auto file = open_gltf(source, error);
        if (!file) return failed(AssetError::invalid_data, error);
        const auto& document = file->document();
        if (index >= document.textures.size()) return failed(AssetError::invalid_data, "there is no texture " + std::to_string(index));
        const auto& texture = document.textures[index];
        if (!texture.image) return failed(AssetError::invalid_data, "textures[" + std::to_string(index) + "] has no image Maya reads");
        const auto bytes = file->image(*texture.image);
        if (!bytes) return failed(AssetError::invalid_data, bytes.error);
        const auto decoded = decode_image(bytes.bytes, limits.max_texture_dimension);
        if (!decoded) return failed(AssetError::invalid_data, "images[" + std::to_string(*texture.image) + "]: " + decoded.error);
        // glTF's sampler, with Maya's anisotropy where it filters between mips.
        settings.sampler = texture.sampler;
        settings.sampler.max_anisotropy = settings.sampler.mip_filter == MipFilter::linear && settings.sampler.min_filter == Filter::linear ? 8 : 1;
        if (settings.sampler.mip_filter == MipFilter::none) settings.mips = false;
        auto cooked = cook_texture(decoded.image, settings, {limits.astc, 0});
        if (!cooked) return failed(AssetError::load_failed, "cooking failed: " + cooked.error);
        image = std::move(cooked.image);
        if (m_cache && key.source != Sha256Digest{}) {
            auto entry = std::vector<std::byte>{};
            const auto& s = settings.sampler;
            put(entry, std::array<uint32_t, 6>{uint32_t(s.min_filter), uint32_t(s.mag_filter), uint32_t(s.address_u), uint32_t(s.address_v),
                                               uint32_t(s.mip_filter), s.max_anisotropy});
            const auto file_bytes = write_ktx2(image);
            entry.insert(entry.end(), file_bytes.begin(), file_bytes.end());
            m_cache->write(key, entry);
        }
    }
    const auto label = source.stem().string() + " " + std::string(part);
    auto gpu = std::make_unique<Texture>(m_device, image.desc(label), image.data);
    if (!gpu->valid()) return failed(AssetError::load_failed, gpu->error().message);
    auto sampler_desc = settings.sampler;
    sampler_desc.label = label;
    if (image.mip_levels == 1) sampler_desc.mip_filter = MipFilter::none;
    sampler_desc.max_anisotropy = std::min(sampler_desc.max_anisotropy, limits.max_anisotropy);
    auto sampler = std::make_unique<Sampler>(m_device, sampler_desc);
    if (!sampler->valid()) return failed(AssetError::load_failed, sampler->error().message);
    return {std::make_shared<const TextureAsset>(std::move(gpu), std::move(sampler), settings.role), {}};
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
