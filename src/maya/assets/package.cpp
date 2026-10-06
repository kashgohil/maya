#include "maya/assets/package.hpp"
#include "maya/assets/asset_cooker.hpp"
#include "maya/assets/material_file.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace maya {
namespace {
std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) return std::nullopt;
    auto bytes = std::vector<std::byte>{};
    std::transform(std::istreambuf_iterator<char>(input), {}, std::back_inserter(bytes), [](char c) { return std::byte(c); });
    if (input.bad()) return std::nullopt;
    return bytes;
}
std::optional<Sha256Digest> parse_digest(std::string_view text) {
    if (text.size() != 64) return std::nullopt;
    auto digest = Sha256Digest{};
    for (size_t i = 0; i < digest.size(); ++i) {
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const auto high = nibble(text[2 * i]), low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        digest[i] = uint8_t(high << 4 | low);
    }
    return digest;
}
} // namespace

Sha256Digest package_content_digest(const std::vector<PackageFile>& files) {
    auto hasher = Sha256{};
    for (const auto& file : files)
        hasher.update(file.path.generic_string() + " " + std::to_string(file.size) + " " + sha256_text(file.digest) + "\n");
    return hasher.finish();
}

void write_package_manifest(std::ostream& output, const PackageManifest& manifest) {
    output << "maya-package 1\n"
           << "name " << std::quoted(manifest.name) << '\n'
           << "build " << std::quoted(manifest.build) << '\n'
           << "project " << std::quoted(manifest.project.generic_string()) << '\n';
    for (const auto& scene : manifest.scenes) output << "scene " << std::quoted(scene.generic_string()) << '\n';
    for (const auto& file : manifest.files)
        output << "file " << std::quoted(file.path.generic_string()) << ' ' << file.size << ' ' << sha256_text(file.digest) << '\n';
    output << "content " << sha256_text(manifest.content) << '\n';
}

PackageManifestResult read_package_manifest(std::istream& input) {
    auto result = PackageManifestResult{};
    auto& manifest = result.manifest;
    auto line = std::string{};
    if (!std::getline(input, line) || line != "maya-package 1") {
        result.error = "not a Maya package manifest, or another version";
        return result;
    }
    auto content = std::optional<Sha256Digest>{};
    for (size_t number = 2; std::getline(input, line); ++number) {
        if (line.empty()) continue;
        auto words = std::istringstream(line);
        auto key = std::string{};
        words >> key;
        auto text = std::string{};
        const auto fail = [&](const std::string& why) {
            result.error = "line " + std::to_string(number) + ": " + why;
            return result;
        };
        if (key == "name" || key == "build" || key == "project" || key == "scene") {
            if (!(words >> std::quoted(text))) return fail("'" + key + "' needs a quoted value");
            if (key == "name") manifest.name = text;
            else if (key == "build") manifest.build = text;
            else if (key == "project") manifest.project = text;
            else manifest.scenes.emplace_back(text);
        } else if (key == "file") {
            auto file = PackageFile{};
            auto digest = std::string{};
            if (!(words >> std::quoted(text) >> file.size >> digest)) return fail("'file' needs a quoted path, a size, and a digest");
            const auto parsed = parse_digest(digest);
            if (!parsed) return fail("'" + digest + "' is not a SHA-256 digest");
            file.path = text;
            file.digest = *parsed;
            manifest.files.push_back(std::move(file));
        } else if (key == "content") {
            auto digest = std::string{};
            words >> digest;
            content = parse_digest(digest);
            if (!content) return fail("'content' needs a SHA-256 digest");
        } else {
            return fail("unknown key '" + key + "'");
        }
    }
    if (manifest.scenes.empty()) result.error = "the manifest names no scene";
    else if (!content) result.error = "the manifest has no content digest";
    else if (*content != package_content_digest(manifest.files)) result.error = "the content digest does not match the files";
    else manifest.content = *content;
    return result;
}

