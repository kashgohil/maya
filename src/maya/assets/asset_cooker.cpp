#include "maya/assets/asset_cooker.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/gltf.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/texture_cook.hpp"
#include "maya/core/model_loader.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>

namespace maya {
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
std::string texture_cook_settings(const TextureSettings& settings, const CookLimits& limits) {
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
template<class T> CookResult<T> failure(AssetError code, std::string message) { return {std::nullopt, {code, std::move(message)}}; }
/// A source a descriptor names, which must sit at or below the descriptor's folder, symlinks included.
std::optional<std::filesystem::path> named_source(const std::filesystem::path& descriptor, const std::filesystem::path& name,
                                                  AssetDiagnostic& problem, const char* what) {
    std::error_code error;
    const auto folder = std::filesystem::canonical(descriptor.parent_path(), error);
    if (error) {
        problem = {AssetError::missing_file, std::string("cannot resolve the ") + what + " file's folder"};
        return std::nullopt;
    }
    const auto source = std::filesystem::weakly_canonical(folder / name, error);
    const auto relative = source.lexically_relative(folder);
    if (error || relative.empty() || *relative.begin() == "..") {
        problem = {AssetError::invalid_path, "source '" + name.generic_string() + "' leaves the " + what + " file's folder"};
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(source, error)) {
        problem = {AssetError::missing_file, "source '" + name.generic_string() + "' is missing"};
        return std::nullopt;
    }
    return source;
}
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

// The keys entries are written under, one function each, shared by cooking and by cook_key (what pruning
// keeps, #1063): a key the cooking writes and cook_key does not compute would be pruned and cooked again.
CookKey texture_key(const TextureSettings& settings, const CookLimits& limits) {
    return {"texture", texture_cook_version, {}, texture_cook_settings(settings, limits)};
}
uint32_t environment_max_dimension(const CookLimits& limits) noexcept { return std::min(limits.max_texture_dimension, 8192u); }
CookKey environment_key(const EnvironmentSettings& settings, const CookLimits& limits) {
    return {"environment", environment_cook_version, {}, "specular_size " + std::to_string(settings.specular_size) + "\nsamples " +
            std::to_string(settings.samples) + "\nmax_dimension " + std::to_string(environment_max_dimension(limits)) + "\n"};
}
CookKey part_key(const char* kind, uint32_t version, std::string_view part, const std::string& settings = {}) {
    return {kind, version, {}, "part " + std::string(part) + "\n" + settings};
}
/// An imported texture's settings: its role from the part, its compression and mips from the import file
/// when there is one (the defaults otherwise). An error message, or empty.
std::string imported_texture_settings(const std::filesystem::path& source, std::string_view part, TextureSettings& settings) {
    uint32_t index = 0;
    auto role_name = std::string{};
    if (!parse_part(part, "texture", index, role_name)) return "a texture part is 'texture/<texture>/<role>'";
    if (role_name == "color") settings.role = TextureRole::color;
    else if (role_name == "data") settings.role = TextureRole::data;
    else if (role_name == "normal") settings.role = TextureRole::normal;
    else return "the role '" + role_name + "' is not color, data, or normal";
    if (auto input = std::ifstream(import_file_path(source))) {
        const auto read = read_import_file(input);
        if (!read) return import_file_path(source).filename().string() + ": " + read.error;
        settings.compression = read.file.settings.compression;
        settings.mips = read.file.settings.mips;
    }
    return {};
}
using SamplerWords = std::array<uint32_t, 6>;
SamplerWords sampler_words(const SamplerDesc& s) {
    return {uint32_t(s.min_filter), uint32_t(s.mag_filter), uint32_t(s.address_u), uint32_t(s.address_v), uint32_t(s.mip_filter), s.max_anisotropy};
}
SamplerDesc sampler_from(const SamplerWords& w) {
    return {Filter(w[0]), Filter(w[1]), AddressMode(w[2]), AddressMode(w[3]), {}, MipFilter(w[4]), w[5]};
}
} // namespace

struct AssetCooker::OpenGltf {
    std::filesystem::path source;
    std::filesystem::file_time_type time;
    uintmax_t size = 0;
    std::shared_ptr<const GltfFile> file;
};
AssetCooker::AssetCooker(CookLimits limits, std::shared_ptr<CookCache> cache) : m_limits(limits), m_cache(std::move(cache)) {}
AssetCooker::~AssetCooker() = default;

std::optional<CookKey> AssetCooker::cook_key(AssetKind kind, const std::filesystem::path& path, std::string_view part) {
    auto key = std::optional<CookKey>{};
    if (!part.empty()) {
        if (kind == AssetKind::mesh) key = part_key("mesh", imported_mesh_cook_version, part);
        else if (kind == AssetKind::skin) key = part_key("skin", skin_cook_version, part);
        else if (kind == AssetKind::animation) key = part_key("animation", animation_cook_version, part);
        else if (kind == AssetKind::texture) {
            auto settings = TextureSettings{};
            if (!imported_texture_settings(path, part, settings).empty()) return std::nullopt;
            key = part_key("imported-texture", imported_texture_cook_version, part, texture_cook_settings(settings, m_limits));
        } else return std::nullopt;
        const auto digest = imported_digest(path);
        if (!digest) return std::nullopt;
        key->source = *digest;
        return key;
    }
    auto source = std::optional<std::filesystem::path>{};
    auto problem = AssetDiagnostic{};
    if (kind == AssetKind::texture) {
        auto input = std::ifstream(path);
        if (!input) return std::nullopt;
        const auto read = read_texture_settings(input);
        if (!read) return std::nullopt;
        source = named_source(path, read.settings.source, problem, "texture");
        const auto extension = source ? lowercase_extension(*source) : std::string{};
        if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") return std::nullopt; // KTX2 is read as it is
        key = texture_key(read.settings, m_limits);
    } else if (kind == AssetKind::environment) {
        auto input = std::ifstream(path);
        if (!input) return std::nullopt;
        const auto read = read_environment_settings(input);
        if (!read) return std::nullopt;
        source = named_source(path, read.settings.source, problem, "environment");
        key = environment_key(read.settings, m_limits);
    }
    if (!key || !source || !m_cache) return std::nullopt;
    const auto digest = m_cache->source_digest(*source);
    if (!digest) return std::nullopt;
    key->source = *digest;
    return key;
}

CookResult<CookedMesh> AssetCooker::mesh(const std::filesystem::path& path) {
    if (path.extension() != ".obj") return failure<CookedMesh>(AssetError::invalid_data, "Initial mesh provider expects a triangulated .obj: " + path.string());
    auto parsed = ModelLoader::parse_obj(path.string());
    if (!parsed) return failure<CookedMesh>(AssetError::load_failed, std::move(parsed.diagnostic));
    return {CookedMesh{std::move(parsed.vertices), std::move(parsed.indices), {}}, {}};
}

CookResult<CookedTexture> AssetCooker::texture(const std::filesystem::path& path) {
    const auto failed = [&](AssetError code, const std::string& message) { return failure<CookedTexture>(code, path.string() + ": " + message); };
    auto input = std::ifstream(path);
    if (!input) return failed(AssetError::missing_file, "cannot read the texture file");
    const auto read = read_texture_settings(input);
    if (!read) return failed(AssetError::invalid_data, read.error);
    const auto& settings = read.settings;
    auto problem = AssetDiagnostic{};
    const auto source = named_source(path, settings.source, problem, "texture");
    if (!source) return failed(problem.code, problem.message);
    const auto bytes = read_bytes(*source);
    if (!bytes) return failed(AssetError::missing_file, "cannot read source '" + settings.source.generic_string() + "'");

    const auto extension = lowercase_extension(*source);
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
        if (is_compressed_format(image.format) && !m_limits.astc)
            return failed(AssetError::load_failed, "source '" + settings.source.generic_string() + "' is ASTC, which this device cannot sample");
    } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
        // Cooked before with these settings: read back from the cache.
        auto key = texture_key(settings, m_limits);
        if (m_cache) {
            key.source = m_cache->source_digest(*source, *bytes);
            if (const auto entry = m_cache->read(key))
                if (auto cached = read_ktx2(*entry)) image = std::move(cached.image);
        }
        if (image.data.empty()) {
            auto decoded = decode_image(*bytes, m_limits.max_texture_dimension);
            if (!decoded) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + decoded.error);
            auto cooked = cook_texture(decoded.image, settings, {m_limits.astc, 0, m_tier});
            if (!cooked) return failed(AssetError::load_failed, "cooking '" + settings.source.generic_string() + "' failed: " + cooked.error);
            image = std::move(cooked.image);
            if (m_cache) m_cache->write(key, write_ktx2(image));
        }
    } else {
        return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "' must be a .png, .jpg, .jpeg, or .ktx2 file");
    }
    return {CookedTexture{std::move(image), settings.sampler, settings.role}, {}};
}

