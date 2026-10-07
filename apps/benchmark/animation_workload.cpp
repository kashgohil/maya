// The animation workload, A1 (#1038, docs/performance.md#animation): copies of an imported skinned model,
// each playing its clip from a different time, under a sun that casts shadows. Each frame runs one tick,
// whose animation system time is recorded per tick, and renders one view, whose passes the GPU times; a
// matched manifest without skins gives skinning's cost per pass by difference.

#include "benchmark_detail.hpp"
#include "maya/import/gltf_import.hpp"
#include <cmath>
#include <fstream>
#include <map>

namespace maya::benchmark::detail {
namespace fs = std::filesystem;

fs::path prepare_animation(const Manifest& manifest, const fs::path& root) {
    fs::remove_all(root);
    fs::create_directories(root / "models");
    std::ofstream(root / "project.maya") << "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n";
    std::ofstream(root / "catalog.maya") << "maya-assets 1\n";
    const auto source = manifest.content / manifest.models.front();
    auto files = std::vector<fs::path>{source.filename()};
    if (source.extension() == ".gltf") {
        const auto opened = GltfFile::open(source);
        if (!opened) throw std::runtime_error(source.string() + ": " + gltf_problem_text(opened.errors.front()));
        for (const auto& named : opened.file->document().files) files.emplace_back(named);
    }
    for (const auto& file : files) {
        fs::create_directories((root / "models" / file).parent_path());
        fs::copy_file(source.parent_path() / file, root / "models" / file, fs::copy_options::overwrite_existing);
    }
    auto opened = open_project(root);
    if (!opened) throw std::runtime_error(opened.error);
    const auto imported = import_gltf(opened.project, fs::path("models") / source.filename());
    if (!imported) throw std::runtime_error(source.string() + ": " + gltf_problem_text(imported.errors.front()));
    if (std::ranges::count(imported.records, AssetKind::animation, &AssetRecord::kind) == 0)
        throw std::runtime_error(source.string() + " has no clips to play");
    return root / imported.scene;
}

SceneDocument animated_scene(const Manifest& manifest, const SceneDocument& model, float duration) {
    auto document = SceneDocument{};
    auto camera = TransformComponent{};
    camera.translation = manifest.camera_position;
    camera.rotation = looking(manifest.camera_position, manifest.camera_target);
    document.entities.push_back({camera_id, std::nullopt, {NameComponent{"Benchmark camera"}, camera, CameraComponent{}}});
    auto sun = TransformComponent{};
    sun.rotation = math::Quat(-0.5161719f, 0.2429044f, 0.0f, 0.82131857f);
    auto light = LightComponent{};
    light.cast_shadows = true; // skinned casters, through the skinned shadow pipelines
    document.entities.push_back({light_id, std::nullopt, {NameComponent{"Sun"}, sun, light}});
    const auto side = uint32_t(std::ceil(std::sqrt(double(manifest.count))));
    const auto offset = (float(side) - 1.0f) * 0.5f * manifest.spacing;
    auto next = first_instance;
    for (uint32_t i = 0; i < manifest.count; ++i) {
        auto ids = std::map<EntityId, EntityId>{};
        for (const auto& entity : model.entities) ids.emplace(entity.id, EntityId{0x616e696d, next++});
        for (const auto& entity : model.entities) {
            auto copy = SceneEntity{ids.at(entity.id), entity.parent ? std::optional(ids.at(*entity.parent)) : std::nullopt, {}};
            for (const auto& component : entity.components) {
                if (!manifest.skinning && std::holds_alternative<SkinComponent>(component)) continue;
                copy.components.push_back(component);
                if (!entity.parent) {
                    if (auto* transform = std::get_if<TransformComponent>(&copy.components.back()))
                        transform->translation = {float(i % side) * manifest.spacing - offset, 0.0f, float(i / side) * manifest.spacing - offset};
                    // Each copy from its own time, chosen by seed, so the poses differ.
                    if (auto* animation = std::get_if<AnimationComponent>(&copy.components.back()))
                        animation->start = float(double(mix(manifest.seed ^ mix(i)) % 1'000'000) / 1'000'000.0 * duration);
                }
            }
            document.entities.push_back(std::move(copy));
        }
    }
    return document;
}

} // namespace maya::benchmark::detail
