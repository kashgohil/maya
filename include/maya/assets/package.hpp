#pragma once
// Packages (#1039, docs/projects.md#packages): a project's content as a standalone player ships it. A
// package is a macOS application bundle whose Contents/Resources holds the shaders, a manifest, and the
// project with only cooked content: meshes, textures, and environments as the cooker made them,
// scenes, materials, and scripts as authored. Its player reads only these (PackageAssetProvider) and
// finds nothing outside the bundle (FileSystem).

#include "maya/assets/asset.hpp"
#include "maya/assets/cook_cache.hpp" // wrap_cooked, unwrap_cooked: a package's cooked files
#include "maya/core/sha256.hpp"
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace maya {
class GraphicsDevice;

/// Contents/Resources/package.maya
inline constexpr const char* package_manifest_name = "package.maya";
/// The cooked files' extensions, which only packages hold.
inline constexpr const char* cooked_mesh_extension = ".cooked-mesh";
inline constexpr const char* cooked_texture_extension = ".cooked-texture";
inline constexpr const char* cooked_environment_extension = ".cooked-environment";

struct PackageFile {
    std::filesystem::path path; // relative to Contents/Resources, generic form
    uint64_t size = 0;
    Sha256Digest digest{};
};
/// What a package holds and what made it: its files, sorted by path, and one digest over all of them
/// (a file's path, size, and digest per line), so equal content gives equal packages.
struct PackageManifest {
    std::string name; // the application's name
    std::string build; // the build that packaged it: revision, type, and compiler
    std::filesystem::path project = "project/project.maya"; // relative to Contents/Resources
    std::vector<std::filesystem::path> scenes; // relative to the project's content root; the first is the startup scene
    std::vector<PackageFile> files;
    Sha256Digest content{}; // over `files`
};
struct PackageManifestResult {
    PackageManifest manifest;
    std::string error; // empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};
/// The text form ("maya-package 1"): name, build, project, scene lines, file lines, then the content digest.
void write_package_manifest(std::ostream& output, const PackageManifest& manifest);
PackageManifestResult read_package_manifest(std::istream& input);
/// The digest of a list of files, as `content` records it.
Sha256Digest package_content_digest(const std::vector<PackageFile>& files);
/// Checks every file a package's manifest names against its size and digest, and that it names all of
/// the package's files but the manifest itself: empty when they agree, else the first difference.
std::string verify_package(const std::filesystem::path& resources);

/// Loads a package's content: cooked meshes, textures, and environments, and materials and scripts.
/// Anything else, a source image or model included, is refused: a package never cooks.
class PackageAssetProvider final : public AssetProvider {
public:
    explicit PackageAssetProvider(GraphicsDevice& device);
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override;
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& path) override;
    AssetLoadResult<TextureAsset> load_texture(const std::filesystem::path& path) override;
    AssetLoadResult<EnvironmentAsset> load_environment(const std::filesystem::path& path) override;
private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
};

} // namespace maya
