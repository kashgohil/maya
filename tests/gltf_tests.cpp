// Reading glTF 2.0 files (#1036, docs/import.md#gltf): what Maya takes from a file, what it warns about,
// what it refuses, and where. Small files are written here; the Khronos samples from
// tools/fetch_render_samples.sh are read when they are present.

#include "maya/assets/gltf.hpp"
#include "maya/assets/import_file.hpp"
#include "support/gltf.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace maya;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {
struct Folder {
    Folder() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("maya-gltf-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream(path / name, std::ios::binary) << text;
        return path / name;
    }
    fs::path path;
};
std::string problems(const std::vector<GltfProblem>& list) {
    auto text = std::string{};
    for (const auto& problem : list) text += gltf_problem_text(problem) + "\n";
    return text;
}
bool has(const std::vector<GltfProblem>& list, std::string_view path, std::string_view words) {
    return std::ranges::any_of(list, [&](const GltfProblem& p) { return p.path == path && p.message.find(words) != std::string::npos; });
}
GltfFile::OpenResult open_text(const Folder& folder, const std::string& json) { return GltfFile::open(folder.write("model/file.gltf", json)); }
void replace_once(std::string& text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
}
#define REQUIRE_OPEN(result) do { INFO(problems((result).errors)); REQUIRE((result)); } while (false)

/// A unit quad in the XY plane, facing +Z, as a triangle strip with 16-bit indices and no normals.
std::string strip_quad(test::GltfBuffer& buffer, const std::string& extra_attributes = "", const std::string& extra = "") {
    const auto positions = buffer.view(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0});
    const auto indices = buffer.view(std::vector<uint16_t>{0, 1, 2, 3});
    return R"({"asset":{"version":"2.0"},)" + buffer.json() + R"(,"accessors":[
        {"bufferView":)" + std::to_string(positions) + R"(,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
        {"bufferView":)" + std::to_string(indices) + R"(,"componentType":5123,"count":4,"type":"SCALAR"}],
        "meshes":[{"primitives":[{"mode":5,"indices":1,"attributes":{"POSITION":0)" + extra_attributes + R"(}}]}],
        "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0)" + extra + "}";
}
} // namespace

TEST_CASE("A triangle strip without normals reads as flat, counter-clockwise triangles with MikkTSpace tangents", "[assets][gltf]") {
    const Folder folder;
    auto buffer = test::GltfBuffer{};
    const auto opened = open_text(folder, strip_quad(buffer));
    REQUIRE_OPEN(opened);
    const auto& document = opened.file->document();
    REQUIRE(document.meshes.size() == 1);
    REQUIRE(document.meshes[0].primitives.size() == 1);
    CHECK(document.meshes[0].primitives[0].vertices == 4);
    CHECK(document.roots == std::vector<uint32_t>{0});
    const auto geometry = opened.file->primitive(0, 0);
    INFO(geometry.error);
    REQUIRE(geometry);
    REQUIRE(geometry.indices.size() == 6); // two triangles
    for (size_t t = 0; t < 6; t += 3) {
        const auto& a = geometry.vertices[geometry.indices[t]].position;
        const auto& b = geometry.vertices[geometry.indices[t + 1]].position;
        const auto& c = geometry.vertices[geometry.indices[t + 2]].position;
        CHECK(math::Vec3::cross(b - a, c - a).z > 0); // counter-clockwise seen from +Z, as glTF's strips are
    }
    for (const auto& vertex : geometry.vertices) {
        CHECK_THAT(vertex.normal.z, WithinAbs(1, 1e-6));
        CHECK_THAT(math::Vec3(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z).length(), WithinAbs(1, 1e-5));
        CHECK(std::abs(vertex.tangent.w) == 1);
        CHECK(vertex.color.w == 1);
    }
    CHECK(geometry.vertices.size() <= 6); // corners that agree are shared
    CHECK_FALSE(opened.file->primitive(0, 1));
    CHECK_FALSE(opened.file->primitive(1, 0));
}

