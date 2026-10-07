#pragma once
// glTF 2.0 files (#1036, docs/import.md): read with cgltf, as #1030 chose, and turned into Maya's data.
// cgltf's types stay inside gltf.cpp. Problems name where they are with JSON paths, such as
// "meshes[0].primitives[1].attributes.NORMAL", since cgltf's own errors name only a kind.

#include "maya/assets/animation.hpp"
#include "maya/assets/material.hpp"
#include "maya/rhi/resource.hpp"
#include "maya/rhi/vertex.hpp"
#include "maya/world/components.hpp"
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace maya {
/// Something in a glTF file, by its JSON path, and what is wrong with it.
struct GltfProblem {
    std::string path; // e.g. "materials[2].normalTexture"; empty for the whole file
    std::string message;
};
/// The extensions Maya reads. A file that requires any other is refused with its name.
inline constexpr std::array<const char*, 4> gltf_supported_extensions = {
    "KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_lights_punctual", "KHR_mesh_quantization"};

/// A material's use of a texture.
struct GltfTextureUse {
    uint32_t texture = 0; // into GltfDocument::textures
    uint32_t texcoord = 0; // only set 0 is read
    bool transformed = false; // KHR_texture_transform
    math::Vec2 offset{0.0f}, scale{1.0f};
    float rotation = 0.0f;
};
/// The material slots, in MaterialAsset's order.
enum class GltfMap : uint8_t { base_color, metallic_roughness, normal, occlusion, emissive };
struct GltfMaterial {
    std::string name;
    MaterialAsset factors; // every factor and mode; its texture references are empty
    std::array<std::optional<GltfTextureUse>, 5> maps; // by GltfMap
};
struct GltfTexture {
    std::string name;
    std::optional<uint32_t> image; // into GltfDocument::images
    SamplerDesc sampler; // from the glTF sampler; glTF's defaults when it has none
};
struct GltfImage {
    std::string name;
    std::string uri; // a relative file, or empty when embedded (in a buffer view or a data URI)
    std::string mime_type;
};
struct GltfPrimitive {
    std::optional<uint32_t> material; // into GltfDocument::materials
    size_t vertices = 0; // as stored, before triangles are expanded
    bool drawable = true; // false for points, lines, and primitives without positions, which Maya does not draw (a warning says so)
};
struct GltfMesh {
    std::string name;
    std::vector<GltfPrimitive> primitives;
};
struct GltfNode {
    std::string name;
    std::optional<uint32_t> parent;
    std::vector<uint32_t> children;
    TransformComponent transform; // local, with positive scale: a mirroring node loses its mirror, with a warning
    std::optional<uint32_t> mesh, camera, light;
    std::optional<uint32_t> skin; // into GltfDocument::skins: its mesh is skinned
};
/// A skin: the nodes its joints are, and their inverse bind matrices (identity where the file gives none).
struct GltfSkin {
    std::string name;
    std::vector<uint32_t> joints; // into GltfDocument::nodes
    std::vector<math::Mat4> inverse_bind;
};
/// An animation channel: a node's translation, rotation, or scale over time.
struct GltfChannel {
    uint32_t node = 0;
    ChannelPath path = ChannelPath::translation;
    Interpolation interpolation = Interpolation::linear;
    std::vector<float> times;
    std::vector<float> values;
};
struct GltfAnimation {
    std::string name;
    float duration = 0; // the last key of any channel
    std::vector<GltfChannel> channels;
};
struct GltfCamera {
    std::string name;
    CameraComponent camera;
};
struct GltfLight {
    std::string name;
    LightComponent light; // KHR_lights_punctual in Maya's units (docs/import.md#lights)
};
/// What a glTF file holds, converted. Warnings are things imported only approximately.
struct GltfDocument {
    std::vector<GltfNode> nodes;
    std::vector<uint32_t> roots; // the default scene's (or else the first scene's) root nodes, in order
    std::vector<GltfMesh> meshes;
    std::vector<GltfMaterial> materials;
    std::vector<GltfTexture> textures;
    std::vector<GltfImage> images;
    std::vector<GltfCamera> cameras;
    std::vector<GltfLight> lights;
    std::vector<GltfSkin> skins;
    std::vector<GltfAnimation> animations;
    /// The files beside it that the file names: its buffers' and images' relative URIs, decoded, each once.
    std::vector<std::string> files;
    std::vector<GltfProblem> warnings;
};

struct GltfGeometry {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangles
    std::vector<SkinVertex> skin; // per vertex, when the primitive has JOINTS_0 and WEIGHTS_0; empty otherwise
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};
struct GltfImageBytes {
    std::vector<std::byte> bytes; // the encoded image (PNG or JPEG)
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

/// An open glTF file (`.gltf` with its buffers, or `.glb`): parsed, its buffers loaded, and validated.
/// It keeps the file's data, so geometry and images can be read from it without parsing it again.
class GltfFile {
public:
    struct OpenResult;
    /// Opens and converts a file. Fails, with every problem found, for invalid glTF, glTF 1.0, a
    /// required extension Maya does not read, or data that cannot be converted.
    static OpenResult open(const std::filesystem::path& file);
    ~GltfFile();
    GltfFile(const GltfFile&) = delete;
    GltfFile& operator=(const GltfFile&) = delete;

    const GltfDocument& document() const noexcept { return m_document; }
    /// A primitive's triangles as Maya vertices: positions, normals (flat where the file has none),
    /// texture set 0, color 0, and tangents (the file's, or MikkTSpace's), and joints and weights (set 0,
    /// weights scaled to sum to 1) where it is skinned, with identical corners shared.
    GltfGeometry primitive(uint32_t mesh, uint32_t primitive) const;
    /// An image's encoded bytes, from a buffer view, a data URI, or a file beside the glTF file.
    GltfImageBytes image(uint32_t image) const;

private:
    struct Data;
    explicit GltfFile(std::unique_ptr<Data> data, GltfDocument document);
    std::unique_ptr<Data> m_data;
    GltfDocument m_document;
};
struct GltfFile::OpenResult {
    std::unique_ptr<GltfFile> file;
    std::vector<GltfProblem> errors;
    explicit operator bool() const noexcept { return file != nullptr; }
};
/// Each node's name as its imported entity carries it (docs/import.md#scenes): the node's name, else its
/// mesh's, else "Node <i>", with a number after any name a sibling already has ("Hand 2").
std::vector<std::string> gltf_node_names(const GltfDocument& document);
/// Each node's path of those names from a root of the file's scene ("Armature/Hips/Spine"), as skins and
/// clips name joints (docs/animation.md#binding); empty for a node outside the scene.
std::vector<std::string> gltf_node_paths(const GltfDocument& document);

/// "meshes[0].primitives[1]: message", or the message alone for the whole file.
std::string gltf_problem_text(const GltfProblem& problem);
} // namespace maya
