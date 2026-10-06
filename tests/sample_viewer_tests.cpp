// The visual bar's material check (#1036, docs/import.md#the-sample-viewer-comparison): Khronos's
// MetalRoughSpheres and NormalTangentMirrorTest, imported and rendered by Maya, against the Khronos glTF
// Sample Viewer's renders of the same views, under the same environment, exposure, and tone mapper.
// The Sample Viewer's images are in tests/references/sample-viewer; tools/sample_viewer/ captures them.

#include "maya/assets/property_context.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/scene/scene_io.hpp"
#include "support/metal_view.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include "maya/world/spatial.hpp"
#include <unistd.h>

using namespace maya;
using namespace maya::test;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t width = 640, height = 480;
constexpr AssetId workshop{0x6d617961, 0x58};

struct SampleView {
    const char* name;
    const char* model; // under the samples' Models folder
    math::Vec3 from, to;
    float vertical_fov;
};
// The same views as tools/sample_viewer/views.json.
constexpr SampleView views[] = {
    {"metal-rough-spheres-front", "MetalRoughSpheres/glTF/MetalRoughSpheres.gltf", {0, 0, 15}, {0, -0.25f, -1.5f}, 0.7f},
    {"metal-rough-spheres-angled", "MetalRoughSpheres/glTF/MetalRoughSpheres.gltf", {9, 5, 11}, {0, -0.25f, -1.5f}, 0.7f},
    {"normal-tangent-mirror-front", "NormalTangentMirrorTest/glTF/NormalTangentMirrorTest.gltf", {0, 0, 3.6f}, {0, -0.08f, 0}, 0.8f},
    {"normal-tangent-mirror-angled", "NormalTangentMirrorTest/glTF/NormalTangentMirrorTest.gltf", {2.2f, 1.2f, 2.8f}, {0, -0.08f, 0}, 0.8f},
};

math::Mat4 pose(const math::Vec3& from, const math::Vec3& to) {
    const auto f = (to - from).normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return local_matrix(TransformComponent{from, q, {1, 1, 1}});
}