TEST_CASE("Triangle fans, quantized positions, sparse accessors, and vertex colors are read", "[assets][gltf]") {
    const Folder folder;
    auto buffer = test::GltfBuffer{};
    // Normalized shorts (KHR_mesh_quantization), with vertex 2 replaced by a sparse accessor.
    const auto positions = buffer.view(std::vector<int16_t>{0, 0, 0, 0, 32767, 0, 0, 0, 0, 0, 0, 0, 0, 32767, 0, 0}, 8);
    const auto sparse_indices = buffer.view(std::vector<uint8_t>{2, 0, 0, 0});
    const auto sparse_values = buffer.view(std::vector<int16_t>{32767, 32767, 0, 0});
    const auto colors = buffer.view(std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});
    const auto json = R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_mesh_quantization"],"extensionsRequired":["KHR_mesh_quantization"],)" +
        buffer.json() + R"(,"accessors":[
        {"bufferView":)" + std::to_string(positions) + R"(,"componentType":5122,"normalized":true,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0],
         "sparse":{"count":1,"indices":{"bufferView":)" + std::to_string(sparse_indices) + R"(,"componentType":5121},
                   "values":{"bufferView":)" + std::to_string(sparse_values) + R"(}}},
        {"bufferView":)" + std::to_string(colors) + R"(,"componentType":5121,"normalized":true,"count":4,"type":"VEC4"}],
        "meshes":[{"primitives":[{"mode":6,"attributes":{"POSITION":0,"COLOR_0":1}}]}],"nodes":[{"mesh":0}]})";
    const auto opened = open_text(folder, json);
    REQUIRE_OPEN(opened);
    CHECK(opened.file->document().roots == std::vector<uint32_t>{0}); // no scene: the nodes without parents
    const auto geometry = opened.file->primitive(0, 0);
    INFO(geometry.error);
    REQUIRE(geometry);
    REQUIRE(geometry.indices.size() == 6); // a fan of four: two triangles around vertex 0
    auto found_corner = false;
    for (const auto& vertex : geometry.vertices) {
        CHECK_THAT(vertex.normal.z, WithinAbs(1, 1e-6));
        if (std::abs(vertex.position.x - 1) < 1e-4f && std::abs(vertex.position.y - 1) < 1e-4f) {
            found_corner = true; // the sparse value
            CHECK_THAT(vertex.color.z, WithinAbs(1, 1e-6));
            CHECK_THAT(vertex.color.x, WithinAbs(0, 1e-6));
        }
    }
    CHECK(found_corner);
}

TEST_CASE("Given normals and tangents are kept, and texture coordinates keep glTF's downward v", "[assets][gltf]") {
    const Folder folder;
    auto buffer = test::GltfBuffer{};
    const auto normals = buffer.view(std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
    const auto tangents = buffer.view(std::vector<float>{0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1});
    const auto uvs = buffer.view(std::vector<float>{0, 1, 1, 1, 0, 0, 1, 0});
    const auto attributes = R"(,"NORMAL":2,"TANGENT":3,"TEXCOORD_0":4)";
    auto json = strip_quad(buffer, attributes);
    const auto accessors = std::string(R"(,{"bufferView":)") + std::to_string(normals) + R"(,"componentType":5126,"count":4,"type":"VEC3"},
        {"bufferView":)" + std::to_string(tangents) + R"(,"componentType":5126,"count":4,"type":"VEC4"},
        {"bufferView":)" + std::to_string(uvs) + R"(,"componentType":5126,"count":4,"type":"VEC2"}])";
    replace_once(json, R"("type":"SCALAR"}])", R"("type":"SCALAR"})" + accessors);
    const auto opened = open_text(folder, json);
    REQUIRE_OPEN(opened);
    const auto geometry = opened.file->primitive(0, 0);
    INFO(geometry.error);
    REQUIRE(geometry);
    for (const auto& vertex : geometry.vertices) {
        CHECK(vertex.tangent.y == 1); // the file's, not MikkTSpace's (+X here)
        CHECK(vertex.tangent.w == -1);
        CHECK(vertex.uv.y == 1 - vertex.position.y); // the bottom of the quad is the bottom of the texture
    }
}

