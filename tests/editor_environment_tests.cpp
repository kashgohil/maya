// Environments in the editor (#1035, docs/editor.md#environments): the Assets panel lists them, using
// one lights the scene through its Environment component, and changed files reload.

#include "editor_harness.hpp"
#include <chrono>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
constexpr auto workshop = AssetId{0x6d617961, 0x58}, sky = AssetId{0x6d617961, 0x59};

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
/// The open scene's Environment components, by entity.
std::vector<std::pair<EntityId, EnvironmentComponent>> environments(SceneEditor& scene) {
    auto found = std::vector<std::pair<EntityId, EnvironmentComponent>>{};
    scene.world().for_each<EnvironmentComponent>([&](EntityHandle entity, const EnvironmentComponent& value) {
        found.emplace_back(*scene.world().persistent_id(entity), value);
    });
    return found;
}
} // namespace

TEST_CASE("Using an environment from the Assets panel lights the scene through one Environment component, as one undo step", "[editor][environments]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    auto& scene = *harness.shell.scene();
    harness.frames(2);
    REQUIRE(environments(scene).empty()); // the basic scene has none
    // Double-click: a new entity named Environment, with the sky.
    double_click(harness, control(harness, "asset.environments/sky.environment"));
    harness.frames(1);
    auto found = environments(scene);
    REQUIRE(found.size() == 1);
    CHECK(found[0].second.environment.id == sky);
    CHECK(scene.display_name(found[0].first) == "Environment");
    CHECK(harness.shell.extraction().skipped == 0);
    // Dropped in the viewport, the workshop replaces it in the same component; nothing else changes.
    harness.shell.scene()->set_selection({});
    drag(harness, control(harness, "asset.environments/workshop.environment"), harness.viewport_center());
    harness.frames(1);
    found = environments(scene);
    REQUIRE(found.size() == 1);
    CHECK(found[0].second.environment.id == workshop);
    CHECK(scene.undo_label() == "Edit Environment");
    REQUIRE(scene.undo());
    CHECK(environments(scene)[0].second.environment.id == sky);
    REQUIRE(scene.undo());
    CHECK(environments(scene).empty());
    // The renderer lights the frames with it.
    REQUIRE(scene.redo());
    harness.frames(2);
    CHECK(harness.shell.assets()->info(sky)->state == AssetState::ready);
    // The Inspector edits it like any component: its field takes environments only.
    scene.select(environments(scene)[0].first);
    harness.frames(2);
    CHECK(harness.shell.layout().field("environment.environment"));
    CHECK(harness.shell.layout().field("environment.intensity"));
    CHECK(harness.shell.layout().field("environment.rotation"));
}

TEST_CASE("The material scene is lit by its environment, and a changed environment file or source image reloads by itself", "[editor][environments]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.shell.request_open_scene("materials.scene");
    harness.frames(3);
    REQUIRE(harness.shell.scene_path() == copy.content / "materials.scene");
    REQUIRE(harness.shell.assets()->info(sky)->state == AssetState::ready);
    const auto before = harness.shell.assets()->info(sky)->generation;
    harness.check_files(); // the loaded environment's file, as it was loaded
    // A smaller cube written by another program: reloaded, a new version, and logged.
    copy.write("environments/sky.environment", "maya-environment 1\nsource \"kloofendal_48d_partly_cloudy_puresky_1k.hdr\"\nspecular_size 32\n");
    fs::last_write_time(copy.content / "environments/sky.environment", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    harness.check_files();
    CHECK(harness.shell.assets()->info(sky)->generation == before + 1);
    CHECK(harness.shell.assets()->acquire(AssetRef<EnvironmentAsset>{sky}).lease.value().specular().desc().width == 32);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Reloaded environments/sky.environment"));
    // Its source image replaced by another program: cooked again from the new image.
    fs::copy_file(copy.content / "environments/aerodynamics_workshop_1k.hdr",
                  copy.content / "environments/kloofendal_48d_partly_cloudy_puresky_1k.hdr", fs::copy_options::overwrite_existing);
    fs::last_write_time(copy.content / "environments/kloofendal_48d_partly_cloudy_puresky_1k.hdr",
                        fs::file_time_type::clock::now() + std::chrono::seconds(7));
    const auto sky_before = harness.shell.assets()->acquire(AssetRef<EnvironmentAsset>{sky}).lease.value().irradiance()[0];
    harness.check_files();
    CHECK(harness.shell.assets()->info(sky)->generation == before + 2);
    const auto sky_after = harness.shell.assets()->acquire(AssetRef<EnvironmentAsset>{sky}).lease.value().irradiance()[0];
    CHECK((sky_after - sky_before).length() > 0.01f); // the workshop's light, not the sky's
    // A broken file keeps the last good version.
    copy.write("environments/sky.environment", "maya-environment 1\n");
    fs::last_write_time(copy.content / "environments/sky.environment", fs::file_time_type::clock::now() + std::chrono::seconds(9));
    harness.check_files();
    CHECK(harness.shell.assets()->info(sky)->generation == before + 2);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "names no source; the last version stays in use"));
    harness.frames(2);
    CHECK(harness.shell.extraction().skipped == 0);
}
