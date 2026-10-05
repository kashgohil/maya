// Importing glTF files into a project (#1036, docs/import.md): the catalog entries, material files, scene,
// and import file an import writes; loading what it cataloged; keeping IDs and edited files when it runs
// again; and leaving the project as it was when it fails.

#include "maya/assets/import_file.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/scene/scene_io.hpp"
#include "support/gltf.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace maya;
namespace fs = std::filesystem;

namespace {
struct TempProject {
    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / ("maya-import-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(root);
        write("project.maya", "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n");
        write("catalog.maya", "maya-assets 1\n");
    }
    ~TempProject() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void write(const std::string& name, const std::string& text) const {
        fs::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << text;
    }
    std::string read(const std::string& name) const {
        auto input = std::ifstream(root / name, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    Project project() const {
        auto opened = open_project(root);
        INFO(opened.error);
        REQUIRE(opened);
        return opened.project;
    }
    fs::path root;
};
std::string problems(const std::vector<GltfProblem>& list) {
    auto text = std::string{};
    for (const auto& problem : list) text += gltf_problem_text(problem) + "\n";
    return text;
}
#define REQUIRE_IMPORTED(result) do { INFO(problems((result).errors)); REQUIRE((result)); } while (false)

void write_props(const TempProject& project, const std::string& extra_root = "") {
    project.write("models/props.gltf", test::props_gltf(extra_root));
    project.write("models/textures/normal.png", test::flat_normal_png());
}
const AssetRecord* record(const GltfImportResult& result, std::string_view path) {
    const auto found = std::ranges::find_if(result.records, [&](const AssetRecord& r) { return r.path.generic_string() == path; });
    return found == result.records.end() ? nullptr : &*found;
}
const SceneEntity* named(const SceneDocument& document, std::string_view name) {
    for (const auto& entity : document.entities)
        for (const auto& component : entity.components)
            if (const auto* value = std::get_if<NameComponent>(&component); value && value->value == name) return &entity;
    return nullptr;
}
template<class T> const T* component(const SceneEntity& entity) {
    for (const auto& value : entity.components)
        if (const auto* typed = std::get_if<T>(&value)) return typed;
    return nullptr;
}
} // namespace

TEST_CASE("Catalog paths name parts of imported files after '#', for meshes and textures only", "[assets][import]") {
    CHECK(split_asset_path("models/helmet.glb#mesh/0/1").file == "models/helmet.glb");
    CHECK(split_asset_path("models/helmet.glb#mesh/0/1").part == "mesh/0/1");
    CHECK(split_asset_path("models/Helmet.GLTF#texture/2/color").part == "texture/2/color");
    CHECK(split_asset_path("notes/a#b.obj").part.empty()); // only glTF files have parts
    CHECK(split_asset_path("notes/a#b.obj").file == "notes/a#b.obj");
    const TempProject project;
    project.write("models/props.gltf", "{}");
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        auto registry = AssetRegistry(project.root, std::make_unique<FileAssetProvider>(device));
        CHECK_FALSE(registry.register_asset({{1, 1}, AssetKind::mesh, "models/props.gltf#mesh/0/0"}));
        CHECK_FALSE(registry.register_asset({{1, 2}, AssetKind::mesh, "models/props.gltf#mesh/0/1"}));
        CHECK(registry.register_asset({{1, 3}, AssetKind::mesh, "models/props.gltf#mesh/0/0"}).code == AssetError::duplicate_path);
        CHECK(registry.register_asset({{1, 4}, AssetKind::material, "models/props.gltf#mesh/0/2"}).code == AssetError::invalid_path);
        CHECK(registry.register_asset({{1, 5}, AssetKind::mesh, "../props.gltf#mesh/0/0"}).code == AssetError::invalid_path);
        CHECK_FALSE(registry.register_asset({{1, 6}, AssetKind::mesh, "models/gone.glb#mesh/0/0"}));
        CHECK_FALSE(registry.register_asset({{1, 7}, AssetKind::mesh, "models/props.gltf#mesh/zero"}));
        // The file must be there, and the part must be one the provider reads.
        CHECK(registry.acquire(AssetRef<MeshAsset>{{1, 6}}).diagnostic.code == AssetError::missing_file);
        CHECK(registry.acquire(AssetRef<MeshAsset>{{1, 7}}).diagnostic.code == AssetError::invalid_path);
        CHECK(registry.acquire(AssetRef<MeshAsset>{{1, 1}}).diagnostic.code == AssetError::invalid_data); // "{}" is not glTF
    }
    device.shutdown();
}

TEST_CASE("Import files round-trip, and refuse what they cannot hold", "[assets][import]") {
    auto file = ImportFile{};
    file.settings = {TextureCompression::rgba8, false, 0.01f, ImportUp::z, false, true};
    file.scene = "props.scene";
    file.scene_written = 0x51ac09e2d3b4f607;
    file.meshes.push_back({{0x6d617961, 0x1a2b}, "mesh/0/0", "Panel/0"});
    file.textures.push_back({{0x6d617961, 0x1a2c}, "texture/1/normal", "normal/normal"});
    file.materials.push_back({{0x6d617961, 0x1a2d}, "props/materials/Painted \"blue\".material", "Painted \"blue\"", 0x9f3c2b1a00ffe1d2});
    file.entities.push_back({{0xabc, 0xdef}, "/Root/Panel"});
    file.files.push_back("textures/normal map.png");
    const auto text = write_import_file(file);
    CHECK(text.ends_with("entity abc def \"/Root/Panel\"\nfile \"textures/normal map.png\"\n"));
    CHECK(text.starts_with("maya-import 1\ncompression rgba8\nmips off\nscale 0.01\nup z\nlights off\ncameras on\n"
                           "scene \"props.scene\" 51ac09e2d3b4f607\nmesh 6d617961 1a2b \"mesh/0/0\" \"Panel/0\"\n"));
    auto input = std::istringstream(text);
    const auto back = read_import_file(input);
    INFO(back.error);
    REQUIRE(back);
    CHECK(write_import_file(back.file) == text);
    CHECK(back.file.materials[0].identity == "Painted \"blue\"");
    CHECK(back.file.entities[0].id == EntityId{0xabc, 0xdef});
    CHECK(back.file.settings.scale == 0.01f);
    CHECK(back.file.settings.up == ImportUp::z);
    CHECK_FALSE(back.file.settings.lights);
    const auto error = [](const std::string& text) {
        auto stream = std::istringstream(text);
        return read_import_file(stream).error;
    };
    CHECK(error("maya-import 2\n") == "line 1: Import file version 2 is not supported; this build reads version 1");
    CHECK(error("maya-import 1\nmesh 1 2 \"mesh/0/0\" \"a\"\nmips on\n") == "line 3: 'mips' must come before the entries");
    CHECK(error("maya-import 1\nmesh 1 2 \"mesh/0/0\" \"a\"\nmesh 1 2 \"mesh/0/1\" \"b\"\n") == "line 3: the mesh ID 1 2 appears more than once");
    CHECK(error("maya-import 1\nmesh 0 0 \"mesh/0/0\" \"a\"\n").find("nonzero ID") != std::string::npos);
    CHECK(error("maya-import 1\ntexture 1 2 \"mesh/0/0\" \"a\"\n").find("starts with 'texture/'") != std::string::npos);
    CHECK(error("maya-import 1\nmaterial 1 2 \"../x.material\" \"a\" 0\n").find("must stay at or below") != std::string::npos);
    CHECK(error("maya-import 1\ncompression bc7\n") == "line 2: 'compression' must be astc or rgba8");
    CHECK(error("maya-import 1\nscale 0\n") == "line 2: 'scale' must be a number from 1e-06 to 1e+06");
    CHECK(error("maya-import 1\nscale -1\n") == "line 2: 'scale' must be a number from 1e-06 to 1e+06");
    CHECK(error("maya-import 1\nup x\n") == "line 2: 'up' must be y or z");
    CHECK(error("maya-import 1\nlights yes\n") == "line 2: 'lights' must be on or off");
    CHECK(error("maya-import 1\nup y\nup z\n") == "line 3: 'up' appears more than once");
    CHECK(error("maya-import 1\nwidget 1 2\n") == "line 2: Unknown key 'widget'");
    CHECK(error("maya-import 1\nfile \"../outside.png\"\n").find("must stay at or below") != std::string::npos);
    CHECK(import_file_path("models/props.glb") == "models/props.glb.import");
}

TEST_CASE("Importing a glTF file catalogs its parts, writes material files, a scene, and an import file", "[assets][import]") {
    const TempProject project;
    write_props(project);
    const auto result = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(result);
    // Painted's normal map has no transform, but its base color map does: Maya has one per material.
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].path == "materials[0]");
    CHECK(result.warnings[0].message.find("one transform") != std::string::npos);
    CHECK(result.scene == "models/props.scene");
    CHECK(result.written == std::vector<fs::path>{"models/props/materials/Painted.material", "models/props/materials/Glow.material",
                                                 "models/props/materials/default.material", "models/props.scene", "models/props.gltf.import",
                                                 "catalog.maya"});
    // Each drawn primitive is a mesh; each texture is one per role it is used in; each material a file.
    REQUIRE(result.records.size() == 3 + 2 + 3);
    const auto* panel_painted = record(result, "models/props.gltf#mesh/0/0");
    const auto* tile = record(result, "models/props.gltf#mesh/1/0");
    const auto* color = record(result, "models/props.gltf#texture/0/color");
    const auto* normal = record(result, "models/props.gltf#texture/1/normal");
    const auto* painted = record(result, "models/props/materials/Painted.material");
    const auto* fallback = record(result, "models/props/materials/default.material");
    REQUIRE(panel_painted);
    REQUIRE(tile);
    REQUIRE(color);
    REQUIRE(normal);
    REQUIRE(painted);
    REQUIRE(fallback);
    CHECK(record(result, "models/props.gltf#mesh/0/1"));
    CHECK(color->kind == AssetKind::texture);

    // The catalog holds them all, and the registry loads them from inside the source.
    auto catalog_input = std::istringstream(project.read("catalog.maya"));
    const auto catalog = read_asset_catalog(catalog_input);
    REQUIRE(catalog);
    CHECK(catalog.records.size() == result.records.size());
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        auto opened = open_project_assets(project.project(), std::make_unique<FileAssetProvider>(device));
        INFO(opened.error);
        REQUIRE(opened);
        auto& registry = *opened.registry;
        for (const auto& entry : result.records) {
            INFO(entry.path.generic_string());
            if (entry.kind == AssetKind::mesh) CHECK(registry.acquire(AssetRef<MeshAsset>{entry.id}).diagnostic.message == "");
            if (entry.kind == AssetKind::texture) CHECK(registry.acquire(AssetRef<TextureAsset>{entry.id}).diagnostic.message == "");
            if (entry.kind == AssetKind::material) CHECK(registry.acquire(AssetRef<MaterialAsset>{entry.id}).diagnostic.message == "");
        }
        CHECK(registry.acquire(AssetRef<TextureAsset>{normal->id}).lease.value().role() == TextureRole::normal);
        CHECK(registry.acquire(AssetRef<MeshAsset>{panel_painted->id}).lease.value().geometry().indices.size() == 6);
        // The material: glTF's factors, the cataloged textures, and the base color map's transform.
        const auto& material = registry.acquire(AssetRef<MaterialAsset>{painted->id}).lease.value();
        CHECK(material.base_color.y == 0.5f);
        CHECK(material.metallic == 0);
        CHECK(material.base_color_texture.id == color->id);
        CHECK(material.normal_texture.id == normal->id);
        CHECK(material.uv_offset.x == 0.5f);
        CHECK(material.uv_scale.y == 2);
        // glTF's default material for the primitive without one: white, metallic, rough.
        const auto& standard = registry.acquire(AssetRef<MaterialAsset>{fallback->id}).lease.value();
        CHECK(standard.metallic == 1);
        CHECK(standard.roughness == 1);

        // The scene: a root named for the file, then the nodes, with their components.
        const auto scene = load_scene_file(project.root / result.scene, asset_property_context(registry));
        REQUIRE(scene.diagnostics.empty());
        const auto& document = scene.document;
        CHECK(result.entities == document.entities.size());
        REQUIRE(document.entities.size() == 8); // props, Root, Panel (and its two primitives), Tile, Lamp, Eye
        const auto* root = named(document, "props");
        REQUIRE(root);
        CHECK_FALSE(root->parent);
        const auto* panel = named(document, "Panel");
        REQUIRE(panel);
        CHECK(component<TransformComponent>(*panel)->translation.y == 1);
        CHECK_FALSE(component<MeshRendererComponent>(*panel)); // two materials: a child entity for each
        const auto* glow = named(document, "Glow");
        REQUIRE(glow);
        CHECK(glow->parent == panel->id);
        CHECK(component<MeshRendererComponent>(*glow)->material.id == record(result, "models/props/materials/Glow.material")->id);
        const auto* tile_entity = named(document, "Tile");
        REQUIRE(tile_entity);
        CHECK(tile_entity->parent == panel->id);
        CHECK(component<MeshRendererComponent>(*tile_entity)->mesh.id == tile->id);
        CHECK(component<MeshRendererComponent>(*tile_entity)->material.id == fallback->id);
        const auto* lamp = named(document, "Lamp");
        REQUIRE(lamp);
        REQUIRE(component<LightComponent>(*lamp));
        CHECK(component<LightComponent>(*lamp)->kind == LightKind::point);
        CHECK(component<LightComponent>(*lamp)->range == 5);
        REQUIRE(named(document, "Eye"));
        CHECK(component<CameraComponent>(*named(document, "Eye"))->vertical_fov == 0.7f);
    }
    device.shutdown();

    // The import file: the settings, the scene, and an entry for everything made.
    auto import_input = std::istringstream(project.read("models/props.gltf.import"));
    const auto import = read_import_file(import_input);
    REQUIRE(import);
    CHECK(import.file.scene == "props.scene");
    CHECK(import.file.meshes.size() == 3);
    CHECK(import.file.textures.size() == 2);
    CHECK(import.file.materials.size() == 3);
    CHECK(import.file.entities.size() == 8);
    CHECK(import.file.materials[0].identity == "Painted");
    CHECK(import.file.materials[0].file == "props/materials/Painted.material");
    CHECK(import.file.meshes[0].identity == "Panel/0");
    CHECK(import.file.files == std::vector<fs::path>{"textures/normal.png"}); // the image beside it; the buffer is embedded
}