TEST_CASE("A required extension Maya does not read refuses the file by name; one only used is a warning", "[assets][gltf]") {
    const Folder folder;
    auto buffer = test::GltfBuffer{};
    const auto refused = open_text(folder, strip_quad(buffer, "",
        R"(,"extensionsUsed":["KHR_draco_mesh_compression","KHR_materials_clearcoat"],"extensionsRequired":["KHR_draco_mesh_compression"])"));
    REQUIRE_FALSE(refused);
    CHECK(has(refused.errors, "extensionsRequired[0]", "requires KHR_draco_mesh_compression, which Maya does not read"));
    auto again = test::GltfBuffer{};
    const auto warned = open_text(folder, strip_quad(again, "", R"(,"extensionsUsed":["KHR_materials_clearcoat","KHR_texture_transform"])"));
    REQUIRE_OPEN(warned);
    const auto& warnings = warned.file->document().warnings;
    CHECK(has(warnings, "extensionsUsed[0]", "KHR_materials_clearcoat is not read"));
    CHECK_FALSE(has(warnings, "extensionsUsed[1]", "")); // one Maya reads
}

TEST_CASE("Broken files are refused with what is wrong and where", "[assets][gltf]") {
    const Folder folder;
    CHECK(has(GltfFile::open(folder.path / "missing.gltf").errors, "", "missing"));
    CHECK(has(open_text(folder, R"({"asset":{"version":"2.0"}, "nodes": [oops]})").errors, "", "JSON is invalid"));
    CHECK(has(open_text(folder, R"({"asset":{"version":"1.0"}})").errors, "", "glTF 1.0"));
    CHECK(has(open_text(folder, R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4,"uri":"gone.bin"}]})").errors, "buffers[0]", "gone.bin"));
    // A normal with two components, and a texture-coordinate count that does not match the positions.
    auto buffer = test::GltfBuffer{};
    const auto bad = buffer.view(std::vector<float>{0, 0, 0, 0, 0, 0, 0, 0});
    auto json = strip_quad(buffer, R"(,"NORMAL":2,"TEXCOORD_0":3)");
    replace_once(json, R"("type":"SCALAR"}])", R"("type":"SCALAR"},{"bufferView":)" + std::to_string(bad) +
        R"(,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":)" + std::to_string(bad) + R"(,"componentType":5126,"count":3,"type":"VEC2"}])");
    const auto refused = open_text(folder, json);
    INFO(problems(refused.errors));
    REQUIRE_FALSE(refused);
    CHECK(has(refused.errors, "meshes[0].primitives[0].attributes.NORMAL", "2 components"));
    CHECK(has(refused.errors, "meshes[0].primitives[0].attributes.TEXCOORD_0", "3 elements, but POSITION has 4"));
}

TEST_CASE("Points and lines are left out with a warning, and a mesh's other primitives still read", "[assets][gltf]") {
    const Folder folder;
    auto buffer = test::GltfBuffer{};
    auto json = strip_quad(buffer);
    replace_once(json, R"(}}]}])", R"(}},{"mode":1,"attributes":{"POSITION":0}}]}])");
    const auto opened = open_text(folder, json);
    REQUIRE_OPEN(opened);
    const auto& primitives = opened.file->document().meshes[0].primitives;
    REQUIRE(primitives.size() == 2);
    CHECK(primitives[0].drawable);
    CHECK_FALSE(primitives[1].drawable);
    CHECK(has(opened.file->document().warnings, "meshes[0].primitives[1]", "points or lines"));
    CHECK(opened.file->primitive(0, 0));
    CHECK_FALSE(opened.file->primitive(0, 1));
}

