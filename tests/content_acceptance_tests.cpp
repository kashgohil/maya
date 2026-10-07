// Milestone acceptance for rendering and content (#1040, docs/acceptance.md#milestone-3-rendering-and-content):
// the workflow end to end in the editor, on a fresh copy of the sample project in build/acceptance: a glTF
// file imported, placed, and edited; a material assigned and edited; the scene lit by a sun, a spot light,
// and an environment; played; a source file changed and reloaded; saved. package_tests.cpp then packages
// the result and runs it outside the checkout (CTest maya_content_acceptance_package).

#include "editor_harness.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/property_context.hpp"
#include "support/gltf.hpp"
#include <chrono>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
const auto amber = AssetId{0x6d617961, 0x10}; // materials/amber.material

fs::path project_folder() { return fs::path(MAYA_ACCEPTANCE_DIR) / "Content Game"; }
constexpr auto scene_name = "levels/lit.scene";

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
void double_click(Harness& harness, ImVec2 at) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    for (int i = 0; i < 2; ++i) {
        harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
        harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    }
}
void type(Harness& harness, const std::string& text) {
    for (const auto c : text) harness.frame({TextEvent{uint32_t(c)}});
}
template<class T> std::optional<T> component_of(const World& world, EntityId id) {
    auto found = std::optional<T>{};
    if (const auto handle = world.find(id)) world.with<T>(*handle, [&](const T& value) { found = value; });
    return found;
}
std::string saved_text(Harness& harness) {
    auto out = std::ostringstream{};
    REQUIRE(write_scene(out, harness.shell.scene()->document(), asset_property_context(*harness.shell.assets())).empty());
    return out.str();
}
void change_file(const fs::path& path, const std::string& text, int seconds) {
    std::ofstream(path, std::ios::binary) << text;
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(seconds));
}
} // namespace

