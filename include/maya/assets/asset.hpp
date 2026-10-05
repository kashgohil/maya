#pragma once
#include "maya/assets/asset_ref.hpp"
#include "maya/assets/material.hpp"
#include "maya/assets/texture_data.hpp"
#include "maya/core/mesh.hpp"
#include "maya/core/texture.hpp"
#include "maya/core/sha256.hpp"
#include <array>
#include <concepts>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace maya {
enum class AssetKind { mesh, material, script, texture, environment };
/// "mesh", "material", "script", "texture", or "environment".
const char* asset_kind_name(AssetKind kind) noexcept;
enum class AssetState { unloaded, loading, ready, failed };
enum class AssetError {
    none, invalid_id, duplicate_id, duplicate_path, invalid_path, not_registered,
    wrong_type, stale_handle, busy, missing_file, load_failed, invalid_data, device_unavailable
};
struct AssetDiagnostic {
    AssetError code = AssetError::none;
    std::string message;
    explicit operator bool() const noexcept { return code != AssetError::none; }
};

class MeshAsset {
public:
    /// `geometry` is an optional CPU copy of the triangles; without it the mesh cannot be picked.
    explicit MeshAsset(std::unique_ptr<Mesh> mesh, MeshGeometry geometry = {})
        : m_mesh(std::move(mesh)), m_geometry(std::move(geometry)) {
        if (!m_mesh || !m_mesh->valid()) throw std::invalid_argument("MeshAsset requires a valid mesh");
    }
    const Mesh& mesh() const noexcept { return *m_mesh; }
    const MeshGeometry& geometry() const noexcept { return m_geometry; }
private:
    std::unique_ptr<Mesh> m_mesh;
    MeshGeometry m_geometry;
};
/// Luau source text (docs/scripting.md). Play sessions compile it; bytecode is never stored or loaded.
struct ScriptAsset {
    std::string source;
};
/// A sampled GPU texture with its mip levels, and the sampler its settings describe (docs/assets.md#textures).
class TextureAsset {
public:
    TextureAsset(std::unique_ptr<Texture> texture, std::unique_ptr<Sampler> sampler, TextureRole role)
        : m_texture(std::move(texture)), m_sampler(std::move(sampler)), m_role(role) {
        if (!m_texture || !m_texture->valid() || !m_sampler || !m_sampler->valid())
            throw std::invalid_argument("TextureAsset requires a valid texture and sampler");
    }
    const Texture& texture() const noexcept { return *m_texture; }
    const Sampler& sampler() const noexcept { return *m_sampler; }
    TextureRole role() const noexcept { return m_role; }
    bool valid() const noexcept { return m_texture->valid() && m_sampler->valid(); }
    /// Tracked GPU bytes of every level.
    size_t gpu_bytes() const noexcept { return m_texture->gpu_bytes(); }
private:
    std::unique_ptr<Texture> m_texture;
    std::unique_ptr<Sampler> m_sampler;
    TextureRole m_role;
};
/// An environment for image-based lighting (docs/assets.md#environments): its HDR image as the sky
/// background (equirectangular RGBA16F, with mips), its GGX-prefiltered specular cube (RGBA16F, one level
/// per roughness step), and its irradiance as spherical-harmonic coefficients, cooked when it loads.
class EnvironmentAsset {
public:
    EnvironmentAsset(std::unique_ptr<Texture> background, std::unique_ptr<Texture> specular, std::unique_ptr<Sampler> sampler,
                     std::array<math::Vec3, 9> irradiance, double cook_milliseconds)
        : m_background(std::move(background)), m_specular(std::move(specular)), m_sampler(std::move(sampler)),
          m_irradiance(irradiance), m_cook_milliseconds(cook_milliseconds) {
        if (!m_background || !m_background->valid() || !m_specular || !m_specular->valid() || !m_sampler || !m_sampler->valid())
            throw std::invalid_argument("EnvironmentAsset requires valid textures and a sampler");
    }
    const Texture& background() const noexcept { return *m_background; }
    const Texture& specular() const noexcept { return *m_specular; }
    /// Linear filtering between levels; the background repeats around the horizon.
    const Sampler& sampler() const noexcept { return *m_sampler; }
    const std::array<math::Vec3, 9>& irradiance() const noexcept { return m_irradiance; }
    uint32_t specular_levels() const noexcept { return m_specular->desc().mip_levels; }
    bool valid() const noexcept { return m_background->valid() && m_specular->valid() && m_sampler->valid(); }
    size_t gpu_bytes() const noexcept { return m_background->gpu_bytes() + m_specular->gpu_bytes(); }
    double cook_milliseconds() const noexcept { return m_cook_milliseconds; }
private:
    std::unique_ptr<Texture> m_background, m_specular;
    std::unique_ptr<Sampler> m_sampler;
    std::array<math::Vec3, 9> m_irradiance;
    double m_cook_milliseconds;
};
template<class T> concept Asset = std::same_as<T,MeshAsset> || std::same_as<T,MaterialAsset> || std::same_as<T,ScriptAsset> ||
                                  std::same_as<T,TextureAsset> || std::same_as<T,EnvironmentAsset>;
template<Asset T> inline constexpr AssetKind asset_kind =
    std::same_as<T,MeshAsset> ? AssetKind::mesh : std::same_as<T,MaterialAsset> ? AssetKind::material :
    std::same_as<T,ScriptAsset> ? AssetKind::script : std::same_as<T,TextureAsset> ? AssetKind::texture : AssetKind::environment;