TEST_CASE("Importing again keeps every ID, rewrites nothing unchanged, and keeps edited files", "[assets][import]") {
    const TempProject project;
    write_props(project);
    const auto first = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(first);
    const auto scene = project.read("models/props.scene");
    const auto again = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(again);
    CHECK(again.records.size() == first.records.size());
    for (size_t i = 0; i < first.records.size(); ++i) {
        CHECK(again.records[i].id == first.records[i].id);
        CHECK(again.records[i].path == first.records[i].path);
    }
    CHECK(again.written == std::vector<fs::path>{"models/props.gltf.import", "catalog.maya"});
    CHECK(project.read("models/props.scene") == scene); // the same entity IDs

    // An edited material is kept, and so is its recorded hash: it stays edited on later imports.
    const auto edited = project.read("models/props/materials/Glow.material") + "# brighter\n";
    project.write("models/props/materials/Glow.material", edited);
    for (int run = 0; run < 2; ++run) {
        const auto kept = import_gltf(project.project(), "models/props.gltf");
        REQUIRE_IMPORTED(kept);
        CHECK(kept.kept == std::vector<fs::path>{"models/props/materials/Glow.material"});
        CHECK(project.read("models/props/materials/Glow.material") == edited);
    }
    // A deleted material file is written again.
    fs::remove(project.root / "models/props/materials/Painted.material");
    const auto restored = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(restored);
    CHECK(fs::exists(project.root / "models/props/materials/Painted.material"));
    CHECK(restored.records[5].id == first.records[5].id);
}

