#pragma once
// Cooking without a device (#1039, docs/assets.md#cooked-content): what loading makes of a source before
// it reaches the GPU. FileAssetProvider cooks and then uploads; packaging cooks and writes the results
// into a package, whose player reads only them (PackageAssetProvider). Both cook alike, through the same
// cook cache, so a package holds exactly what the editor draws.

#include "maya/assets/animation.hpp"
#include "maya/assets/asset.hpp"
#include "maya/assets/environment_cook.hpp"
#include "maya/assets/texture_data.hpp"
#include "maya/core/sha256.hpp"
#include "maya/rhi/vertex.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace maya {
class CookCache;
class GltfFile;
struct CookKey;

/// What the device samples, which changes how textures cook: ASTC or RGBA8, and the largest dimension.
struct CookLimits {
    bool astc = true;
    uint32_t max_texture_dimension = 16384;
};

/// A mesh's welded vertices (with tangents) and triangle indices, and for a skinned mesh each vertex's
/// joints and weights.
struct CookedMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SkinVertex> skin; // empty, or one per vertex
};
/// A texture's mip chain in its GPU format, how it is sampled, and its role.
struct CookedTexture {
    TextureImage image;
    SamplerDesc sampler;
    TextureRole role = TextureRole::color;
};
template<class T> struct CookResult {
    std::optional<T> value;
    AssetDiagnostic diagnostic;
    explicit operator bool() const noexcept { return value.has_value(); }
};

class AssetCooker {
public:
    /// With a cache, cooked results are read from it when there and written to it when not.
    explicit AssetCooker(CookLimits limits = {}, std::shared_ptr<CookCache> cache = nullptr);
    ~AssetCooker();
    AssetCooker(const AssetCooker&) = delete;
    AssetCooker& operator=(const AssetCooker&) = delete;

    const CookLimits& limits() const noexcept { return m_limits; }
    void set_limits(const CookLimits& limits) noexcept { m_limits = limits; }
    /// The job tier texture compression and environment prefiltering fan out on (docs/jobs.md#cooking):
    /// frame while a caller waits, background for asynchronous loads.
    void set_tier(JobTier tier) noexcept { m_tier = tier; }
    /// A triangulated .obj file.
    CookResult<CookedMesh> mesh(const std::filesystem::path& path);
    /// A texture descriptor (.texture) and the PNG, JPEG, or cooked KTX2 image it names.
    CookResult<CookedTexture> texture(const std::filesystem::path& path);
    /// An environment file (.environment) and the Radiance HDR image it names.
    CookResult<CookedEnvironment> environment(const std::filesystem::path& path);
    /// Parts of an imported glTF file: "mesh/<mesh>/<primitive>" and "texture/<texture>/<role>". The
    /// last file opened stays open while it is unchanged on disk, so its parts parse it once.
    CookResult<CookedMesh> imported_mesh(const std::filesystem::path& source, std::string_view part);
    CookResult<CookedTexture> imported_texture(const std::filesystem::path& source, std::string_view part);
    /// "skin/<skin>" and "animation/<animation>" (#1038): joints and clips named by node path.
    CookResult<SkinAsset> imported_skin(const std::filesystem::path& source, std::string_view part);
    CookResult<AnimationAsset> imported_animation(const std::filesystem::path& source, std::string_view part);
    /// The key loading this source reads from the cook cache (`path` is the descriptor or the imported
    /// file, `part` what follows '#'), or nothing when it cooks nothing (an .obj, a cooked KTX2, a
    /// material, a script) or cannot be read: what pruning keeps (#1063, docs/assets.md#cook-cache).
    /// Needs a cache, for the sources' digests.
    std::optional<CookKey> cook_key(AssetKind kind, const std::filesystem::path& path, std::string_view part);

private:
    struct OpenGltf;
    std::shared_ptr<const GltfFile> open_gltf(const std::filesystem::path& source, std::string& error);
    std::optional<Sha256Digest> imported_digest(const std::filesystem::path& source);
    CookLimits m_limits;
    JobTier m_tier = JobTier::frame;
    std::shared_ptr<CookCache> m_cache;
    std::unique_ptr<OpenGltf> m_gltf;
};

/// What a device samples, for cooking what it will draw.
CookLimits cook_limits(const GraphicsDevice& device) noexcept;
/// Uploads cooked content to the device: the last step of loading, from sources or from a package.
AssetLoadResult<MeshAsset> upload_mesh(GraphicsDevice& device, const CookedMesh& mesh);
AssetLoadResult<TextureAsset> upload_texture(GraphicsDevice& device, const CookedTexture& texture, const std::string& label);
AssetLoadResult<EnvironmentAsset> upload_environment(GraphicsDevice& device, const CookedEnvironment& environment, const std::string& label);

/// The cooked forms as bytes, as packages store them (inside wrap_cooked's checked envelope): each
/// read gives back exactly what was written, or nothing when the bytes are not one.
std::vector<std::byte> write_cooked_mesh(const CookedMesh& mesh);
std::optional<CookedMesh> read_cooked_mesh(std::span<const std::byte> bytes);
std::vector<std::byte> write_cooked_texture(const CookedTexture& texture);
std::optional<CookedTexture> read_cooked_texture(std::span<const std::byte> bytes);

} // namespace maya