TEST_CASE("Materials read every factor, texture slot, and texture transform; out-of-range factors keep defaults with a warning", "[assets][gltf]") {
    const Folder folder;
    auto buffer = test::GltfBuffer{};
    const auto json = strip_quad(buffer, "", R"(,"extensionsUsed":["KHR_texture_transform","KHR_materials_emissive_strength"],
        "samplers":[{"magFilter":9728,"minFilter":9984,"wrapS":33071,"wrapT":33648},{"minFilter":9729}],
        "images":[{"uri":"albedo%20map.png"},{"uri":"data:image/png;base64,AAAA"}],
        "textures":[{"source":0,"sampler":0},{"source":1,"sampler":1},{"source":0}],
        "materials":[{"name":"Painted","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,1,0.75],"metallicFactor":0.2,"roughnessFactor":2,
            "baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"offset":[0.5,0.25],"rotation":1.5,"scale":[2,3]}}},
            "metallicRoughnessTexture":{"index":2,"texCoord":1}},
          "normalTexture":{"index":1,"scale":0.5},"occlusionTexture":{"index":2,"strength":0.25},
          "emissiveTexture":{"index":0},"emissiveFactor":[1,0.5,0],"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4}},
          "alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true}])");
    const auto opened = open_text(folder, json);
    REQUIRE_OPEN(opened);
    const auto& document = opened.file->document();
    REQUIRE(document.materials.size() == 1);
    const auto& material = document.materials[0];
    CHECK(material.name == "Painted");
    const auto& factors = material.factors;
    CHECK(factors.base_color.y == 0.25f);
    CHECK(factors.base_color.w == 0.75f);
    CHECK(factors.metallic == 0.2f);
    CHECK(factors.roughness == 1); // 2 is out of range: the default, with a warning
    CHECK(has(document.warnings, "materials[0]", "roughness"));
    CHECK(factors.normal_scale == 0.5f);
    CHECK(factors.occlusion_strength == 0.25f);
    CHECK(factors.emissive.y == 0.5f);
    CHECK(factors.emissive_strength == 4);
    CHECK(factors.alpha_mode == AlphaMode::mask);
    CHECK(factors.alpha_cutoff == 0.3f);
    CHECK(factors.double_sided);
    const auto& base = material.maps[size_t(GltfMap::base_color)];
    REQUIRE(base);
    CHECK(base->texture == 0);
    CHECK(base->transformed);
    CHECK(base->offset.x == 0.5f);
    CHECK(base->scale.y == 3);
    CHECK(base->rotation == 1.5f);
    REQUIRE(material.maps[size_t(GltfMap::metallic_roughness)]);
    CHECK(material.maps[size_t(GltfMap::metallic_roughness)]->texcoord == 1);
    CHECK(has(document.warnings, "materials[0].pbrMetallicRoughness.metallicRoughnessTexture", "TEXCOORD_1"));
    CHECK(material.maps[size_t(GltfMap::normal)]->texture == 1);
    CHECK_FALSE(material.maps[size_t(GltfMap::normal)]->transformed);
    CHECK(material.maps[size_t(GltfMap::emissive)]);
    // Samplers: glTF's filters and wraps, and its defaults for a texture without one.
    REQUIRE(document.textures.size() == 3);
    const auto& first = document.textures[0].sampler;
    CHECK(first.mag_filter == Filter::nearest);
    CHECK(first.min_filter == Filter::nearest);
    CHECK(first.mip_filter == MipFilter::nearest);
    CHECK(first.address_u == AddressMode::clamp_to_edge);
    CHECK(first.address_v == AddressMode::mirror_repeat);
    CHECK(document.textures[1].sampler.mip_filter == MipFilter::none); // LINEAR: no mips
    CHECK(document.textures[2].sampler.mip_filter == MipFilter::linear);
    CHECK(document.textures[2].sampler.address_u == AddressMode::repeat);
    // Images: a relative file's URI is decoded; an embedded one has none.
    CHECK(document.images[0].uri == "albedo map.png");
    CHECK(document.images[1].uri.empty());
}

TEST_CASE("Images are read from data URIs and files beside the glTF file, never from outside its folder", "[assets][gltf]") {
    const Folder folder;
    folder.write("model/textures/albedo map.png", "PNGBYTES");
    folder.write("outside.png", "SECRET");
    auto buffer = test::GltfBuffer{};
    const auto opened = open_text(folder, strip_quad(buffer, "",
        R"(,"images":[{"uri":"textures/albedo%20map.png"},{"uri":"data:image/png;base64,)" + test::base64("hello!") +
        R"("},{"uri":"../outside.png"},{"uri":"missing.png"}])"));
    REQUIRE_OPEN(opened);
    const auto bytes = [](const GltfImageBytes& image) { return std::string(reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size()); };
    const auto file = opened.file->image(0);
    INFO(file.error);
    CHECK(bytes(file) == "PNGBYTES");
    CHECK(bytes(opened.file->image(1)) == "hello!");
    CHECK(opened.file->image(2).error == "images[2]: '../outside.png' is outside the glTF file's folder");
    CHECK(opened.file->image(3).error.find("missing") != std::string::npos);
    CHECK_FALSE(opened.file->image(4));
}

