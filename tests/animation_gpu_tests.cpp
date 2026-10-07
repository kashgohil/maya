// Skeletal animation's images (#1038, docs/animation.md#reference-images): Khronos's CesiumMan and Fox,
// imported, posed by the animation system at chosen times of their clips, and skinned on the GPU, against
// blessed references in tests/references/animation. The poses themselves are checked against an
// independent evaluator in animation_tests.cpp; these images show the skinning that places the vertices.

#include "maya/assets/property_context.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/script_assets.hpp"
#include "maya/world/spatial.hpp"
#include "support/metal_view.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <unistd.h>

using namespace maya;
using namespace maya::test;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t width = 480, height = 480;
constexpr AssetId workshop{0x6d617961, 0x58};

struct Pose {
    const char* name;
    const char* model; // under the samples' Models folder
    uint32_t clip; // the file's animation
    float time; // seconds into it
    math::Vec3 from, to;
    bool skeleton = false; // the skeleton debug view over it
};
constexpr math::Vec3 man_from{1.3f, 1.15f, 2.6f}, man_at{0, 0.8f, 0}, fox_from{110, 72, 158}, fox_at{0, 30, 0};
constexpr Pose poses[] = {
    {"cesium-man-0.0", "CesiumMan/glTF-Binary/CesiumMan.glb", 0, 0.0f, man_from, man_at},
    {"cesium-man-0.5", "CesiumMan/glTF-Binary/CesiumMan.glb", 0, 0.5f, man_from, man_at},
    {"cesium-man-1.25", "CesiumMan/glTF-Binary/CesiumMan.glb", 0, 1.25f, man_from, man_at},
    {"cesium-man-skeleton-0.5", "CesiumMan/glTF-Binary/CesiumMan.glb", 0, 0.5f, man_from, man_at, true},
    {"fox-survey-1.0", "Fox/glTF-Binary/Fox.glb", 0, 1.0f, fox_from, fox_at},
    {"fox-walk-0.3", "Fox/glTF-Binary/Fox.glb", 1, 0.3f, fox_from, fox_at},
    {"fox-run-0.2", "Fox/glTF-Binary/Fox.glb", 2, 0.2f, fox_from, fox_at},
    {"fox-skeleton-run-0.2", "Fox/glTF-Binary/Fox.glb", 2, 0.2f, fox_from, fox_at, true},
};

math::Mat4 look(const math::Vec3& from, const math::Vec3& to) {
    const auto f = (to - from).normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return local_matrix(TransformComponent{from, q, {1, 1, 1}});
}

/// A project in the temporary folder with the sample project's workshop environment and the models imported.
struct AnimatedProject {
    fs::path root;
    Project project;
    std::map<std::string, GltfImportResult> imports; // by model
    AnimatedProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / ("maya-animation-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(root / "environments");
        const auto samples = fs::path(MAYA_SOURCE_DIR) / "samples/basic_scene/assets/environments";
        fs::copy_file(samples / "aerodynamics_workshop_1k.hdr", root / "environments/aerodynamics_workshop_1k.hdr");
        std::ofstream(root / "environments/workshop.environment") << "maya-environment 1\nsource \"aerodynamics_workshop_1k.hdr\"\n";
        std::ofstream(root / "project.maya") << "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n";
        std::ofstream(root / "catalog.maya") << "maya-assets 1\nenvironment 6d617961 58 \"environments/workshop.environment\"\n";
        auto opened = open_project(root);
        REQUIRE(opened);
        project = opened.project;
        for (const auto& pose : poses) {
            if (imports.contains(pose.model)) continue;
            const auto source = fs::path(MAYA_RENDER_SAMPLES) / "Models" / pose.model;
            const auto folder = root / "models" / source.stem();
            fs::create_directories(folder);
            fs::copy_file(source, folder / source.filename());
            auto imported = import_gltf(project, folder / source.filename());
            INFO(pose.model);
            REQUIRE(imported);
            imports.emplace(pose.model, std::move(imported));
        }
    }
    ~AnimatedProject() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    /// The import's clip `index`, by its catalog part.
    AssetId clip(const std::string& model, uint32_t index) const {
        for (const auto& record : imports.at(model).records)
            if (record.kind == AssetKind::animation && record.path.generic_string().ends_with("#animation/" + std::to_string(index)))
                return record.id;
        FAIL("no clip " << index << " in " << model);
        return {};
    }
};

/// Every pose: the model's imported scene with its clip held at the pose's time, after one tick of play.
std::vector<std::pair<std::string, RgbImage>> render_poses() {
    auto project = AnimatedProject{};
    Gpu gpu;
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    for (const auto& pose : poses) {
        INFO(pose.name);
        auto loaded = load_scene_file(project.project.content_root / project.imports.at(pose.model).scene, context);
        REQUIRE(loaded);
        auto held = false;
        for (auto& entity : loaded.document.entities)
            for (auto& component : entity.components)
                if (auto* animation = std::get_if<AnimationComponent>(&component)) {
                    *animation = {{project.clip(pose.model, pose.clip)}, false, true, 1.0f, pose.time};
                    held = true;
                }
        REQUIRE(held);
        auto environment = EnvironmentComponent{};
        environment.environment = {workshop};
        environment.background = false;
        loaded.document.entities.push_back({EntityId{0x5356, 1}, std::nullopt, {NameComponent{"Workshop"}, environment}});
        auto started = PlaySession::start(std::move(loaded.document), context,
                                          play_systems(registry_script_sources(*assets.registry), registry_animation_clips(*assets.registry)));
        INFO(started.error);
        REQUIRE(started);
        auto& session = *started.session;
        session.clock().pause();
        session.clock().step();
        const auto frame = session.update(0.0);
        REQUIRE(frame.ticks_run == 1);
        for (const auto& message : frame.messages) UNSCOPED_INFO(message.text);
        REQUIRE(frame.messages.empty());
        auto lens = CameraComponent{};
        lens.vertical_fov = 0.6f;
        lens.near_clip = 0.05f;
        lens.far_clip = 5000.0f;
        auto view = make_render_view(lens, look(pose.from, pose.to), width, height);
        REQUIRE(view);
        view->exposure = 1.0f;
        view->tone_mapping = ToneMapping::pbr_neutral;
        view->clear_color = {0.0, 0.0, 0.0, 1.0};
        auto debug = DebugDraw{};
        auto options = RenderExtractOptions{};
        if (pose.skeleton) {
            skeleton_debug(session.world(), *assets.registry, nullptr, debug);
            REQUIRE_FALSE(debug.empty());
            options.debug = &debug;
        }
        images.emplace_back(pose.name, gpu.render(session.world(), *assets.registry, *view, options));
    }
    return images;
}
} // namespace

TEST_CASE("CesiumMan and Fox are skinned at their clips' poses as the references show", "[visual][gpu][samples][animation]") {
    if (!fs::is_directory(fs::path(MAYA_RENDER_SAMPLES) / "Models/Fox")) SKIP("no samples; run tools/fetch_render_samples.sh");
    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/animation",
                            fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs", render_poses());
}
