#pragma once
// Importing glTF files into a project (#1036, docs/import.md). An import adds the file's meshes and
// textures to the catalog as parts of the file, writes an editable material file per glTF material, and
// writes a scene file holding the file's nodes. It records what it made, and the IDs it gave, in the
// import file beside the source, so importing again keeps every ID.

#include "maya/assets/gltf.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/project.hpp"
#include <filesystem>
#include <vector>

namespace maya {
/// Something an import made that the previous import of the same file did or did not.
struct GltfImportChange {
    std::string what; // "mesh", "texture", "material", or "entity"
    std::string identity; // e.g. "Panel/1", or an entity's node path "/Root/Panel"
    std::string previous; // renamed: the identity it had, whose ID it keeps
};
struct GltfImportResult {
    std::filesystem::path scene; // content-relative: the import's scene file
    std::vector<AssetRecord> records; // the import's catalog entries: meshes, textures, materials, skins, then clips
    size_t entities = 0; // in the scene, its root included
    std::vector<std::filesystem::path> written; // content-relative files written, the catalog last
    std::vector<std::filesystem::path> kept; // edited material and scene files left as they were
    /// Changes since the previous import, empty on the first. A removed mesh or texture leaves the
    /// catalog, so a scene still using it reports it missing; a removed material's file and entry stay.
    std::vector<GltfImportChange> added, removed, renamed;
    std::vector<GltfProblem> warnings; // what was imported only approximately
    std::vector<GltfProblem> errors; // why nothing was imported; the catalog is then unchanged
    explicit operator bool() const noexcept { return errors.empty(); }
};

/// Imports a .gltf or .glb file inside the project's content root (a content-relative or absolute path).
/// Beside `models/helmet.glb` it writes `models/helmet.scene`, `models/helmet/materials/*.material`, and
/// `models/helmet.glb.import`, then the project's catalog. Skins and clips are catalog parts of the source;
/// skinned meshes' entities get a maya.skin, and the root a maya.animation playing the first clip. Importing a file again reuses the IDs in its
/// import file; a material or scene file edited since the import wrote it is kept, and listed in `kept`.
/// Nothing is loaded: meshes and textures are read from the source when the registry loads them.
GltfImportResult import_gltf(const Project& project, const std::filesystem::path& source);
} // namespace maya