TEST_CASE("Reimporting a changed file reports what was added, removed, and renamed; a renamed part keeps its ID", "[assets][import]") {
    const TempProject project;
    write_props(project);
    const auto first = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(first);
    CHECK(first.added.empty()); // a first import reports no changes
    const auto id_of = [](const GltfImportResult& result, std::string_view path) {
        const auto found = std::ranges::find_if(result.records, [&](const AssetRecord& r) { return r.path.generic_string() == path; });
        return found == result.records.end() ? AssetId{} : found->id;
    };
    // In the modelling program: the Tile mesh renamed Floor, the Glow material Shine, and the Lamp node Light.
    auto text = test::props_gltf();
    for (const auto& [from, to] : {std::pair{R"("name":"Tile","primitives")", R"("name":"Floor","primitives")"},
                                   std::pair{R"("name":"Glow")", R"("name":"Shine")"}, std::pair{R"("name":"Lamp")", R"("name":"Light")"}}) {
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string_view(from).size(), to);
    }
    project.write("models/props.gltf", text);
    const auto again = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(again);
    const auto list = [](const std::vector<GltfImportChange>& changes) {
        auto text = std::string{};
        for (const auto& change : changes) text += change.what + " " + (change.previous.empty() ? "" : change.previous + " -> ") + change.identity + "\n";
        return text;
    };
    CHECK(list(again.renamed) == "mesh Tile/0 -> Floor/0\n");
    CHECK(list(again.added) == "material Shine\nentity /Root/Light\n");
    CHECK(list(again.removed) == "material Glow\nentity /Root/Lamp\n");
    // The renamed mesh is the same asset; the renamed material is a new one, and the old one stays.
    CHECK(id_of(again, "models/props.gltf#mesh/1/0") == id_of(first, "models/props.gltf#mesh/1/0"));
    CHECK(fs::exists(project.root / "models/props/materials/Shine.material"));
    CHECK(fs::exists(project.root / "models/props/materials/Glow.material"));
    CHECK(project.read("catalog.maya").find("models/props/materials/Glow.material") != std::string::npos);
    // Importing the same file once more changes nothing.
    const auto settled = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(settled);
    CHECK(settled.added.empty());
    CHECK(settled.removed.empty());
    CHECK(settled.renamed.empty());
}