TEST_CASE("Nodes keep their hierarchy and transforms; a matrix is decomposed, and a mirror is dropped with a warning", "[assets][gltf]") {
    const Folder folder;
    const auto opened = open_text(folder, R"({"asset":{"version":"2.0"},
        "nodes":[{"name":"Root","children":[1,2],"translation":[1,2,3],"rotation":[0,0.7071068,0,0.7071068],"scale":[2,2,2]},
                 {"name":"Turned","matrix":[0,0,-3, 0, 0,2,0, 0, 1,0,0, 0, 4,5,6,1]},
                 {"name":"Mirror","scale":[-1,1,1]},
                 {"name":"Spare"}],
        "scenes":[{"nodes":[3]},{"nodes":[0]}],"scene":1})");
    REQUIRE_OPEN(opened);
    const auto& document = opened.file->document();
    REQUIRE(document.nodes.size() == 4);
    CHECK(document.roots == std::vector<uint32_t>{0}); // the default scene's
    CHECK(document.nodes[0].name == "Root");
    CHECK(document.nodes[0].children == std::vector<uint32_t>{1, 2});
    CHECK(document.nodes[1].parent == 0u);
    CHECK(document.nodes[0].transform.translation.z == 3);
    CHECK(document.nodes[0].transform.scale.x == 2);
    // The matrix: a quarter turn about +Y (x maps to -Z), scale (3, 2, 1), and a translation.
    const auto& turned = document.nodes[1].transform;
    CHECK_THAT(turned.scale.x, WithinAbs(3, 1e-6));
    CHECK_THAT(turned.scale.y, WithinAbs(2, 1e-6));
    CHECK_THAT(turned.scale.z, WithinAbs(1, 1e-6));
    CHECK_THAT(turned.rotation.y, WithinAbs(std::sqrt(0.5), 1e-6));
    CHECK_THAT(turned.rotation.w, WithinAbs(std::sqrt(0.5), 1e-6));
    CHECK(turned.translation.y == 5);
    const auto& mirror = document.nodes[2].transform;
    CHECK(mirror.scale.x == 1);
    CHECK(std::abs(mirror.rotation.w) > 0.9999f); // the mirror is dropped, not turned into a half turn
    CHECK(has(document.warnings, "nodes[2]", "mirrored"));
    CHECK_FALSE(has(document.warnings, "nodes[1]", ""));
}

