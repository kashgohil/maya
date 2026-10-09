// Packages (#1039, docs/projects.md#packages): packaging takes a project's scenes and exactly the assets
// they reach, cooked, refuses what is missing or unreadable, and makes the same package from the same
// project; packaged players read only their bundle and draw what the editor draws.

#include "maya/assets/asset_cooker.hpp"
#include "maya/assets/gltf.hpp"
#include "maya/assets/package.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/assets/registry.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/package/packager.hpp"
#include "r1.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/scene/world_io.hpp"
#include "maya/streaming/world_streamer.hpp"
#include "maya/simulation/script_assets.hpp"
#include "maya/simulation/play_session.hpp"
#include "support/gltf.hpp"
#include "support/metal_view.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <unistd.h>

using namespace maya;
namespace fs = std::filesystem;

namespace {
/// A temporary folder, removed afterwards.
struct Scratch {
    fs::path root;
    Scratch() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / ("maya-package-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};
/// A copy of the sample project, in a folder whose name has a space, as games' names do.
fs::path copy_sample(const Scratch& scratch) {
    const auto project = scratch.root / "Sample Game";
    fs::copy(fs::path(MAYA_SOURCE_DIR) / "samples/basic_scene", project, fs::copy_options::recursive);
    fs::remove_all(project / ".maya"); // an earlier run's cook cache
    return project;
}
PackageOptions options_for(const fs::path& project, const fs::path& output, std::vector<fs::path> scenes = {}) {
    auto options = PackageOptions{};
    options.project = project;
    options.output = output;
    options.scenes = std::move(scenes);
    options.player = MAYA_PLAYER;
    options.shader = fs::path(MAYA_SOURCE_DIR) / "resources/shaders/metal/renderer.metal";
    return options;
}
std::string read_text(const fs::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
std::vector<std::byte> read_bytes(const fs::path& path) {
    const auto text = read_text(path);
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}
std::vector<AssetRecord> packaged_catalog(const fs::path& bundle) {
    auto input = std::ifstream(bundle / "Contents/Resources/project/content/catalog.maya");
    auto catalog = read_asset_catalog(input);
    REQUIRE(catalog);
    return catalog.records;
}
/// Every file in a folder, relative and sorted.
std::vector<std::string> files_in(const fs::path& folder) {
    auto files = std::vector<std::string>{};
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file()) files.push_back(entry.path().lexically_relative(folder).generic_string());
    std::ranges::sort(files);
    return files;
}
/// Runs a command, returning its exit status and what it printed to both streams.
std::pair<int, std::string> run(const std::string& command) {
    auto* pipe = ::popen((command + " 2>&1").c_str(), "r");
    REQUIRE(pipe);
    auto output = std::string{};
    auto buffer = std::array<char, 4096>{};
    while (const auto read = std::fread(buffer.data(), 1, buffer.size(), pipe)) output.append(buffer.data(), read);
    const auto status = ::pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}
std::string quoted(const fs::path& path) { return "'" + path.string() + "'"; }
} // namespace

TEST_CASE("A package holds its scenes and exactly the assets they reach, cooked, and nothing else", "[package]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    const auto bundle = scratch.root / "out/Sample Game.app";
    const auto report = package_project(options_for(project, bundle, {"materials.scene", "physics.scene"}));
    INFO(report.error);
    REQUIRE(report);
    CHECK(report.manifest.name == "Sample Game");
    CHECK(report.manifest.scenes == std::vector<fs::path>{"basic.scene", "materials.scene", "physics.scene"}); // the startup scene first
    CHECK(report.bytes > 0);
    CHECK(verify_package(bundle / "Contents/Resources").empty());
    CHECK(fs::is_regular_file(bundle / "Contents/MacOS/maya_player"));
    CHECK(read_text(bundle / "Contents/Info.plist").find("<string>maya_player</string>") != std::string::npos);
    // No source images, models, descriptors, import files, or cook cache: only cooked and authored files.
    for (const auto& file : files_in(bundle)) {
        INFO(file);
        for (const auto* source : {".png", ".jpg", ".hdr", ".obj", ".glb", ".gltf", ".bin", ".import", ".texture", ".environment", ".ktx2"})
            CHECK_FALSE(file.ends_with(source));
        CHECK(file.find(".maya/") == std::string::npos);
    }
    // The catalog lists cooked files, which unwrap into exactly what cooking the sources gives.
    auto cooker = AssetCooker{};
    auto kinds = std::map<AssetKind, size_t>{};
    for (const auto& record : packaged_catalog(bundle)) {
        INFO(record.path);
        ++kinds[record.kind];
        const auto packaged = bundle / "Contents/Resources/project/content" / record.path;
        REQUIRE(fs::is_regular_file(packaged));
        const auto source_catalog = [&] {
            auto input = std::ifstream(project / "assets/catalog.maya");
            return read_asset_catalog(input).records;
        }();
        const auto source = std::ranges::find(source_catalog, record.id, &AssetRecord::id);
        REQUIRE(source != source_catalog.end());
        const auto payload = unwrap_cooked(read_bytes(packaged));
        switch (record.kind) {
        case AssetKind::mesh: {
            CHECK(record.path.extension() == cooked_mesh_extension);
            REQUIRE(payload);
            CHECK(*payload == write_cooked_mesh(*cooker.mesh(project / "assets" / source->path).value));
            break;
        }
        case AssetKind::texture: {
            CHECK(record.path.extension() == cooked_texture_extension);
            REQUIRE(payload);
            CHECK(*payload == write_cooked_texture(*cooker.texture(project / "assets" / source->path).value));
            break;
        }
        case AssetKind::environment:
            CHECK(record.path.extension() == cooked_environment_extension);
            REQUIRE(payload);
            CHECK(read_cooked_environment(*payload));
            break;
        case AssetKind::skin: // R1's CesiumMan has them; the sample project does not
            CHECK(record.path.extension() == cooked_skin_extension);
            REQUIRE(payload);
            CHECK(read_skin(*payload));
            break;
        case AssetKind::animation:
            CHECK(record.path.extension() == cooked_animation_extension);
            REQUIRE(payload);
            CHECK(read_animation(*payload));
            break;
        case AssetKind::material:
        case AssetKind::script:
            CHECK(record.path == source->path); // authored, unchanged
            CHECK(read_text(packaged) == read_text(project / "assets" / source->path));
            break;
        }
    }
    CHECK(kinds[AssetKind::mesh] == report.meshes);
    CHECK(kinds[AssetKind::texture] == report.textures);
    CHECK(report.textures >= 4); // the material scene's maps
    CHECK(report.environments == 1); // the sky the material scene uses; the workshop it does not
    CHECK(report.scripts >= 1); // the physics scene's

    // The startup scene alone reaches no textures, environments, or scripts.
    const auto small = package_project(options_for(project, scratch.root / "out/Small.app"));
    REQUIRE(small);
    CHECK(small.textures == 0);
    CHECK(small.environments == 0);
    CHECK(small.scripts == 0);
    CHECK(small.meshes == 2);
    CHECK(small.manifest.scenes == std::vector<fs::path>{"basic.scene"});
}

TEST_CASE("The same project and build make the same package", "[package]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    const auto first = package_project(options_for(project, scratch.root / "one/Game.app", {"materials.scene"}));
    REQUIRE(first);
    // A second time, from the cook cache the first filled, and a third from a fresh copy that cooks again.
    const auto second = package_project(options_for(project, scratch.root / "two/Game.app", {"materials.scene"}));
    REQUIRE(second);
    fs::remove_all(project / ".maya");
    const auto third = package_project(options_for(project, scratch.root / "three/Game.app", {"materials.scene"}));
    REQUIRE(third);
    CHECK(first.manifest.content == second.manifest.content);
    CHECK(first.manifest.content == third.manifest.content);
    const auto files = files_in(scratch.root / "one/Game.app");
    CHECK(files == files_in(scratch.root / "three/Game.app"));
    for (const auto& file : files) {
        INFO(file);
        CHECK(read_bytes(scratch.root / "one/Game.app" / file) == read_bytes(scratch.root / "three/Game.app" / file));
    }
}

TEST_CASE("Packaging refuses missing and unreadable content with the reason, and leaves an earlier package as it was", "[package]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    const auto bundle = scratch.root / "out/Game.app";
    REQUIRE(package_project(options_for(project, bundle, {"materials.scene"})));
    const auto before = read_text(bundle / "Contents/Resources/package.maya");
    const auto refused = [&](const std::string& reason, std::vector<fs::path> scenes = {"materials.scene"}, fs::path output = {}) {
        const auto report = package_project(options_for(project, output.empty() ? bundle : output, std::move(scenes)));
        INFO(report.error);
        CHECK_FALSE(report);
        CHECK(report.error.find(reason) != std::string::npos);
        CHECK(read_text(bundle / "Contents/Resources/package.maya") == before); // the earlier package stays
        CHECK(verify_package(bundle / "Contents/Resources").empty());
        CHECK_FALSE(fs::exists(fs::path(bundle.string() + ".partial")));
    };
    SECTION("A mesh's file is missing") {
        fs::remove(project / "assets/pyramid.obj");
        refused("mesh pyramid.obj");
    }
    SECTION("A texture's image cannot be decoded") {
        std::ofstream(project / "assets/textures/grid.png", std::ios::binary) << "not a PNG";
        refused("texture textures/grid.texture");
    }
    SECTION("An environment's image is gone") {
        fs::remove(project / "assets/environments/kloofendal_48d_partly_cloudy_puresky_1k.hdr");
        refused("environment environments/sky.environment");
    }
    SECTION("A scene names an asset the catalog does not have") {
        auto text = read_text(project / "assets/basic.scene");
        const auto at = text.find("mesh 6d617961 2");
        REQUIRE(at != std::string::npos);
        text.replace(at, 15, "mesh 6d617961 77");
        std::ofstream(project / "assets/basic.scene", std::ios::binary) << text;
        refused("basic.scene");
    }
    SECTION("A named scene is not in the project") {
        refused("is outside the project's content root", {"../elsewhere.scene"});
    }
    SECTION("The output is not a bundle, or is something else") {
        refused("must be a macOS application bundle", {}, scratch.root / "out/Game");
        fs::create_directories(scratch.root / "out/Other.app");
        std::ofstream(scratch.root / "out/Other.app/notes.txt") << "mine";
        refused("is not a package", {}, scratch.root / "out/Other.app");
        CHECK(read_text(scratch.root / "out/Other.app/notes.txt") == "mine");
    }
}

TEST_CASE("A package's manifest and cooked files refuse damage", "[package]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    const auto bundle = scratch.root / "Game.app";
    const auto resources = bundle / "Contents/Resources";
    const auto report = package_project(options_for(project, bundle));
    REQUIRE(report);
    // The manifest round-trips, and its content digest guards its file list.
    auto text = std::stringstream{};
    write_package_manifest(text, report.manifest);
    const auto read = read_package_manifest(text);
    REQUIRE(read);
    CHECK(read.manifest.content == report.manifest.content);
    CHECK(read.manifest.files.size() == report.manifest.files.size());
    auto altered = read_text(resources / package_manifest_name);
    const auto size_at = altered.find(" 1", altered.find("file "));
    altered.insert(size_at + 1, "9");
    auto altered_input = std::istringstream(altered);
    CHECK(read_package_manifest(altered_input).error == "the content digest does not match the files");
    // A changed, missing, or extra file is found.
    const auto catalog = resources / "project/content/catalog.maya";
    const auto original = read_text(catalog);
    std::ofstream(catalog, std::ios::app) << "\n";
    CHECK(verify_package(resources).find("differs from the manifest") != std::string::npos);
    std::ofstream(catalog, std::ios::binary | std::ios::trunc) << original;
    std::ofstream(resources / "extra.txt") << "x";
    CHECK(verify_package(resources).find("extra.txt is not in the manifest") != std::string::npos);
    fs::remove(resources / "extra.txt");
    CHECK(verify_package(resources).empty());
    // A damaged cooked file is refused, not drawn, and so is anything not cooked.
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    auto provider = PackageAssetProvider(device);
    const auto mesh = resources / "project/content" / packaged_catalog(bundle).front().path;
    REQUIRE(mesh.extension() == cooked_mesh_extension);
    CHECK(provider.load_mesh(mesh).value);
    auto bytes = read_bytes(mesh);
    bytes.back() ^= std::byte{1};
    std::ofstream(mesh, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    CHECK(provider.load_mesh(mesh).diagnostic.message.find("damaged") != std::string::npos);
    CHECK(provider.load_mesh(project / "assets/cube.obj").diagnostic.message.find("only cooked content") != std::string::npos);
    CHECK(provider.load_texture(project / "assets/textures/grid.texture").diagnostic.message.find("only cooked content") != std::string::npos);
    CHECK_FALSE(unwrap_cooked(std::vector<std::byte>(10)));
    device.shutdown();
}

TEST_CASE("Imported glTF content is packaged cooked, without the file it came from", "[package][import]") {
    const Scratch scratch;
    const auto root = scratch.root / "Props";
    fs::create_directories(root / "models");
    std::ofstream(root / "project.maya") << "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\nstartup \"models/props.scene\"\n";
    std::ofstream(root / "catalog.maya") << "maya-assets 1\n";
    std::ofstream(root / "models/props.gltf", std::ios::binary) << test::props_gltf();
    fs::create_directories(root / "models/textures");
    std::ofstream(root / "models/textures/normal.png", std::ios::binary) << test::flat_normal_png();
    auto opened = open_project(root);
    REQUIRE(opened);
    const auto imported = import_gltf(opened.project, "models/props.gltf");
    REQUIRE(imported);
    REQUIRE(imported.scene == fs::path("models/props.scene"));
    const auto bundle = scratch.root / "Props.app";
    const auto report = package_project(options_for(root, bundle));
    INFO(report.error);
    REQUIRE(report);
    CHECK(report.meshes == size_t(std::ranges::count(imported.records, AssetKind::mesh, &AssetRecord::kind)));
    CHECK(report.textures == size_t(std::ranges::count(imported.records, AssetKind::texture, &AssetRecord::kind)));
    for (const auto& record : packaged_catalog(bundle)) CHECK(record.path.generic_string().find('#') == std::string::npos);
    for (const auto& file : files_in(bundle)) CHECK_FALSE(file.ends_with(".gltf"));
}

// On Metal, with the real player: packages draw exactly what the editor draws, and find nothing outside
// their bundle.
namespace {
/// Renders a scene's first camera through a registry, as the editor's and the player's views do.
test::RgbImage render_scene(test::Gpu& gpu, AssetRegistry& registry, const fs::path& scene, EntityId camera, uint32_t width, uint32_t height) {
    auto loaded = load_scene_file(scene, asset_property_context(registry));
    INFO(scene);
    REQUIRE(loaded);
    auto world = instantiate_scene(std::move(loaded.document), asset_property_context(registry));
    REQUIRE(world);
    const auto handle = world.world->find(camera);
    REQUIRE(handle);
    const auto view = extract_render_view(*world.world, *handle, width, height);
    REQUIRE(view);
    return gpu.render(*world.world, registry, *view);
}
std::unique_ptr<AssetRegistry> source_registry(GraphicsDevice& device, const fs::path& project_folder) {
    auto opened = open_project(project_folder);
    REQUIRE(opened);
    auto assets = open_project_assets(opened.project, std::make_unique<FileAssetProvider>(device, std::make_shared<CookCache>(cook_cache_folder(opened.project))));
    REQUIRE(assets);
    return std::move(assets.registry);
}
std::unique_ptr<AssetRegistry> package_registry(GraphicsDevice& device, const fs::path& bundle) {
    auto opened = open_project(bundle / "Contents/Resources/project/project.maya");
    REQUIRE(opened);
    auto assets = open_project_assets(opened.project, std::make_unique<PackageAssetProvider>(device));
    REQUIRE(assets);
    return std::move(assets.registry);
}
/// The packaged player, launched from a copy of the bundle in another folder with no way back to the checkout.
void launch_elsewhere(const fs::path& bundle, const fs::path& folder, const std::string& expect, const std::string& scene = {}) {
    fs::create_directories(folder);
    const auto copy = folder / bundle.filename();
    fs::copy(bundle, copy, fs::copy_options::recursive);
    const auto [status, output] = run("cd " + quoted(folder) + " && env -u MAYA_RESOURCES " + quoted(copy / "Contents/MacOS/maya_player") + (scene.empty() ? std::string{} : " " + quoted(fs::path(scene))) + " --smoke 30");
    INFO(output);
    CHECK(status == 0);
    CHECK(output.find("[Player] package ") != std::string::npos);
    CHECK(output.find(expect) != std::string::npos);
    CHECK(output.find("smoke: completed 30/30 frames") != std::string::npos);
    CHECK(output.find(" 0 waited for inside a frame") != std::string::npos);
}
} // namespace

TEST_CASE("A package cooks the skins and clips its scenes reach, and its clips play as the project's do", "[package][samples]") {
    const auto sample = fs::path(MAYA_RENDER_SAMPLES) / "Models/RiggedSimple/glTF-Binary/RiggedSimple.glb";
    if (!fs::exists(sample)) SKIP("no samples; run tools/fetch_render_samples.sh");
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    fs::create_directories(project / "assets/models");
    fs::copy_file(sample, project / "assets/models/RiggedSimple.glb");
    auto opened = open_project(project);
    REQUIRE(opened);
    const auto imported = import_gltf(opened.project, "models/RiggedSimple.glb");
    REQUIRE(imported);
    const auto bundle = scratch.root / "out/Rigged.app";
    const auto report = package_project(options_for(project, bundle, {imported.scene}));
    INFO(report.error);
    REQUIRE(report);
    CHECK(report.skins == 1);
    CHECK(report.animations == 1);
    // Cooked skins and clips unwrap into what cooking the file's parts gives.
    auto cooker = AssetCooker{};
    for (const auto& record : packaged_catalog(bundle)) {
        const auto source = std::ranges::find(imported.records, record.id, &AssetRecord::id);
        const auto payload = unwrap_cooked(read_bytes(bundle / "Contents/Resources/project/content" / record.path));
        if (record.kind == AssetKind::skin) {
            REQUIRE(payload);
            CHECK(*payload == write_skin(*cooker.imported_skin(project / "assets/models/RiggedSimple.glb", split_asset_path(source->path).part).value));
        }
        if (record.kind == AssetKind::animation) {
            REQUIRE(payload);
            CHECK(*payload == write_animation(*cooker.imported_animation(project / "assets/models/RiggedSimple.glb",
                                                                         split_asset_path(source->path).part).value));
        }
    }
    // The scene played from the package and from the project: the same poses, tick for tick.
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        const auto sources = source_registry(device, project);
        const auto packaged = package_registry(device, bundle);
        const auto play = [](AssetRegistry& registry, const fs::path& scene) {
            const auto context = asset_property_context(registry);
            auto loaded = load_scene_file(scene, context);
            REQUIRE(loaded);
            auto started = PlaySession::start(std::move(loaded.document), context,
                                              play_systems(registry_script_sources(registry), registry_animation_clips(registry)));
            INFO(started.error);
            REQUIRE(started);
            auto hashes = std::vector<uint64_t>{};
            for (int frame = 0; frame < 90; ++frame) {
                const auto result = started.session->update(1.0 / 60.0);
                REQUIRE(result.error.empty());
                CHECK(result.messages.empty());
                hashes.push_back(started.session->state_hash());
            }
            return hashes;
        };
        const auto from_project = play(*sources, project / "assets" / imported.scene);
        CHECK(from_project.front() != from_project.back()); // it moved
        CHECK(play(*packaged, bundle / "Contents/Resources/project/content" / imported.scene) == from_project);
    }
    device.shutdown();
}

TEST_CASE("The packaged sample draws what the editor draws, and runs from a folder outside the checkout", "[package][gpu]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    const auto bundle = scratch.root / "out/Sample Game.app";
    REQUIRE(package_project(options_for(project, bundle, {"materials.scene"})));
    {
        test::Gpu gpu;
        const auto sources = source_registry(gpu.device, project);
        const auto packaged = package_registry(gpu.device, bundle);
        for (const auto& [scene, camera] : {std::pair{"basic.scene", EntityId{0x6d617961, 0x100}}, std::pair{"materials.scene", EntityId{0x6d617961, 0x500}}}) {
            INFO(scene);
            const auto from_sources = render_scene(gpu, *sources, project / "assets" / scene, camera, 320, 180);
            const auto from_package = render_scene(gpu, *packaged, bundle / "Contents/Resources/project/content" / scene, camera, 320, 180);
            CHECK(from_package.rgb == from_sources.rgb); // the same cooked content: the same pixels
        }
    }
    launch_elsewhere(bundle, scratch.root / "elsewhere", "Sample Game / basic.scene");
    // Inside the checkout, with its shader removed, a package does not start: it never looks outside itself,
    // even where the checkout's shader is a folder or two up, or named by MAYA_RESOURCES.
    const auto inside = fs::path(MAYA_BINARY_DIR) / ("package-inside-" + std::to_string(::getpid()));
    fs::create_directories(inside);
    fs::copy(bundle, inside / bundle.filename(), fs::copy_options::recursive);
    fs::remove(inside / bundle.filename() / "Contents/Resources/resources/shaders/metal/renderer.metal");
    const auto [status, output] = run("cd " + quoted(inside) + " && MAYA_RESOURCES=" + quoted(MAYA_SOURCE_DIR) + " " +
                                      quoted(inside / bundle.filename() / "Contents/MacOS/maya_player") + " --smoke 5");
    fs::remove_all(inside);
    INFO(output);
    CHECK(status != 0);
    CHECK(output.find("the renderer shader was not found") != std::string::npos);
}

TEST_CASE("R1 packaged draws what the editor draws, and runs from a folder outside the checkout", "[package][gpu][samples]") {
    // R1 as maya_r1 assembles it (the r1_project fixture, RGBA8 textures; docs/acceptance.md#r1), packaged
    // through its warm cook cache.
    const auto r1 = fs::path(MAYA_R1_PROJECT);
    if (!fs::is_regular_file(r1 / "r1.scene")) SKIP("R1 is not assembled; fetch the samples (tools/fetch_render_samples.sh) and run the r1_project fixture");
    const Scratch scratch;
    const auto bundle = scratch.root / "R1.app";
    const auto report = package_project(options_for(r1, bundle));
    INFO(report.error);
    REQUIRE(report);
    WARN("R1 package: " << report.bytes << " bytes, " << report.textures << " textures, in " << report.milliseconds << " ms");
    CHECK(report.skins == 1); // CesiumMan's (#1038)
    CHECK(report.animations == 2); // his walk, and the camera path and walk loop
    CHECK(report.environments == 1);
    {
        test::Gpu gpu;
        const auto sources = source_registry(gpu.device, r1);
        const auto packaged = package_registry(gpu.device, bundle);
        for (const auto& record : packaged_catalog(bundle)) {
            INFO(record.path.generic_string());
            if (record.kind == AssetKind::skin) CHECK(packaged->acquire(AssetRef<SkinAsset>{record.id}).lease.value().joints.size() == 19);
            if (record.kind == AssetKind::animation) CHECK(packaged->acquire(AssetRef<AnimationAsset>{record.id}).lease.value().duration > 1.0f);
        }
        // Each named view, as authored: the same cooked content gives the same pixels.
        for (const auto& view : r1::views()) {
            INFO(view.name);
            const auto from_sources = render_scene(gpu, *sources, r1 / "r1.scene", view.camera, 480, 270);
            const auto from_package = render_scene(gpu, *packaged, bundle / "Contents/Resources/project/content/r1.scene", view.camera, 480, 270);
            CHECK(from_package.rgb == from_sources.rgb);
        }
        // Played 300 ticks, through the path camera: CesiumMan walked and the camera turned alike.
        const auto played = [&](AssetRegistry& registry, const fs::path& scene) {
            const auto context = asset_property_context(registry);
            auto loaded = load_scene_file(scene, context);
            REQUIRE(loaded);
            auto started = PlaySession::start(std::move(loaded.document), context,
                                              play_systems(registry_script_sources(registry), registry_animation_clips(registry)));
            REQUIRE(started);
            auto& session = *started.session;
            session.clock().pause();
            while (session.clock().tick() < 300) {
                session.clock().step();
                REQUIRE(session.update(0.0).error.empty());
            }
            const auto& world = session.world();
            auto view = extract_render_view(world, *world.find(*session.camera()), 480, 270);
            REQUIRE(view);
            return gpu.render(world, registry, *view);
        };
        CHECK(played(*packaged, bundle / "Contents/Resources/project/content/r1.scene").rgb == played(*sources, r1 / "r1.scene").rgb);
    }
    launch_elsewhere(bundle, scratch.root / "elsewhere", "r1 / r1.scene");
}

TEST_CASE("The content workflow's scene runs from a standalone package outside the checkout with the same image", "[acceptance][content][gpu]") {
    // Written by maya_content_acceptance_author (content_acceptance_tests.cpp, fixture content_project).
    const auto project = fs::path(MAYA_ACCEPTANCE_DIR) / "Content Game";
    const auto scene = fs::path("levels/lit.scene");
    REQUIRE(fs::is_regular_file(project / "assets" / scene));
    const Scratch scratch;
    const auto bundle = scratch.root / "Content Game.app";
    const auto report = package_project(options_for(project, bundle, {scene}));
    INFO(report.error);
    REQUIRE(report);
    CHECK(report.environments == 1); // the workshop
    CHECK(report.meshes >= 3); // the imported model's parts, among the startup scene's
    {
        test::Gpu gpu;
        const auto sources = source_registry(gpu.device, project);
        const auto packaged = package_registry(gpu.device, bundle);
        const auto context = asset_property_context(*sources);
        auto loaded = load_scene_file(project / "assets" / scene, context);
        REQUIRE(loaded);
        const auto probe = PlaySession::start(std::move(loaded.document), context, {});
        REQUIRE(probe);
        const auto camera = *probe.session->camera();
        const auto from_sources = render_scene(gpu, *sources, project / "assets" / scene, camera, 480, 270);
        const auto from_package = render_scene(gpu, *packaged, bundle / "Contents/Resources/project/content" / scene, camera, 480, 270);
        CHECK(from_package.rgb == from_sources.rgb);
        // Something was drawn: the props under the spot light differ from the plain sky.
        CHECK(std::ranges::count(from_sources.rgb, from_sources.rgb.front()) < std::ptrdiff_t(from_sources.rgb.size() / 2));
    }
    launch_elsewhere(bundle, scratch.root / "elsewhere", "Content Game / levels/lit.scene", scene.generic_string());
}

namespace {
/// The sample's basic scene with copies of its meshes in cells far out, saved as a world (#1064).
fs::path save_sample_world(const fs::path& project) {
    auto opened = open_project(project);
    REQUIRE(opened);
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    auto assets = open_project_assets(opened.project, std::make_unique<FileAssetProvider>(device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto loaded = load_scene_file(opened.project.content_root / "basic.scene", context);
    REQUIRE(loaded);
    auto scene = loaded.document;
    auto low = uint64_t{1};
    for (const auto& entity : loaded.document.entities) {
        if (entity.parent || !std::ranges::any_of(entity.components, [](const ComponentValue& v) {
                return std::holds_alternative<MeshRendererComponent>(v);
            }))
            continue;
        for (const auto x : {200.0, 330.0, -150.0}) {
            auto copy = SceneEntity{EntityId{0x5800, low++}, std::nullopt, entity.components};
            for (auto& value : copy.components)
                if (auto* transform = std::get_if<TransformComponent>(&value)) transform->translation += math::DVec3{x, 0.0, 20.0};
            scene.entities.push_back(std::move(copy));
        }
    }
    const auto path = opened.project.content_root / "levels/sample.world";
    auto saved = save_world(path, scene, context);
    INFO((saved.empty() ? std::string() : saved.front().message));
    REQUIRE(saved.empty());
    assets.registry.reset();
    device.shutdown();
    return path;
}
} // namespace

TEST_CASE("A world packages with its cells cooked, and streams from the package as from the project", "[package][streaming]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    const auto world = save_sample_world(project);
    const auto bundle = scratch.root / "World Game.app";
    const auto report = package_project(options_for(project, bundle, {"levels/sample.world"}));
    INFO(report.error);
    REQUIRE(report);
    const auto content = bundle / "Contents/Resources/project/content";
    const auto files = files_in(content / "levels");
    CHECK(std::ranges::count(files, "sample.world") == 1);
    CHECK(std::ranges::count(files, "sample/persistent.scene") == 1);
    CHECK(std::ranges::count_if(files, [](const std::string& file) { return file.ends_with(".cell"); }) >= 3);
    CHECK(std::ranges::none_of(files, [](const std::string& file) { return file.starts_with("sample/cells/") && file.ends_with(".scene"); }));
    // From the package (cooked cells) and from the project (scene text, cooked through the cache): the same World.
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        const auto stream = [&](AssetRegistry& registry, const fs::path& file, std::shared_ptr<CookCache> cache) {
            const auto context = asset_property_context(registry);
            auto input = std::ifstream(file);
            auto read = read_world(std::string(std::istreambuf_iterator<char>(input), {}));
            REQUIRE(read);
            auto persistent = load_scene_file(file.parent_path() / read.document->persistent, context);
            REQUIRE(persistent);
            auto started = PlaySession::start(std::move(persistent.document), context, builtin_systems());
            REQUIRE(started);
            auto streamer = WorldStreamer(*read.document, cooked_cell_loader(file.parent_path(), cache, context));
            streamer.set_sources({{100.0, 0.0, 0.0}});
            const auto& stats = streamer.settle(started.session->world(), &started.session->physics());
            INFO(stats.last_error);
            CHECK(stats.cells[size_t(CellState::active)] >= 2);
            auto text = std::ostringstream{};
            REQUIRE(write_scene(text, capture_scene(started.session->world()), context).empty());
            return text.str();
        };
        const auto sources = source_registry(device, project);
        const auto packaged = package_registry(device, bundle);
        const auto from_project = stream(*sources, world, std::make_shared<CookCache>(scratch.root / "cache"));
        CHECK(stream(*packaged, content / "levels/sample.world", nullptr) == from_project);
    }
    device.shutdown();
}

TEST_CASE("The packaged player streams a world around its camera", "[package][streaming][gpu]") {
    const Scratch scratch;
    const auto project = copy_sample(scratch);
    save_sample_world(project);
    const auto bundle = scratch.root / "World Game.app";
    const auto report = package_project(options_for(project, bundle, {"levels/sample.world"}));
    INFO(report.error);
    REQUIRE(report);
    launch_elsewhere(bundle, scratch.root / "elsewhere", "[Player] streamed", "levels/sample.world");
}