TEST_CASE("Import settings scale and turn the whole import, and leave out lights or cameras", "[assets][import]") {
    const TempProject project;
    write_props(project);
    project.write("models/props.gltf.import", "maya-import 1\nscale 0.01\nup z\nlights off\ncameras off\n");
    const auto result = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(result);
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        auto opened = open_project_assets(project.project(), std::make_unique<FileAssetProvider>(device));
        REQUIRE(opened);
        const auto scene = load_scene_file(project.root / result.scene, asset_property_context(*opened.registry));
        REQUIRE(scene.diagnostics.empty());
        // The root: a hundredth the size, and turned so the file's +Z is up.
        const auto* root = named(scene.document, "props");
        REQUIRE(root);
        const auto& transform = *component<TransformComponent>(*root);
        CHECK(transform.scale.x == 0.01f);
        const auto up = transform.rotation.rotate(math::Vec3{0, 0, 1});
        CHECK((up - math::Vec3{0, 1, 0}).length() < 1e-6f);
        // No lights or cameras; their nodes stay.
        for (const auto& entity : scene.document.entities) {
            CHECK_FALSE(component<LightComponent>(entity));
            CHECK_FALSE(component<CameraComponent>(entity));
        }
        CHECK(named(scene.document, "Lamp"));
        CHECK(named(scene.document, "Eye"));
    }
    device.shutdown();
    // The settings are kept, and written in full.
    CHECK(project.read("models/props.gltf.import").starts_with("maya-import 1\ncompression astc\nmips on\nscale 0.01\nup z\nlights off\ncameras off\n"));
}