TEST_CASE("Content is imported, placed, edited, lit, played, reloaded, and saved in the editor", "[acceptance][content][author]") {
    auto error = std::error_code{};
    fs::remove_all(project_folder(), error);
    fs::create_directories(project_folder());
    fs::create_directories(fs::path(MAYA_ACCEPTANCE_DIR) / "elsewhere");
    fs::copy(sample_project().parent_path() / "assets", project_folder() / "assets", fs::copy_options::recursive);
    fs::copy_file(sample_project(), project_folder() / "project.maya");
    fs::remove_all(project_folder() / "assets/.maya", error); // an earlier run's cook cache
    // A model saved into the project by a modelling program: a glTF file and the normal map it names.
    const auto models = project_folder() / "assets/models";
    fs::create_directories(models / "textures");
    std::ofstream(models / "props.gltf", std::ios::binary) << test::props_gltf();
    std::ofstream(models / "textures/normal.png", std::ios::binary) << test::flat_normal_png();

    auto expected = std::string{};
    {
        Harness harness(false);
        REQUIRE(harness.shell.open_project(project_folder()));
        harness.frames(3);
        chord(harness, {KeyCode::LeftSuper}, KeyCode::N); // a new scene: a camera and a sun
        auto& scene = *harness.shell.scene();
        harness.frames(2);
        const auto sun = find_named(scene, "Sun");
        CHECK(component_of<LightComponent>(scene.world(), sun)->cast_shadows);

        // Import, which places the model in view, then move and turn it, and rename it.
        const auto imported = harness.shell.import_model("models/props.gltf", true);
        INFO((imported.errors.empty() ? std::string{} : gltf_problem_text(imported.errors.front())));
        REQUIRE(imported);
        harness.frames(2);
        const auto props = find_named(scene, "props");
        auto placed = *component_of<TransformComponent>(scene.world(), props);
        placed.translation = {0.0f, 0.0f, -1.0f};
        placed.rotation = math::Quat::from_axis_angle({0, 1, 0}, 0.4f);
        REQUIRE(scene.set_component(props, placed));
        REQUIRE(scene.rename(props, "Props"));
        // A material from the project on the tile, dropped on its Hierarchy row, then edited.
        const auto tile = find_named(scene, "Tile");
        drag(harness, control(harness, "asset.materials/amber.material"), row_center(harness, tile));
        CHECK(component_of<MeshRendererComponent>(scene.world(), tile)->material.id == amber);
        if (!scene.material(amber)) scene.open_material(amber, harness.shell.assets()->acquire(AssetRef<MaterialAsset>{amber}).lease.value());
        auto edited = *scene.material(amber);
        edited.roughness = 0.3f;
        edited.base_color = {0.9f, 0.45f, 0.1f, 1.0f};
        REQUIRE(scene.set_material(amber, edited, "Edit amber"));
        // Light it: a spot light from the Create menu, aimed at the props, and the workshop environment.
        press(harness, control(harness, "hierarchy.create"));
        harness.frames(2);
        press(harness, control(harness, "create.spot-light"));
        harness.frames(2);
        const auto spot = find_named(scene, "Spot light");
        REQUIRE(scene.set_component(spot, TransformComponent{{0, 3, 1}, math::Quat::from_axis_angle({1, 0, 0}, -1.2f), {1, 1, 1}}));
        double_click(harness, control(harness, "asset.environments/workshop.environment"));
        harness.frames(2);
        auto environments = 0;
        scene.world().for_each<EnvironmentComponent>([&](EntityHandle, const EnvironmentComponent& value) {
            environments += value.environment.id == AssetId{0x6d617961, 0x58};
        });
        CHECK(environments == 1);
        CHECK(harness.shell.extraction().skipped == 0);
        // Something to see in Play: the panel spins.
        const auto panel = find_named(scene, "Panel");
        REQUIRE(scene.set_component(panel, SpinComponent{{0, 1, 0}, 1.0f}));
        const auto authored = saved_text(harness);

        // Play: the panel turns in the play World only; Stop returns the scene as authored.
        REQUIRE(harness.shell.start_play());
        harness.frames(30);
        const auto* play = harness.shell.play_session();
        REQUIRE(play);
        CHECK(component_of<TransformComponent>(play->world(), panel)->rotation.w < 0.999f);
        harness.shell.stop_play();
        harness.frames(1);
        CHECK(saved_text(harness) == authored);

        // The modelling program saves a new normal map: the model is imported again, and its texture reloads.
        const auto normal = std::ranges::find_if(imported.records, [](const AssetRecord& record) {
            return record.path.generic_string() == "models/props.gltf#texture/1/normal";
        });
        REQUIRE(normal != imported.records.end());
        change_file(models / "textures/normal.png", test::encode_png_rgba(1, 1, {128, 128, 255, 255}), 5);
        harness.frames(20); // files are checked every quarter second
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "models/props.gltf changed; importing it again"));
        const auto texture = harness.shell.assets()->acquire(AssetRef<TextureAsset>{normal->id});
        REQUIRE(texture);
        CHECK(texture.lease.value().texture().desc().width == 1);
        CHECK(harness.shell.extraction().skipped == 0);
        // The edits survive the reimport: the placement, the name, and the material.
        CHECK(component_of<TransformComponent>(scene.world(), props)->translation.z == -1.0f);
        CHECK(component_of<MeshRendererComponent>(scene.world(), tile)->material.id == amber);

        // Save: the scene, and the edited material at version 2.
        chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        type(harness, "levels/lit");
        harness.frame(key(KeyCode::Enter, true));
        harness.frame(key(KeyCode::Enter, false));
        REQUIRE(harness.shell.scene_path() == fs::canonical(project_folder()) / "assets" / scene_name);
        CHECK_FALSE(scene.dirty());
        expected = saved_text(harness);
        auto material_file = std::ifstream(project_folder() / "assets/materials/amber.material");
        const auto material = read_material_file(material_file);
        REQUIRE(material);
        CHECK(material.material.roughness == 0.3f);
    }
    // Reopened in a fresh editor, the scene is as it was saved.
    Harness reopened(false);
    REQUIRE(reopened.shell.open_project(project_folder()));
    REQUIRE(reopened.shell.open_scene(scene_name));
    CHECK(saved_text(reopened) == expected);
}
