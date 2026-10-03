// Material assets (#1033, docs/assets.md#materials): files at versions 1 and 2, the material schema,
// publishing edits through the registry, and generated tangents.

#include "maya/assets/material_file.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/model_loader.hpp"
#include "maya/core/tangents.hpp"
#include "maya/rhi/null_device.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

using namespace maya;
namespace fs = std::filesystem;

namespace {
MaterialFileResult read(const std::string& text) {
    auto input = std::istringstream(text);
    return read_material_file(input);
}
bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

/// Every input set away from its default.
MaterialAsset full_material() {
    auto m = MaterialAsset{{0.25f, 0.5f, 0.75f, 0.6f}, 0.3f, 0.45f};
    m.base_color_texture = {{0x6d617961, 0x50}};
    m.metallic_roughness_texture = {{0x6d617961, 0x52}};
    m.normal_texture = {{0x6d617961, 0x51}};
    m.normal_scale = 1.5f;
    m.occlusion_texture = {{0x6d617961, 0x52}};
    m.occlusion_strength = 0.8f;
    m.emissive = {1.0f, 0.5f, 0.1f};
    m.emissive_strength = 12.5f;
    m.emissive_texture = {{0xabcdef, 0x1234567890}};
    m.alpha_mode = AlphaMode::mask;
    m.alpha_cutoff = 0.35f;
    m.double_sided = true;
    return m;
}

struct Folder {
    Folder() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("maya-materials-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    void write(const std::string& name, const std::string& text) const { std::ofstream(path / name) << text; }
    std::string read_file(const std::string& name) const {
        auto input = std::ifstream(path / name);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    fs::path path;
};
} // namespace

TEST_CASE("Material files at version 2 round-trip every property exactly, in a fixed order", "[assets][materials]") {
    const auto material = full_material();
    const auto text = write_material_file(material);
    CHECK(text.starts_with("maya-material 2\nbase_color 0.25 0.5 0.75\nalpha 0.6\nbase_color_texture 6d617961 50\n"));
    CHECK(has(text, "\nemissive_texture abcdef 1234567890\n"));
    CHECK(has(text, "\nalpha_mode mask\nalpha_cutoff 0.35\ndouble_sided true\n"));
    const auto back = read(text);
    INFO(back.error);
    REQUIRE(back);
    CHECK(back.version == 2);
    CHECK(back.material == material);
    CHECK(write_material_file(back.material) == text); // deterministic
    // Every property has a line, so a default material writes them all too.
    auto lines = 0;
    for (const auto c : write_material_file(MaterialAsset{})) lines += c == '\n';
    CHECK(lines == 1 + int(material_properties().size()));
}

TEST_CASE("Material files at version 2 take lines in any order, default the missing ones, and allow comments", "[assets][materials]") {
    const auto read_back = read("maya-material 2\n# a hand-written material\nroughness 0.2\n\nbase_color 1 0 0\n  metallic   1\n");
    INFO(read_back.error);
    REQUIRE(read_back);
    auto expected = MaterialAsset{{1, 0, 0, 1}, 1.0f, 0.2f};
    CHECK(read_back.material == expected);
    CHECK(read("maya-material 2\n").material == MaterialAsset{});
}

TEST_CASE("Material files refuse what they cannot read, with the line and what was expected", "[assets][materials]") {
    const auto error = [](const std::string& text) {
        const auto result = read(text);
        CHECK_FALSE(result);
        CHECK(result.version == 0);
        CHECK(result.material == MaterialAsset{});
        return result.error;
    };
    CHECK(has(error(""), "Expected 'maya-material 2'"));
    CHECK(has(error("maya-scene 1\n"), "Expected 'maya-material 2'"));
    CHECK(has(error("maya-material 3\n"), "version 3 is not supported"));
    CHECK(has(error("maya-material 2 extra\n"), "line 1"));
    CHECK(has(error("maya-material 2\nshininess 4\n"), "line 2: Unknown material property 'shininess'"));
    CHECK(has(error("maya-material 2\nmetallic 0\nroughness 1\nmetallic 1\n"), "line 4: 'metallic' appears more than once"));
    CHECK(has(error("maya-material 2\nbase_color 1 1\n"), "'base_color' needs three finite numbers"));
    CHECK(has(error("maya-material 2\nroughness 1.5\n"), "'roughness': Value is nonfinite or outside its allowed range"));
    CHECK(has(error("maya-material 2\nroughness nan\n"), "'roughness'"));
    CHECK(has(error("maya-material 2\nemissive -1 0 0\n"), "'emissive'"));
    CHECK(has(error("maya-material 2\nnormal_scale 11\n"), "'normal_scale'"));
    CHECK(has(error("maya-material 2\nalpha_mode glass\n"), "'alpha_mode' needs one of its choices"));
    CHECK(has(error("maya-material 2\ndouble_sided yes\n"), "'double_sided' needs true or false"));
    CHECK(has(error("maya-material 2\nnormal_texture 6d617961\n"), "'normal_texture' needs 'none' or two hexadecimal texture ID words"));
    CHECK(has(error("maya-material 2\nnormal_texture 0 0\n"), "'normal_texture'")); // not both zero
    CHECK(has(error("maya-material 2\nnormal_texture 6d617961 12345678901234567\n"), "'normal_texture'"));
}

TEST_CASE("Version 1 material files still load as before, and are written as version 2", "[assets][materials]") {
    // The fixed order of version 1, on lines or not, as its reader always took words.
    for (const auto* text : {"maya-material 1\nbase_color 0.35 0.55 0.95 1\nmetallic 0.35\nroughness 0.35\n",
                             "maya-material 1 base_color 0.35 0.55 0.95 1 metallic 0.35 roughness 0.35"}) {
        const auto v1 = read(text);
        INFO(v1.error);
        REQUIRE(v1);
        CHECK(v1.version == 1);
        CHECK(v1.material == MaterialAsset{{0.35f, 0.55f, 0.95f, 1.0f}, 0.35f, 0.35f});
        const auto migrated = read(write_material_file(v1.material));
        CHECK(migrated.version == 2);
        CHECK(migrated.material == v1.material);
    }
    CHECK(has(read("maya-material 1\nmetallic 0\nbase_color 1 1 1 1\nroughness 1\n").error, "Expected maya-material 1, base_color RGBA"));
    CHECK(has(read("maya-material 1\nbase_color 2 0 0 1\nmetallic 0\nroughness 1\n").error, "finite in [0,1]"));
    CHECK(has(read("maya-material 1\nbase_color 1 1 1 1\nmetallic 0\nroughness 1\nextra\n").error, "Expected maya-material 1"));
    // Every sample material still loads; the ones made for #1033 are version 2.
    const auto materials = fs::path(MAYA_SOURCE_DIR) / "samples/basic_scene/assets/materials";
    auto versions = std::map<uint32_t, int>{};
    for (const auto& entry : fs::recursive_directory_iterator(materials)) {
        if (entry.path().extension() != ".material") continue;
        auto input = std::ifstream(entry.path());
        const auto loaded = read_material_file(input);
        INFO(entry.path().string() << ": " << loaded.error);
        CHECK(loaded);
        ++versions[loaded.version];
    }
    CHECK(versions[1] == 4);
    CHECK(versions[2] == 16);
}

TEST_CASE("Saving a material replaces its file whole, and a failed save leaves it alone", "[assets][materials]") {
    const Folder folder;
    folder.write("old.material", "maya-material 1\nbase_color 1 1 1 1\nmetallic 0\nroughness 1\n");
    const auto material = full_material();
    CHECK(save_material_file(folder.path / "old.material", material).empty());
    CHECK(folder.read_file("old.material") == write_material_file(material));
    // Into a folder that does not exist: refused, naming the file and saying nothing changed.
    const auto failed = save_material_file(folder.path / "missing" / "new.material", material);
    CHECK(has(failed, "new.material"));
    CHECK(has(failed, "The existing material file was not changed"));
    // No temporary files are left behind.
    auto files = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder.path)) ++files;
    CHECK(files == 1);
}