TEST_CASE("Punctual lights convert to Maya's units and full cone angles; perspective cameras keep their lens", "[assets][gltf]") {
    const Folder folder;
    const auto opened = open_text(folder, R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_lights_punctual"],
        "extensions":{"KHR_lights_punctual":{"lights":[
            {"type":"directional","intensity":3,"color":[1,0.5,0.25]},
            {"type":"point","intensity":10,"range":7},
            {"type":"spot","intensity":100,"spot":{"innerConeAngle":0.2,"outerConeAngle":0.5}}]}},
        "cameras":[{"type":"perspective","perspective":{"yfov":0.8,"znear":0.05,"zfar":250}},
                   {"type":"perspective","perspective":{"yfov":0.6,"znear":0.5}},
                   {"type":"orthographic","orthographic":{"xmag":1,"ymag":1,"znear":0.1,"zfar":10}}],
        "nodes":[{"extensions":{"KHR_lights_punctual":{"light":2}}},{"camera":0},{"camera":2}]})");
    REQUIRE_OPEN(opened);
    const auto& document = opened.file->document();
    REQUIRE(document.lights.size() == 3);
    CHECK(document.lights[0].light.kind == LightKind::directional);
    CHECK(document.lights[0].light.intensity == 3); // lux
    CHECK(document.lights[0].light.color.y == 0.5f);
    CHECK(document.lights[1].light.kind == LightKind::point);
    CHECK_THAT(document.lights[1].light.intensity, WithinAbs(40 * math::PI, 1e-3)); // 10 cd over 4 pi steradians
    CHECK(document.lights[1].light.range == 7);
    const auto& spot = document.lights[2].light;
    CHECK(spot.kind == LightKind::spot);
    CHECK_THAT(spot.inner_cone, WithinAbs(0.4, 1e-6));
    CHECK_THAT(spot.outer_cone, WithinAbs(1.0, 1e-6));
    CHECK_THAT(spot.range, WithinAbs(100, 1e-3)); // unbounded: where 100 cd gives 0.01 lux
    CHECK(document.nodes[0].light == 2u);
    REQUIRE(document.cameras.size() == 3);
    CHECK(document.cameras[0].camera.vertical_fov == 0.8f);
    CHECK(document.cameras[0].camera.near_clip == 0.05f);
    CHECK(document.cameras[0].camera.far_clip == 250);
    CHECK(document.cameras[1].camera.far_clip >= 1000); // infinite: far enough
    CHECK(document.nodes[1].camera == 0u);
    CHECK_FALSE(document.nodes[2].camera); // orthographic: left out
    CHECK(has(document.warnings, "cameras[2]", "orthographic"));
}

TEST_CASE("Every Khronos sample Maya reads opens, with drawable geometry and decodable images", "[assets][gltf][samples]") {
    const auto models = fs::path(MAYA_RENDER_SAMPLES) / "Models";
    if (!fs::is_directory(models)) SKIP("no samples at " << models.string() << "; run tools/fetch_render_samples.sh");
    // One variant of each model, GLB where there is one, and one with embedded data URIs.
    auto files = std::vector<fs::path>{models / "BoxTextured/glTF-Embedded/BoxTextured.gltf"};
    for (const auto& model : fs::directory_iterator(models))
        for (const auto* variant : {"glTF-Binary", "glTF"})
            if (fs::is_directory(model.path() / variant)) {
                for (const auto& file : fs::directory_iterator(model.path() / variant))
                    if (file.path().extension() == ".gltf" || file.path().extension() == ".glb") files.push_back(file.path());
                break;
            }
    REQUIRE(files.size() >= 10);
    for (const auto& path : files) {
        INFO(path.string());
        const auto opened = GltfFile::open(path);
        REQUIRE_OPEN(opened);
        const auto& document = opened.file->document();
        for (uint32_t m = 0; m < document.meshes.size(); ++m)
            for (uint32_t p = 0; p < document.meshes[m].primitives.size(); ++p) {
                if (!document.meshes[m].primitives[p].drawable) continue;
                const auto geometry = opened.file->primitive(m, p);
                INFO(geometry.error);
                REQUIRE(geometry);
                CHECK(!geometry.indices.empty());
                // Unit normals and tangents everywhere: the shader normalizes them.
                const auto bad = std::ranges::count_if(geometry.vertices, [](const Vertex& vertex) {
                    return std::abs(vertex.normal.length() - 1) > 1e-3f ||
                           std::abs(math::Vec3(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z).length() - 1) > 1e-2f;
                });
                CHECK(bad == 0);
            }
        for (uint32_t i = 0; i < document.images.size(); ++i) {
            const auto image = opened.file->image(i);
            INFO(image.error);
            REQUIRE(image);
            // PNG or JPEG, by signature: decoding is the texture tests' work, and slow for these.
            REQUIRE(image.bytes.size() > 8);
            const auto first = uint8_t(image.bytes[0]), second = uint8_t(image.bytes[1]);
            CHECK(((first == 0x89 && second == 'P') || (first == 0xff && second == 0xd8)));
        }
    }
}