TEST_CASE("A failed import leaves the project as it was", "[assets][import]") {
    const TempProject project;
    write_props(project, R"(,"extensionsRequired":["KHR_draco_mesh_compression"])");
    project.write("catalog.maya", "maya-assets 1\nmaterial 6d617961 1 \"other.material\"\n");
    const auto catalog = project.read("catalog.maya");
    const auto result = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_FALSE(result);
    CHECK(result.errors[0].path == "extensionsRequired[0]");
    CHECK(project.read("catalog.maya") == catalog);
    CHECK_FALSE(fs::exists(project.root / "models/props.scene"));
    CHECK_FALSE(fs::exists(project.root / "models/props"));
    CHECK_FALSE(fs::exists(project.root / "models/props.gltf.import"));
    // Files outside the content, and files that are not glTF, are refused by name.
    CHECK(import_gltf(project.project(), "../elsewhere.glb").errors[0].message.find("outside the project") != std::string::npos);
    CHECK(import_gltf(project.project(), "models/textures/normal.png").errors[0].message.find("not a .gltf or .glb") != std::string::npos);
    CHECK(import_gltf(project.project(), "models/gone.glb").errors[0].message.find("missing") != std::string::npos);
    // An existing scene of the same name is not replaced: the import's scene takes another name.
    write_props(project);
    project.write("models/props.scene", "not ours\n");
    const auto beside = import_gltf(project.project(), "models/props.gltf");
    REQUIRE_IMPORTED(beside);
    CHECK(beside.scene == "models/props 2.scene");
    CHECK(project.read("models/props.scene") == "not ours\n");
    CHECK(project.read("catalog.maya").starts_with(catalog)); // the other entry stays, first
}