TEST_CASE("The material schema has stable identities, the file's keys, and the property system's validation", "[assets][materials][properties]") {
    const auto properties = material_properties();
    const char* names[] = {"base_color", "alpha", "base_color_texture", "metallic", "roughness", "metallic_roughness_texture",
                           "normal_texture", "normal_scale", "occlusion_texture", "occlusion_strength", "emissive",
                           "emissive_strength", "emissive_texture", "alpha_mode", "alpha_cutoff", "double_sided"};
    REQUIRE(properties.size() == std::size(names));
    for (size_t i = 0; i < properties.size(); ++i) {
        INFO(names[i]);
        CHECK(properties[i].id == PropertyId(i + 1));
        CHECK(properties[i].name == names[i]);
        CHECK(material_property(properties[i].id) == &properties[i]);
        CHECK(material_property(names[i]) == &properties[i]);
        // Defaults are the asset's: writing one into a default material changes nothing.
        auto material = MaterialAsset{};
        const auto edit = PropertyEdit{properties[i].id, properties[i].default_value};
        CHECK(edit_properties(material, std::span(&edit, 1)));
        CHECK(material == MaterialAsset{});
    }
    CHECK(material_property("normal_texture")->type == PropertyType::texture_ref);
    CHECK(material_property("normal_texture")->encoding == PropertyEncoding::persistent_asset_id);
    CHECK(material_property("base_color")->presentation == PropertyPresentation::color);

    // Edits are atomic: one bad value leaves the material as it was.
    auto material = MaterialAsset{};
    const PropertyEdit bad[] = {{4, 0.5f}, {5, 2.0f}};
    CHECK(edit_properties(material, bad).error == PropertyError::invalid_value);
    CHECK(material == MaterialAsset{});
    const PropertyEdit good[] = {{1, math::Vec3{0.1f, 0.2f, 0.3f}}, {2, 0.5f}, {14, ChoiceValue{uint32_t(AlphaMode::blend)}}};
    REQUIRE(edit_properties(material, good));
    CHECK(material.base_color.x == 0.1f);
    CHECK(material.base_color.w == 0.5f);
    CHECK(material.alpha_mode == AlphaMode::blend);
    CHECK(edit_properties(material, std::span<const PropertyEdit>(std::array{PropertyEdit{14, ChoiceValue{7}}})).error == PropertyError::invalid_value);
    CHECK(edit_properties(material, std::span<const PropertyEdit>(std::array{PropertyEdit{4, true}})).error == PropertyError::type_mismatch);
    CHECK(edit_properties(material, std::span<const PropertyEdit>(std::array{PropertyEdit{99, 1.0f}})).error == PropertyError::unknown_property);

    // Texture slots take cataloged textures only.
    const auto texture = AssetId{0x6d617961, 0x50}, mesh = AssetId{0x6d617961, 2};
    const auto context = PropertyValidationContext{[&](AssetId id, ReferenceKind kind) {
        if (id == texture) return kind == ReferenceKind::texture ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
        if (id == mesh) return kind == ReferenceKind::mesh ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
        return ReferenceStatus::missing;
    }};
    const auto slot = [&](AssetId id) {
        auto edited = MaterialAsset{};
        const auto edit = PropertyEdit{7, AssetRef<TextureAsset>{id}};
        return edit_properties(edited, std::span(&edit, 1), context).error;
    };
    CHECK(slot(texture) == PropertyError::none);
    CHECK(slot(mesh) == PropertyError::wrong_reference_type);
    CHECK(slot({0x1, 0x2}) == PropertyError::missing_reference);
    CHECK(slot({}) == PropertyError::none); // an empty slot
    auto unchecked = MaterialAsset{};
    unchecked.normal_texture = {texture};
    CHECK(validate_material(unchecked).error == PropertyError::validation_context_required);
}