CookResult<CookedEnvironment> AssetCooker::environment(const std::filesystem::path& path) {
    const auto failed = [&](AssetError code, const std::string& message) { return failure<CookedEnvironment>(code, path.string() + ": " + message); };
    auto input = std::ifstream(path);
    if (!input) return failed(AssetError::missing_file, "cannot read the environment file");
    const auto read = read_environment_settings(input);
    if (!read) return failed(AssetError::invalid_data, read.error);
    const auto& settings = read.settings;
    auto problem = AssetDiagnostic{};
    const auto source = named_source(path, settings.source, problem, "environment");
    if (!source) return failed(problem.code, problem.message);
    if (lowercase_extension(*source) != ".hdr")
        return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "' must be a Radiance .hdr file");
    const auto bytes = read_bytes(*source);
    if (!bytes) return failed(AssetError::missing_file, "cannot read source '" + settings.source.generic_string() + "'");
    const auto max_dimension = environment_max_dimension(m_limits);
    auto key = environment_key(settings, m_limits);
    auto cooked = std::optional<CookedEnvironment>{};
    if (m_cache) {
        key.source = m_cache->source_digest(*source, *bytes);
        if (const auto entry = m_cache->read(key)) cooked = read_cooked_environment(*entry);
    }
    if (!cooked) {
        const auto decoded = decode_hdr_image(*bytes, max_dimension);
        if (!decoded) return failed(AssetError::invalid_data, "source '" + settings.source.generic_string() + "': " + decoded.error);
        cooked = cook_environment(decoded.image, settings, 0, m_tier);
        if (m_cache) m_cache->write(key, write_cooked_environment(*cooked));
    }
    return {std::move(cooked), {}};
}