template<Asset T> struct AssetHandle {
    uint64_t registry = 0;
    uint32_t slot = std::numeric_limits<uint32_t>::max();
    uint64_t generation = 0;
    auto operator<=>(const AssetHandle&) const = default;
};
/// A copyable residency lease, owning one immutable loaded version and its resources.
template<Asset T> class AssetLease {
public:
    AssetLease() = default;
    explicit operator bool() const noexcept { return static_cast<bool>(m_value); }
    const T& value() const {
        if (!m_value) throw std::logic_error("Empty asset lease");
        return *m_value;
    }
    AssetHandle<T> handle() const noexcept { return m_handle; }
    AssetRef<T> reference() const noexcept { return m_reference; }
private:
    friend class AssetRegistry;
    AssetLease(std::shared_ptr<const T> value, AssetHandle<T> handle, AssetRef<T> reference)
        : m_value(std::move(value)), m_handle(handle), m_reference(reference) {}
    std::shared_ptr<const T> m_value;
    AssetHandle<T> m_handle;
    AssetRef<T> m_reference;
};
template<Asset T> struct AssetResult {
    AssetLease<T> lease;
    AssetDiagnostic diagnostic;
    explicit operator bool() const noexcept { return static_cast<bool>(lease); }
};
template<Asset T> struct AssetLoadResult {
    std::shared_ptr<const T> value;
    AssetDiagnostic diagnostic;
};
struct AssetRecord {
    AssetId id;
    AssetKind kind;
    std::filesystem::path path; // normalized, project-relative, never a serialized runtime handle
};
/// A catalog path names a file, or a part of an imported glTF file after '#' (docs/import.md#catalog-entries):
/// "models/helmet.glb#mesh/0/1" is primitive 1 of mesh 0, and "models/helmet.glb#texture/2/color" is
/// texture 2 cooked as color. Only a path whose file ends in .gltf or .glb has a part.
struct AssetSourcePath {
    std::filesystem::path file;
    std::string part; // empty for a whole file
};
AssetSourcePath split_asset_path(const std::filesystem::path& path);
struct AssetInfo {
    AssetRecord record;
    AssetState state = AssetState::unloaded;
    uint64_t generation = 0;
    AssetDiagnostic diagnostic; // failed reload may coexist with a ready previous version
};
class AssetProvider {
public:
    virtual ~AssetProvider() = default;
    virtual AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& absolute_path) = 0;
    virtual AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& absolute_path) = 0;
    /// Reads the file as UTF-8 text; providers need not override it.
    virtual AssetLoadResult<ScriptAsset> load_script(const std::filesystem::path& absolute_path);
    /// `absolute_path` is a texture descriptor (.texture). The default refuses: this provider loads no textures.
    virtual AssetLoadResult<TextureAsset> load_texture(const std::filesystem::path& absolute_path);
    /// `absolute_path` is an environment file (.environment). The default refuses.
    virtual AssetLoadResult<EnvironmentAsset> load_environment(const std::filesystem::path& absolute_path);
    /// A mesh or texture inside an imported file: `source` is the glTF file and `part` what follows '#'
    /// in its catalog path. The defaults refuse.
    virtual AssetLoadResult<MeshAsset> load_imported_mesh(const std::filesystem::path& source, std::string_view part);
    virtual AssetLoadResult<TextureAsset> load_imported_texture(const std::filesystem::path& source, std::string_view part);
};
class GltfFile;
class CookCache;
/// Initial adapter: the OBJ loader, material files, texture descriptors whose source is cooked at load
/// (PNG or JPEG) or read as cooked KTX2, environments cooked at load from Radiance HDR images, and the
/// meshes and textures of imported glTF files.
class FileAssetProvider final : public AssetProvider {
public:
    /// With a cache, what loading cooks is read from it when it is there and written to it when not
    /// (docs/assets.md#cook-cache).
    explicit FileAssetProvider(GraphicsDevice& device, std::shared_ptr<CookCache> cache = nullptr);
    ~FileAssetProvider() override;
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override;
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& path) override;
    AssetLoadResult<TextureAsset> load_texture(const std::filesystem::path& path) override;
    /// Decodes the source .hdr and cooks it (cook_environment) on every core.
    AssetLoadResult<EnvironmentAsset> load_environment(const std::filesystem::path& path) override;
    /// Parts of a glTF file. The last file opened stays open while it is unchanged on disk, so loading
    /// its meshes and textures one after another parses it once.
    AssetLoadResult<MeshAsset> load_imported_mesh(const std::filesystem::path& source, std::string_view part) override;
    AssetLoadResult<TextureAsset> load_imported_texture(const std::filesystem::path& source, std::string_view part) override;
private:
    struct OpenGltf;
    /// The open file, or why it cannot be opened.
    std::shared_ptr<const GltfFile> open_gltf(const std::filesystem::path& source, std::string& error);
    /// The digest of an imported file and every file its import file says it names (buffers, images),
    /// for cache keys; null without an import file, or when one of them cannot be read.
    std::optional<Sha256Digest> imported_digest(const std::filesystem::path& source);
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    std::shared_ptr<CookCache> m_cache;
    std::unique_ptr<OpenGltf> m_gltf;
};
/// Explicit fallback: missing meshes skip their draw; failed materials may use this value.
const MaterialAsset& fallback_material() noexcept;
/// The declared stand-in for a missing or failed texture: an 8x8 magenta and black checkerboard
/// (sRGB RGBA8, nearest filtering, repeating). It replaces the texture's pixels only; the problem is
/// still reported, and the missing reference keeps its ID.
inline constexpr uint32_t placeholder_texture_size = 8;
std::span<const std::byte> placeholder_texture_pixels() noexcept;
/// One placeholder for a device session, or null if the device cannot create it; its owner keeps it
/// as long as it may be drawn.
std::shared_ptr<const TextureAsset> make_placeholder_texture(GraphicsDevice& device);
} // namespace maya