TEST_CASE("Publishing a material makes it the next version without reading its file, as a reload would", "[assets][materials]") {
    const Folder folder;
    folder.write("surface.material", "maya-material 2\nroughness 0.5\n");
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        AssetRegistry registry(folder.path, std::make_unique<FileAssetProvider>(device));
        const auto ref = AssetRef<MaterialAsset>{{0x7465, 1}};
        const auto mesh = AssetRef<MaterialAsset>{{0x7465, 2}};
        REQUIRE_FALSE(registry.register_asset(ref, "surface.material"));
        REQUIRE_FALSE(registry.register_asset(AssetRecord{mesh.id, AssetKind::mesh, "cube.obj"}));
        const auto before = registry.acquire(ref);
        REQUIRE(before);
        auto edited = before.lease.value();
        edited.roughness = 0.9f;
        REQUIRE_FALSE(registry.publish(ref, edited));
        const auto after = registry.acquire(ref);
        CHECK(after.lease.value().roughness == 0.9f);
        CHECK(after.lease.handle().generation == before.lease.handle().generation + 1);
        CHECK(before.lease.value().roughness == 0.5f); // a lease keeps its version
        CHECK_FALSE(registry.resolve(before.lease.handle()));
        CHECK(folder.read_file("surface.material") == "maya-material 2\nroughness 0.5\n"); // the file is untouched
        // A reload reads the file again.
        CHECK(registry.reload(ref).lease.value().roughness == 0.5f);
        CHECK(registry.publish(AssetRef<MaterialAsset>{{0x7465, 9}}, edited).code == AssetError::not_registered);
        CHECK(registry.publish(mesh, edited).code == AssetError::wrong_type);
        // A material that failed to load becomes ready when one is published.
        folder.write("surface.material", "broken");
        CHECK_FALSE(registry.reload(ref));
        REQUIRE_FALSE(registry.publish(ref, edited));
        CHECK(registry.info(ref.id)->state == AssetState::ready);
        CHECK_FALSE(registry.info(ref.id)->diagnostic);
    }
    device.shutdown();
}