std::shared_ptr<const GltfFile> AssetCooker::open_gltf(const std::filesystem::path& source, std::string& error) {
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

std::optional<Sha256Digest> AssetCooker::imported_digest(const std::filesystem::path& source) {
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

CookResult<CookedMesh> AssetCooker::imported_mesh(const std::filesystem::path& source, std::string_view part) {
    const auto failed = [&](AssetError code, const std::string& message) { return failure<CookedMesh>(code, part_name(source, part) + ": " + message); };
    uint32_t mesh = 0, primitive = 0;
    auto rest = std::string{};
    if (!parse_part(part, "mesh", mesh, rest) || std::from_chars(rest.data(), rest.data() + rest.size(), primitive).ptr != rest.data() + rest.size())
        return failed(AssetError::invalid_path, "a mesh part is 'mesh/<mesh>/<primitive>'");
    // Cooked before: the welded vertices and indices, read without parsing the file.
    auto key = part_key("mesh", imported_mesh_cook_version, part);
    if (m_cache)
        if (const auto digest = imported_digest(source)) {
            key.source = *digest;
            if (const auto entry = m_cache->read(key))
                if (auto cached = read_cooked_mesh(*entry)) return {std::move(cached), {}};
        }
    auto error = std::string{};
    const auto file = open_gltf(source, error);
    if (!file) return failed(AssetError::invalid_data, error);
    auto geometry = file->primitive(mesh, primitive);
    if (!geometry) return failed(AssetError::invalid_data, geometry.error);
    auto cooked = CookedMesh{std::move(geometry.vertices), std::move(geometry.indices), std::move(geometry.skin)};
    if (m_cache && key.source != Sha256Digest{}) m_cache->write(key, write_cooked_mesh(cooked));
    return {std::move(cooked), {}};
}

CookResult<CookedTexture> AssetCooker::imported_texture(const std::filesystem::path& source, std::string_view part) {
    const auto failed = [&](AssetError code, const std::string& message) { return failure<CookedTexture>(code, part_name(source, part) + ": " + message); };
    uint32_t index = 0;
    auto role_name = std::string{};
    if (!parse_part(part, "texture", index, role_name)) return failed(AssetError::invalid_path, "a texture part is 'texture/<texture>/<role>'");
    // The import's settings, when the import file is there; the defaults otherwise.
    auto settings = TextureSettings{};
    if (auto problem = imported_texture_settings(source, part, settings); !problem.empty())
        return failed(problem.starts_with("a texture part") || problem.starts_with("the role") ? AssetError::invalid_path : AssetError::invalid_data, problem);
    // Cooked before: the sampler's description, then the KTX2 file, read without parsing the source.
    auto key = part_key("imported-texture", imported_texture_cook_version, part, texture_cook_settings(settings, m_limits));
    if (m_cache)
        if (const auto digest = imported_digest(source)) {
            key.source = *digest;
            if (const auto entry = m_cache->read(key)) {
                auto in = std::span<const std::byte>(*entry);
                auto sampler = SamplerWords{};
                if (take(in, sampler))
                    if (auto cached = read_ktx2(in)) return {CookedTexture{std::move(cached.image), sampler_from(sampler), settings.role}, {}};
            }
        }
    auto error = std::string{};
    const auto file = open_gltf(source, error);
    if (!file) return failed(AssetError::invalid_data, error);
    const auto& document = file->document();
    if (index >= document.textures.size()) return failed(AssetError::invalid_data, "there is no texture " + std::to_string(index));
    const auto& texture = document.textures[index];
    if (!texture.image) return failed(AssetError::invalid_data, "textures[" + std::to_string(index) + "] has no image Maya reads");
    const auto bytes = file->image(*texture.image);
    if (!bytes) return failed(AssetError::invalid_data, bytes.error);
    const auto decoded = decode_image(bytes.bytes, m_limits.max_texture_dimension);
    if (!decoded) return failed(AssetError::invalid_data, "images[" + std::to_string(*texture.image) + "]: " + decoded.error);
    // glTF's sampler, with Maya's anisotropy where it filters between mips.
    settings.sampler = texture.sampler;
    settings.sampler.max_anisotropy = settings.sampler.mip_filter == MipFilter::linear && settings.sampler.min_filter == Filter::linear ? 8 : 1;
    if (settings.sampler.mip_filter == MipFilter::none) settings.mips = false;
    auto cooked = cook_texture(decoded.image, settings, {m_limits.astc, 0, m_tier});
    if (!cooked) return failed(AssetError::load_failed, "cooking failed: " + cooked.error);
    if (m_cache && key.source != Sha256Digest{}) {
        auto entry = std::vector<std::byte>{};
        put(entry, sampler_words(settings.sampler));
        const auto file_bytes = write_ktx2(cooked.image);
        entry.insert(entry.end(), file_bytes.begin(), file_bytes.end());
        m_cache->write(key, entry);
    }
    return {CookedTexture{std::move(cooked.image), settings.sampler, settings.role}, {}};
}

CookResult<SkinAsset> AssetCooker::imported_skin(const std::filesystem::path& source, std::string_view part) {
    const auto failed = [&](AssetError code, const std::string& message) { return failure<SkinAsset>(code, part_name(source, part) + ": " + message); };
    uint32_t index = 0;
    if (!part.starts_with("skin/") || std::from_chars(part.data() + 5, part.data() + part.size(), index).ptr != part.data() + part.size())
        return failed(AssetError::invalid_path, "a skin part is 'skin/<skin>'");
    auto key = part_key("skin", skin_cook_version, part);
    if (m_cache)
        if (const auto digest = imported_digest(source)) {
            key.source = *digest;
            if (const auto entry = m_cache->read(key))
                if (auto cached = read_skin(*entry)) return {std::move(cached), {}};
        }
    auto error = std::string{};
    const auto file = open_gltf(source, error);
    if (!file) return failed(AssetError::invalid_data, error);
    const auto& document = file->document();
    if (index >= document.skins.size()) return failed(AssetError::invalid_data, "there is no skin " + std::to_string(index));
    const auto paths = gltf_node_paths(document);
    const auto& skin = document.skins[index];
    auto cooked = SkinAsset{};
    for (const auto joint : skin.joints) {
        if (paths[joint].empty()) return failed(AssetError::invalid_data, "joint node " + std::to_string(joint) + " is not in the file's scene");
        cooked.joints.push_back(paths[joint]);
    }
    cooked.inverse_bind = skin.inverse_bind;
    if (m_cache && key.source != Sha256Digest{}) m_cache->write(key, write_skin(cooked));
    return {std::move(cooked), {}};
}

CookResult<AnimationAsset> AssetCooker::imported_animation(const std::filesystem::path& source, std::string_view part) {
    const auto failed = [&](AssetError code, const std::string& message) { return failure<AnimationAsset>(code, part_name(source, part) + ": " + message); };
    uint32_t index = 0;
    if (!part.starts_with("animation/") || std::from_chars(part.data() + 10, part.data() + part.size(), index).ptr != part.data() + part.size())
        return failed(AssetError::invalid_path, "an animation part is 'animation/<animation>'");
    auto key = part_key("animation", animation_cook_version, part);
    if (m_cache)
        if (const auto digest = imported_digest(source)) {
            key.source = *digest;
            if (const auto entry = m_cache->read(key))
                if (auto cached = read_animation(*entry)) return {std::move(cached), {}};
        }
    auto error = std::string{};
    const auto file = open_gltf(source, error);
    if (!file) return failed(AssetError::invalid_data, error);
    const auto& document = file->document();
    if (index >= document.animations.size()) return failed(AssetError::invalid_data, "there is no animation " + std::to_string(index));
    const auto paths = gltf_node_paths(document);
    const auto& animation = document.animations[index];
    auto cooked = AnimationAsset{animation.name, animation.duration, {}};
    for (const auto& channel : animation.channels) {
        if (paths[channel.node].empty()) continue; // a node outside the scene: nothing it moves is drawn
        cooked.channels.push_back({paths[channel.node], channel.path, channel.interpolation, channel.times, channel.values});
    }
    if (m_cache && key.source != Sha256Digest{}) m_cache->write(key, write_animation(cooked));
    return {std::move(cooked), {}};
}

CookLimits cook_limits(const GraphicsDevice& device) noexcept {
    return {device.limits().astc, device.limits().max_texture_dimension};
}

AssetLoadResult<MeshAsset> upload_mesh(GraphicsDevice& device, const CookedMesh& mesh) {
    auto loaded = std::make_unique<Mesh>(device, mesh.vertices, mesh.indices, mesh.skin);
    if (!loaded->valid()) return {nullptr, {AssetError::load_failed, "GPU mesh allocation failed or the device session is unavailable"}};
    return {std::make_shared<const MeshAsset>(std::move(loaded), MeshGeometry::from(mesh.vertices, mesh.indices)), {}};
}
AssetLoadResult<TextureAsset> upload_texture(GraphicsDevice& device, const CookedTexture& cooked, const std::string& label) {
    auto texture = std::make_unique<Texture>(device, cooked.image.desc(label), cooked.image.data);
    if (!texture->valid()) return {nullptr, {AssetError::load_failed, texture->error().message}};
    auto sampler_desc = cooked.sampler;
    sampler_desc.label = label;
    if (cooked.image.mip_levels == 1) sampler_desc.mip_filter = MipFilter::none;
    sampler_desc.max_anisotropy = std::min(sampler_desc.max_anisotropy, device.limits().max_anisotropy);
    auto sampler = std::make_unique<Sampler>(device, sampler_desc);
    if (!sampler->valid()) return {nullptr, {AssetError::load_failed, sampler->error().message}};
    return {std::make_shared<const TextureAsset>(std::move(texture), std::move(sampler), cooked.role), {}};
}
AssetLoadResult<EnvironmentAsset> upload_environment(GraphicsDevice& device, const CookedEnvironment& cooked, const std::string& label) {
    auto background = std::make_unique<Texture>(device, TextureDesc{cooked.background_width, cooked.background_height, Format::rgba16_float,
        TextureUsage::sampled, label + " background", cooked.background_levels}, cooked.background);
    if (!background->valid()) return {nullptr, {AssetError::load_failed, background->error().message}};
    auto specular = std::make_unique<Texture>(device, TextureDesc{cooked.specular_size, cooked.specular_size, Format::rgba16_float,
        TextureUsage::sampled, label + " specular", cooked.specular_levels, TextureType::cube}, cooked.specular);
    if (!specular->valid()) return {nullptr, {AssetError::load_failed, specular->error().message}};
    auto sampler = std::make_unique<Sampler>(device, SamplerDesc{Filter::linear, Filter::linear, AddressMode::repeat,
        AddressMode::clamp_to_edge, label, MipFilter::linear, 1});
    if (!sampler->valid()) return {nullptr, {AssetError::load_failed, sampler->error().message}};
    return {std::make_shared<const EnvironmentAsset>(std::move(background), std::move(specular), std::move(sampler),
                                                     cooked.irradiance, cooked.milliseconds), {}};
}

std::vector<std::byte> write_cooked_mesh(const CookedMesh& mesh) {
    auto out = std::vector<std::byte>{};
    put(out, uint64_t(mesh.vertices.size()));
    put(out, uint64_t(mesh.indices.size()));
    const auto* vertices = reinterpret_cast<const std::byte*>(mesh.vertices.data());
    const auto* indices = reinterpret_cast<const std::byte*>(mesh.indices.data());
    out.insert(out.end(), vertices, vertices + mesh.vertices.size() * sizeof(Vertex));
    out.insert(out.end(), indices, indices + mesh.indices.size() * sizeof(uint32_t));
    put(out, uint64_t(mesh.skin.size()));
    const auto* skin = reinterpret_cast<const std::byte*>(mesh.skin.data());
    out.insert(out.end(), skin, skin + mesh.skin.size() * sizeof(SkinVertex));
    return out;
}
std::optional<CookedMesh> read_cooked_mesh(std::span<const std::byte> in) {
    uint64_t vertices = 0, indices = 0, skinned = 0;
    if (!take(in, vertices) || !take(in, indices) || vertices == 0 || indices == 0 || indices % 3 != 0) return std::nullopt;
    if (vertices > in.size() / sizeof(Vertex) || indices > in.size() / sizeof(uint32_t) ||
        in.size() < vertices * sizeof(Vertex) + indices * sizeof(uint32_t) + sizeof(uint64_t)) return std::nullopt;
    auto mesh = CookedMesh{};
    mesh.vertices.resize(vertices, Vertex{{}, {}, {}});
    mesh.indices.resize(indices);
    std::memcpy(mesh.vertices.data(), in.data(), vertices * sizeof(Vertex));
    std::memcpy(mesh.indices.data(), in.data() + vertices * sizeof(Vertex), indices * sizeof(uint32_t));
    in = in.subspan(vertices * sizeof(Vertex) + indices * sizeof(uint32_t));
    if (!take(in, skinned) || (skinned != 0 && skinned != vertices) || in.size() != skinned * sizeof(SkinVertex)) return std::nullopt;
    mesh.skin.resize(skinned);
    if (skinned) std::memcpy(mesh.skin.data(), in.data(), skinned * sizeof(SkinVertex));
    if (std::ranges::any_of(mesh.indices, [&](uint32_t i) { return i >= vertices; })) return std::nullopt;
    return mesh;
}
std::vector<std::byte> write_cooked_texture(const CookedTexture& texture) {
    auto out = std::vector<std::byte>{};
    put(out, uint32_t(texture.role));
    put(out, sampler_words(texture.sampler));
    const auto file = write_ktx2(texture.image);
    out.insert(out.end(), file.begin(), file.end());
    return out;
}
std::optional<CookedTexture> read_cooked_texture(std::span<const std::byte> in) {
    uint32_t role = 0;
    auto sampler = SamplerWords{};
    if (!take(in, role) || role > uint32_t(TextureRole::normal) || !take(in, sampler)) return std::nullopt;
    auto image = read_ktx2(in);
    if (!image) return std::nullopt;
    return CookedTexture{std::move(image.image), sampler_from(sampler), TextureRole(role)};
}

} // namespace maya