TEST_CASE("The DamagedHelmet sample reads as one mesh with one fully textured material", "[assets][gltf][samples]") {
    const auto file = fs::path(MAYA_RENDER_SAMPLES) / "Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb";
    if (!fs::exists(file)) SKIP("no DamagedHelmet sample; run tools/fetch_render_samples.sh");
    const auto opened = GltfFile::open(file);
    REQUIRE_OPEN(opened);
    const auto& document = opened.file->document();
    CHECK(document.warnings.empty());
    REQUIRE(document.meshes.size() == 1);
    REQUIRE(document.materials.size() == 1);
    for (const auto& map : document.materials[0].maps) CHECK(map);
    CHECK(document.textures.size() == 5);
    const auto geometry = opened.file->primitive(0, 0);
    REQUIRE(geometry);
    CHECK(geometry.indices.size() == 46356); // 15452 triangles
}

// Damaged files: a run of mutated copies of small glTF files must be refused or read without crashing
// or reading out of bounds. Hidden from normal runs; run it under Guard Malloc (docs/import.md#tests):
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib maya_asset_tests "[fuzz]"
TEST_CASE("Damaged glTF and import files are refused or read safely", "[.fuzz]") {
    const Folder folder;
    auto sources = std::vector<std::pair<std::string, std::string>>{{"props.gltf", test::props_gltf()}};
    folder.write("model/textures/normal.png", test::flat_normal_png());
    const auto glb = fs::path(MAYA_RENDER_SAMPLES) / "Models/BoxTextured/glTF-Binary/BoxTextured.glb";
    if (auto input = std::ifstream(glb, std::ios::binary)) sources.emplace_back("box.glb", std::string(std::istreambuf_iterator<char>(input), {}));
    auto state = uint64_t{1036};
    const auto next = [&] { // SplitMix64: the same damage on every run
        state += 0x9e3779b97f4a7c15ull;
        auto z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    };
    size_t opened = 0, refused = 0;
    for (const auto& [name, original] : sources)
        for (int round = 0; round < 1500; ++round) {
            auto damaged = original;
            const auto edits = 1 + next() % 8;
            for (uint64_t i = 0; i < edits; ++i) {
                const auto at = next() % damaged.size();
                switch (next() % 3) {
                case 0: damaged[at] = char(next()); break; // a changed byte
                case 1: damaged.erase(at, 1 + next() % 16); break; // bytes cut out
                default: damaged[at] = "0123456789-,:[]{}\""[next() % 18]; break; // JSON punctuation
                }
                if (damaged.empty()) damaged = "{";
            }
            const auto path = folder.write("model/" + name, damaged);
            const auto file = GltfFile::open(path);
            if (!file) {
                ++refused;
                continue;
            }
            ++opened;
            const auto& document = file.file->document();
            for (uint32_t m = 0; m < document.meshes.size(); ++m)
                for (uint32_t p = 0; p < document.meshes[m].primitives.size(); ++p) (void)file.file->primitive(m, p);
            for (uint32_t i = 0; i < document.images.size(); ++i) (void)file.file->image(i);
        }
    // Import files, damaged the same way.
    const auto import = std::string("maya-import 1\ncompression astc\nmips on\nscale 1\nup y\nlights on\ncameras on\n"
                                    "scene \"props.scene\" 51ac09e2d3b4f607\nmesh 6d617961 1a2b \"mesh/0/0\" \"Panel/0\"\n"
                                    "material 6d617961 1a2d \"props/materials/Painted.material\" \"Painted\" 9f3c2b1a00ffe1d2\n"
                                    "entity abc def \"/Root/Panel\"\nfile \"textures/normal.png\"\n");
    for (int round = 0; round < 3000; ++round) {
        auto damaged = import;
        for (uint64_t i = 0, edits = 1 + next() % 4; i < edits && !damaged.empty(); ++i) damaged[next() % damaged.size()] = char(next());
        auto input = std::istringstream(damaged);
        (void)read_import_file(input);
    }
    INFO(opened << " opened, " << refused << " refused");
    CHECK(refused > 0);
    CHECK(opened > 0);
}