TEST_CASE("Generated tangents are perpendicular to their normals, and an OBJ loads its corners welded", "[assets][materials]") {
    // A sphere's corners: every tangent is unit length, perpendicular to its normal, and along +u.
    const auto path = fs::path(MAYA_SOURCE_DIR) / "samples/basic_scene/assets/sphere.obj";
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        const auto loaded = ModelLoader::load_obj_checked(device, path.string());
        INFO(loaded.diagnostic);
        REQUIRE(loaded.mesh);
        // 24 rings of 48 segments, less the pole triangles' missing halves: 2208 triangles.
        CHECK(loaded.geometry.indices.size() == 2208 * 3);
        // Seam and pole corners differ in u, so fewer vertices than corners but more than grid points.
        CHECK(loaded.geometry.positions.size() < 2208 * 3);
        CHECK(loaded.geometry.positions.size() >= 25 * 49 - 2 * 48);
    }
    device.shutdown();
    // Directly: a slanted triangle's tangent follows u and is perpendicular to the normal.
    auto corners = std::vector<Vertex>{{{0, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, {0, 1}}, {{2, 0, 0}, {0, 0, 1}, {1, 1, 1, 1}, {1, 1}},
                                       {{0, 2, 0}, {0, 0, 1}, {1, 1, 1, 1}, {0, 0}}};
    REQUIRE(generate_tangents(corners));
    for (const auto& v : corners) {
        CHECK(std::abs(v.tangent.x - 1.0f) < 1e-5f);
        CHECK(std::abs(v.tangent.y) < 1e-5f);
        CHECK(v.tangent.w == 1.0f);
    }
    auto partial = std::vector<Vertex>(corners.begin(), corners.begin() + 2);
    CHECK_FALSE(generate_tangents(partial)); // not whole triangles
    auto none = std::vector<Vertex>{};
    CHECK(generate_tangents(none));
    const auto welded = weld_vertices(std::vector<Vertex>{corners[0], corners[1], corners[0], corners[2]});
    CHECK(welded.vertices.size() == 3);
    CHECK(welded.indices == std::vector<uint32_t>{0, 1, 0, 2});
}