TEST_CASE("The DamagedHelmet sample imports, and every part it catalogs loads", "[assets][import][samples]") {
    const auto sample = fs::path(MAYA_RENDER_SAMPLES) / "Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb";
    if (!fs::exists(sample)) SKIP("no DamagedHelmet sample; run tools/fetch_render_samples.sh");
    const TempProject project;
    fs::create_directories(project.root / "models");
    fs::copy_file(sample, project.root / "models/helmet.glb");
    // Uncompressed: ASTC compression of five 2048-texel textures is slow in unoptimized builds. An
    // import file that is already there keeps its settings.
    project.write("models/helmet.glb.import", "maya-import 1\ncompression rgba8\n");
    const auto result = import_gltf(project.project(), "models/helmet.glb");
    REQUIRE_IMPORTED(result);
    CHECK(result.warnings.empty());
    CHECK(result.records.size() == 1 + 5 + 1);
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        auto opened = open_project_assets(project.project(), std::make_unique<FileAssetProvider>(device));
        REQUIRE(opened);
        auto roles = std::vector<TextureRole>{};
        for (const auto& entry : result.records) {
            INFO(entry.path.generic_string());
            if (entry.kind == AssetKind::mesh) CHECK(opened.registry->acquire(AssetRef<MeshAsset>{entry.id}).diagnostic.message == "");
            if (entry.kind == AssetKind::texture) {
                const auto texture = opened.registry->acquire(AssetRef<TextureAsset>{entry.id});
                CHECK(texture.diagnostic.message == "");
                if (texture) roles.push_back(texture.lease.value().role());
            }
        }
        CHECK(std::ranges::count(roles, TextureRole::color) == 2); // base color and emissive
        CHECK(std::ranges::count(roles, TextureRole::normal) == 1);
        CHECK(std::ranges::count(roles, TextureRole::data) == 2); // metallic-roughness and occlusion
    }
    CHECK(project.read("models/helmet.glb.import").starts_with("maya-import 1\ncompression rgba8\nmips on\nscale 1\nup y\n"
                                                               "lights on\ncameras on\nscene \"helmet.scene\" "));
    device.shutdown();
}