/// A project in the temporary folder with the sample project's workshop environment, and the models imported.
struct SampleProject {
    fs::path root;
    Project project;
    std::map<std::string, fs::path> scenes; // by model, content-relative
    SampleProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / ("maya-sample-viewer-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(root / "environments");
        const auto samples = fs::path(MAYA_SOURCE_DIR) / "samples/basic_scene/assets/environments";
        fs::copy_file(samples / "aerodynamics_workshop_1k.hdr", root / "environments/aerodynamics_workshop_1k.hdr");
        // Sharper than the sample project's (128): mirror reflections show the environment's detail.
        std::ofstream(root / "environments/workshop.environment")
            << "maya-environment 1\nsource \"aerodynamics_workshop_1k.hdr\"\nspecular_size 256\nsamples 512\n";
        std::ofstream(root / "project.maya") << "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n";
        std::ofstream(root / "catalog.maya") << "maya-assets 1\nenvironment 6d617961 58 \"environments/workshop.environment\"\n";
        auto opened = open_project(root);
        REQUIRE(opened);
        project = opened.project;
        for (const auto& view : views) {
            if (scenes.contains(view.model)) continue;
            const auto source = fs::path(MAYA_RENDER_SAMPLES) / "Models" / view.model;
            const auto folder = root / "models" / source.stem();
            fs::create_directories(folder);
            fs::copy_file(source, folder / source.filename());
            const auto file = GltfFile::open(source);
            REQUIRE(file);
            for (const auto& named : file.file->document().files) {
                fs::create_directories((folder / named).parent_path());
                fs::copy_file(source.parent_path() / named, folder / named);
            }
            const auto imported = import_gltf(project, folder / source.filename());
            INFO(view.model);
            REQUIRE(imported);
            scenes.emplace(view.model, imported.scene);
        }
    }
    ~SampleProject() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

/// Renders every view: the model's imported scene, lit only by the workshop (turned by `rotation`, its
/// sky drawn or not), at exposure 1 (the Sample Viewer's default) with Khronos PBR Neutral.
std::vector<std::pair<std::string, RgbImage>> render_views(float rotation, bool sky) {
    auto project = SampleProject{};
    Gpu gpu;
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    for (const auto& view : views) {
        INFO(view.name);
        auto loaded = load_scene_file(project.project.content_root / project.scenes.at(view.model), context);
        REQUIRE(loaded);
        auto environment = EnvironmentComponent{};
        environment.environment = {workshop};
        environment.rotation = rotation;
        environment.background = sky;
        loaded.document.entities.push_back({EntityId{0x5356, 1}, std::nullopt, {NameComponent{"Workshop"}, environment}});
        auto world = instantiate_scene(std::move(loaded.document), context);
        REQUIRE(world);
        auto lens = CameraComponent{};
        lens.vertical_fov = view.vertical_fov;
        lens.near_clip = 0.05f;
        auto render = make_render_view(lens, pose(view.from, view.to), width, height);
        REQUIRE(render);
        render->exposure = 1.0f;
        render->tone_mapping = ToneMapping::pbr_neutral;
        render->clear_color = {0.0, 0.0, 0.0, 1.0};
        images.emplace_back(view.name, gpu.render(*world.world, *assets.registry, *render));
    }
    return images;
}

/// The declared tolerance (docs/import.md#the-sample-viewer-comparison), over the pixels either renderer
/// draws the model on: the mean of each pixel's largest channel difference at most 9 (of 255); at least
/// 92% of them within 16 and at most 2% beyond 48 (reflections of the environment's fine detail, which
/// each renderer prefilters its own way); and no channel brighter or darker overall by more than 5.
struct Agreement {
    double mean = 0, within_16 = 0, beyond_48 = 0;
    std::array<double, 3> bias{};
    bool passes() const {
        return mean <= 9 && within_16 >= 0.92 && beyond_48 <= 0.02 && std::ranges::all_of(bias, [](double b) { return std::abs(b) <= 5; });
    }
};
Agreement agreement(const RgbImage& maya, const RgbImage& reference) {
    auto result = Agreement{};
    size_t drawn = 0;
    for (size_t i = 0; i < maya.rgb.size(); i += 3) {
        const auto any = [&](const RgbImage& image) { return image.rgb[i] || image.rgb[i + 1] || image.rgb[i + 2]; };
        if (!any(maya) && !any(reference)) continue; // both cleared to black: no model there
        ++drawn;
        auto largest = 0;
        for (size_t c = 0; c < 3; ++c) {
            const auto difference = int(maya.rgb[i + c]) - int(reference.rgb[i + c]);
            largest = std::max(largest, std::abs(difference));
            result.bias[c] += difference;
        }
        result.mean += largest;
        result.within_16 += largest <= 16;
        result.beyond_48 += largest > 48;
    }
    REQUIRE(drawn > 0);
    result.mean /= double(drawn);
    result.within_16 /= double(drawn);
    result.beyond_48 /= double(drawn);
    for (auto& b : result.bias) b /= double(drawn);
    return result;
}
} // namespace

TEST_CASE("MetalRoughSpheres and NormalTangentMirrorTest render as the Khronos glTF Sample Viewer renders them", "[visual][gpu][samples]") {
    if (!fs::is_directory(fs::path(MAYA_RENDER_SAMPLES) / "Models")) SKIP("no samples; run tools/fetch_render_samples.sh");
    const auto references = fs::path(MAYA_SOURCE_DIR) / "tests/references/sample-viewer";
    const auto diffs = fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs";
    for (const auto& [name, image] : render_views(0.0f, false)) {
        INFO(name);
        const auto reference = read_png(references / (name + ".png"));
        REQUIRE(reference);
        REQUIRE(reference->width == image.width);
        REQUIRE(reference->height == image.height);
        const auto result = agreement(image, *reference);
        INFO("mean " << result.mean << ", within 16 " << result.within_16 * 100 << "%, beyond 48 " << result.beyond_48 * 100
             << "%, bias " << result.bias[0] << " " << result.bias[1] << " " << result.bias[2]);
        if (!result.passes()) {
            fs::create_directories(diffs);
            write_png(diffs / (name + ".maya.png"), image);
        }
        CHECK(result.passes());
    }
}

// Maya's renders of the same views, for review beside the Sample Viewer's (tools/sample_viewer/capture.sh):
//   MAYA_SAMPLE_VIEWER_OUT=<folder> maya_editor_tests "[sample-viewer-renders]"
TEST_CASE("Maya's renders of the Sample Viewer views", "[.sample-viewer-renders]") {
    const auto* out = std::getenv("MAYA_SAMPLE_VIEWER_OUT");
    REQUIRE(out);
    for (const auto& [name, image] : render_views(0.0f, std::getenv("MAYA_SAMPLE_VIEWER_SKY") != nullptr))
        REQUIRE(write_png(fs::path(out) / (name + ".maya.png"), image));
}