std::string verify_package(const std::filesystem::path& resources) {
    auto input = std::ifstream(resources / package_manifest_name);
    if (!input) return "no " + std::string(package_manifest_name) + " in " + resources.string();
    const auto read = read_package_manifest(input);
    if (!read) return std::string(package_manifest_name) + ": " + read.error;
    auto named = std::vector<std::string>{};
    for (const auto& file : read.manifest.files) {
        const auto bytes = read_file(resources / file.path);
        if (!bytes) return file.path.generic_string() + " is missing";
        if (bytes->size() != file.size || sha256(*bytes) != file.digest) return file.path.generic_string() + " differs from the manifest";
        named.push_back(file.path.generic_string());
    }
    std::ranges::sort(named);
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(resources, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file()) continue;
        const auto relative = it->path().lexically_relative(resources).generic_string();
        if (relative != package_manifest_name && !std::ranges::binary_search(named, relative))
            return relative + " is not in the manifest";
    }
    if (error) return "cannot list " + resources.string() + ": " + error.message();
    return {};
}

PackageAssetProvider::PackageAssetProvider(GraphicsDevice& device) : m_device(device), m_lifetime(device.resource_lifetime()) {}

namespace {
/// A cooked file's payload, unwrapped and checked, or why not.
template<class Asset>
std::optional<std::vector<std::byte>> cooked_payload(const std::filesystem::path& path, const char* extension, AssetLoadResult<Asset>& failure) {
    if (path.extension() != extension) {
        failure = {nullptr, {AssetError::invalid_data, path.string() + ": a package holds only cooked content (" + extension + ")"}};
        return std::nullopt;
    }
    const auto bytes = read_file(path);
    if (!bytes) {
        failure = {nullptr, {AssetError::missing_file, path.string() + ": cannot read the cooked file"}};
        return std::nullopt;
    }
    auto payload = unwrap_cooked(*bytes);
    if (!payload) failure = {nullptr, {AssetError::invalid_data, path.string() + ": the cooked file is damaged"}};
    return payload;
}
} // namespace

AssetLoadResult<MeshAsset> PackageAssetProvider::load_mesh(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Package provider's graphics session has ended"}};
    auto failure = AssetLoadResult<MeshAsset>{};
    const auto payload = cooked_payload(path, cooked_mesh_extension, failure);
    if (!payload) return failure;
    const auto mesh = read_cooked_mesh(*payload);
    if (!mesh) return {nullptr, {AssetError::invalid_data, path.string() + ": not a cooked mesh"}};
    return upload_mesh(m_device, *mesh);
}

AssetLoadResult<MaterialAsset> PackageAssetProvider::load_material(const std::filesystem::path& path) {
    auto input = std::ifstream(path);
    if (!input) return {nullptr, {AssetError::missing_file, "Cannot read material: " + path.string()}};
    auto read = read_material_file(input);
    if (!read) return {nullptr, {AssetError::invalid_data, path.string() + ": " + read.error}};
    return {std::make_shared<const MaterialAsset>(read.material), {}};
}

AssetLoadResult<TextureAsset> PackageAssetProvider::load_texture(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Package provider's graphics session has ended"}};
    auto failure = AssetLoadResult<TextureAsset>{};
    const auto payload = cooked_payload(path, cooked_texture_extension, failure);
    if (!payload) return failure;
    const auto texture = read_cooked_texture(*payload);
    if (!texture) return {nullptr, {AssetError::invalid_data, path.string() + ": not a cooked texture"}};
    if (is_compressed_format(texture->image.format) && !m_device.limits().astc)
        return {nullptr, {AssetError::load_failed, path.string() + ": the texture is ASTC, which this device cannot sample"}};
    return upload_texture(m_device, *texture, path.stem().string());
}

AssetLoadResult<EnvironmentAsset> PackageAssetProvider::load_environment(const std::filesystem::path& path) {
    if (m_lifetime.expired()) return {nullptr, {AssetError::device_unavailable, "Package provider's graphics session has ended"}};
    auto failure = AssetLoadResult<EnvironmentAsset>{};
    const auto payload = cooked_payload(path, cooked_environment_extension, failure);
    if (!payload) return failure;
    const auto environment = read_cooked_environment(*payload);
    if (!environment) return {nullptr, {AssetError::invalid_data, path.string() + ": not a cooked environment"}};
    return upload_environment(m_device, *environment, path.stem().string());
}

} // namespace maya