TEST_CASE("R1's content imports with every node, drawn primitive, and material, and its meshes load", "[assets][import][samples]") {
    const auto models = fs::path(MAYA_RENDER_SAMPLES) / "Models";
    if (!fs::is_directory(models)) SKIP("no samples; run tools/fetch_render_samples.sh");
    for (const auto* relative : {"ABeautifulGame/glTF-Binary/ABeautifulGame.glb", "FlightHelmet/glTF/FlightHelmet.gltf",
                                 "CesiumMan/glTF-Binary/CesiumMan.glb"}) {
        INFO(relative);
        const auto source = models / relative;
        const TempProject project;
        fs::create_directories(project.root / "models");
        fs::copy_file(source, project.root / "models" / source.filename());
        const auto opened = GltfFile::open(source);
        REQUIRE(opened);
        const auto& document = opened.file->document();
        if (source.extension() == ".gltf")
            for (const auto& named : document.files) {
                fs::create_directories((project.root / "models" / named).parent_path());
                fs::copy_file(source.parent_path() / named, project.root / "models" / named);
            }
        const auto result = import_gltf(project.project(), fs::path("models") / source.filename());
        REQUIRE_IMPORTED(result);
        // What the file has: the nodes under its scene's roots, and each drawn primitive of their meshes.
        size_t nodes = 0, extra = 0;
        const auto walk = [&](auto&& self, uint32_t index) -> void {
            ++nodes;
            const auto& node = document.nodes[index];
            if (node.mesh) {
                const auto drawn = std::ranges::count_if(document.meshes[*node.mesh].primitives, [](const GltfPrimitive& p) { return p.drawable; });
                if (drawn > 1) extra += size_t(drawn);
            }
            for (const auto child : node.children) self(self, child);
        };
        for (const auto root : document.roots) walk(walk, root);
        CHECK(result.entities == 1 + nodes + extra);
        size_t primitives = 0;
        for (const auto& mesh : document.meshes) primitives += size_t(std::ranges::count_if(mesh.primitives, [](const GltfPrimitive& p) { return p.drawable; }));
        CHECK(std::ranges::count(result.records, AssetKind::mesh, &AssetRecord::kind) == std::ptrdiff_t(primitives));
        CHECK(std::ranges::count(result.records, AssetKind::material, &AssetRecord::kind) >= std::ptrdiff_t(document.materials.size()));
        // Every mesh loads, with the file's geometry.
        NullGraphicsDevice device;
        REQUIRE(device.initialize(nullptr));
        {
            auto assets = open_project_assets(project.project(), std::make_unique<FileAssetProvider>(device));
            REQUIRE(assets);
            for (const auto& record : result.records)
                if (record.kind == AssetKind::mesh) {
                    const auto mesh = assets.registry->acquire(AssetRef<MeshAsset>{record.id});
                    INFO(mesh.diagnostic.message);
                    CHECK(mesh);
                }
            const auto scene = load_scene_file(project.root / result.scene, asset_property_context(*assets.registry));
            CHECK(scene.diagnostics.empty());
        }
        device.shutdown();
    }
}
