#pragma once
// Import files (#1036, docs/import.md#import-files): `<source>.import`, beside an imported glTF file. It
// holds the import's settings, and the IDs given to what the import made, by what each was made from,
// so reimporting keeps them.

#include "maya/assets/texture_data.hpp"
#include "maya/core/identity.hpp"
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace maya {
inline constexpr uint32_t import_format_version = 1;

/// Which way is up in the source. glTF says +Y, but some exporters write +Z.
enum class ImportUp : uint8_t { y, z };
inline constexpr float min_import_scale = 1e-6f, max_import_scale = 1e6f;
/// An import's settings. The scale and axis conversion apply to the import's root entity, so the whole
/// hierarchy follows; meshes are cooked as the file has them. Each texture's role comes from the
/// material slots that use it, and its sampler from the glTF file.
struct ImportSettings {
    TextureCompression compression = TextureCompression::astc;
    bool mips = true;
    float scale = 1; // metres per unit of the file
    ImportUp up = ImportUp::y; // z: turned so the file's +Z is up (+Y), as glTF's exporters convert
    bool lights = true, cameras = true; // import KHR_lights_punctual lights and cameras
};
/// A mesh, texture, skin, or clip inside the source: its catalog part (after '#') and what it was made from.
struct ImportedAsset {
    AssetId id;
    std::string part; // "mesh/0/1", "texture/2/color", "skin/0", or "animation/1"
    std::string identity; // e.g. "Helmet/1" or "Albedo/color": names where the file has them, else indices
};
/// A material file the import wrote, and a hash of the text it wrote: a file whose text no longer
/// has that hash was edited, and reimporting keeps it.
struct ImportedMaterial {
    AssetId id;
    std::filesystem::path file; // relative to the import file's folder
    std::string identity;
    uint64_t written = 0; // import_text_hash of the text written
};
/// An entity of the import's scene, by its glTF node path ("/Root/Child").
struct ImportedEntity {
    EntityId id;
    std::string identity;
};
struct ImportFile {
    ImportSettings settings;
    /// The import's scene file, relative to the import file's folder, and a hash of the text written:
    /// an edited scene, like an edited material, is kept.
    std::filesystem::path scene;
    uint64_t scene_written = 0;
    std::vector<ImportedAsset> meshes, textures;
    std::vector<ImportedAsset> skins, animations; // #1038
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedEntity> entities;
    /// The files beside the source that it names (buffers, images), relative to the import file's
    /// folder: a change to one is a change to the source.
    std::vector<std::filesystem::path> files;
};
struct ImportFileResult {
    ImportFile file;
    std::string error; // "line N: ..." when the file is invalid
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Reads `maya-import 1`:
///
///     maya-import 1
///     compression astc
///     mips on
///     scale 1
///     up y
///     lights on
///     cameras on
///     scene "helmet.scene" 51ac09e2d3b4f607
///     mesh 6d617961 1a2b "mesh/0/0" "Helmet/0"
///     texture 6d617961 1a2c "texture/2/color" "Albedo/color"
///     material 6d617961 1a2d "helmet/materials/Metal.material" "Metal" 9f3c2b1a00ffe1d2
///     skin 6d617961 1a2f "skin/0" "Armature"
///     animation 6d617961 1a30 "animation/0" "Walk"
///     entity 6d617961 1a2e "/Helmet"
///     file "textures/normal.png"
///
/// Settings and the scene come first, each at most once; IDs are nonzero and unique within each kind.
ImportFileResult read_import_file(std::istream& input);
std::string write_import_file(const ImportFile& file);
/// `models/helmet.glb` -> `models/helmet.glb.import`.
std::filesystem::path import_file_path(const std::filesystem::path& source);
/// 64-bit FNV-1a, for ImportedMaterial::written.
uint64_t import_text_hash(std::string_view text) noexcept;
} // namespace maya
