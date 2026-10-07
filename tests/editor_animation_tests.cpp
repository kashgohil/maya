// Skeletal animation in the editor (#1038, docs/editor.md#animation): an imported model plays its clip in
// Play as the player plays it, Stop returns its joints to their authored poses, the Inspector and Assets
// panel edit skins and clips, and the skeleton debug view draws its bones.
#include "editor_harness.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/script_assets.hpp"
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
/// Canonical text of a World: equal text means equal IDs, hierarchy, and values.
std::string text(const World& world) {
    auto out = std::ostringstream{};
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    REQUIRE(write_scene(out, capture_scene(world), any_asset).empty());
    return out.str();
}
TransformComponent transform_in(const World& world, EntityId id) {
    auto value = read_component(world, *world.find(id), ComponentId::transform);
    REQUIRE(value);
    return std::get<TransformComponent>(*value);
}

/// The sample project with CesiumMan imported, and its scene open.
struct CesiumProject {
    ProjectCopy copy;
    Harness harness{false};
    GltfImportResult imported;
    EntityId root, mesh, joint; // the import's root, the skinned mesh, and its first spine joint
    CesiumProject() {
        const auto sample = fs::path(MAYA_RENDER_SAMPLES) / "Models/CesiumMan/glTF-Binary/CesiumMan.glb";
        fs::create_directories(copy.content / "models");
        fs::copy_file(sample, copy.content / "models/CesiumMan.glb");
        REQUIRE(harness.shell.open_project(copy.folder));
        harness.frames(2);
        imported = harness.shell.import_model("models/CesiumMan.glb", false);
        REQUIRE(imported);
        REQUIRE(harness.shell.open_scene(imported.scene));
        harness.frames(2);
        auto& scene = *harness.shell.scene();
        root = find_named(scene, "CesiumMan");
        mesh = find_named(scene, "Cesium_Man");
        joint = find_named(scene, "Skeleton_torso_joint_2");
    }
    AssetId clip() const {
        for (const auto& record : imported.records)
            if (record.kind == AssetKind::animation) return record.id;
        FAIL("no clip");
        return {};
    }
    AssetId skin() const {
        for (const auto& record : imported.records)
            if (record.kind == AssetKind::skin) return record.id;
        FAIL("no skin");
        return {};
    }
};
bool samples() { return fs::exists(fs::path(MAYA_RENDER_SAMPLES) / "Models/CesiumMan/glTF-Binary/CesiumMan.glb"); }
} // namespace

TEST_CASE("An imported model's clip plays in Play as in the player, and Stop returns its joints to their authored poses",
          "[editor][animation][samples]") {
    if (!samples()) SKIP("no samples; run tools/fetch_render_samples.sh");
    auto project = CesiumProject{};
    auto& harness = project.harness;
    auto& scene = *harness.shell.scene();
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "1 skins, 1 clips, scene models/CesiumMan.scene"));
    const auto authored = text(scene.world());
    const auto authored_joint = transform_in(scene.world(), project.joint);
    REQUIRE(harness.shell.start_play());
    auto* play = harness.shell.play_session();
    REQUIRE(play);
    harness.frames(45);
    CHECK(play->clock().tick() == 45);
    const auto moved = transform_in(play->world(), project.joint);
    CHECK((moved.rotation.x != authored_joint.rotation.x || moved.rotation.w != authored_joint.rotation.w));
    CHECK(text(scene.world()) == authored); // the authored joints never move
    // The player's path: the same scene file, straight into a play session, gives the same World tick for tick.
    {
        const auto context = asset_property_context(*harness.shell.assets());
        auto loaded = load_scene_file(project.copy.content / project.imported.scene, context);
        REQUIRE(loaded);
        auto player = PlaySession::start(std::move(loaded.document), context,
                                         play_systems(registry_script_sources(*harness.shell.assets()),
                                                      registry_animation_clips(*harness.shell.assets())));
        REQUIRE(player);
        while (player.session->clock().tick() < play->clock().tick()) player.session->update(1.0 / 60.0);
        CHECK(text(player.session->world()) == text(play->world()));
        CHECK(player.session->state_hash() == play->state_hash());
    }
    // Nothing to report: every joint was found.
    CHECK_FALSE(logged(harness.shell.diagnostics(), DiagnosticSource::play, "Animation"));
    harness.shell.stop_play();
    harness.frames(1);
    CHECK_FALSE(harness.shell.play_session());
    CHECK(text(scene.world()) == authored);
    const auto back = transform_in(scene.world(), project.joint);
    CHECK(back.rotation.x == authored_joint.rotation.x);
    CHECK(back.rotation.w == authored_joint.rotation.w);
    CHECK(back.translation.y == authored_joint.translation.y);
}

TEST_CASE("The Inspector edits skins and clips, a clip dropped on a model plays on its root, and renaming a joint is reported in Play",
          "[editor][animation][samples]") {
    if (!samples()) SKIP("no samples; run tools/fetch_render_samples.sh");
    auto project = CesiumProject{};
    auto& harness = project.harness;
    auto& scene = *harness.shell.scene();
    // The root's Animation component and the mesh's Skin, each with its asset field.
    scene.select(project.root);
    harness.frames(2);
    for (const auto* field : {"animation.clip", "animation.playing", "animation.loop", "animation.speed", "animation.start"}) {
        INFO(field);
        CHECK(harness.shell.layout().field(field));
    }
    scene.select(project.mesh);
    harness.frames(2);
    CHECK(harness.shell.layout().field("skin.skin"));
    // The Assets panel lists the skin and clip under Animation, by the file's names.
    const auto label = [&](AssetId id) {
        const auto info = harness.shell.assets()->info(id);
        REQUIRE(info);
        return harness.shell.asset_label(info->record);
    };
    CHECK(label(project.clip()) == "CesiumMan clip 0"); // the file does not name it
    CHECK(label(project.skin()) == "Armature");
    // Assigning the clip to the mesh plays it on the root, which names the joints, keeping how it plays.
    REQUIRE(scene.set_component(project.root, AnimationComponent{{}, true, false, 0.5f, 0.25f}));
    REQUIRE(harness.shell.assign_asset(project.mesh, project.clip()));
    auto animation = AnimationComponent{};
    scene.world().with<AnimationComponent>(*scene.world().find(project.root), [&](const AnimationComponent& value) { animation = value; });
    CHECK(animation.clip.id == project.clip());
    CHECK(animation.speed == 0.5f);
    CHECK(animation.start == 0.25f);
    CHECK_FALSE(scene.world().has<AnimationComponent>(*scene.world().find(project.mesh)));
    // A joint renamed in the scene: Play reports the channels it leaves behind, and the mesh is drawn unskinned.
    REQUIRE(scene.rename(project.joint, "Spine"));
    harness.frames(1);
    CHECK(harness.shell.extraction().skipped == 0); // drawn, as authored
    REQUIRE(harness.shell.start_play());
    harness.frames(3);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play,
                 "Animation: 'CesiumMan' has no entity at 'Z_UP/Armature/Skeleton_torso_joint_1/Skeleton_torso_joint_2', which its clip"));
    harness.shell.stop_play();
}

TEST_CASE("The skeleton debug view draws bones in the editor and in Play, and is kept as a preference", "[editor][animation][samples]") {
    if (!samples()) SKIP("no samples; run tools/fetch_render_samples.sh");
    auto project = CesiumProject{};
    auto& harness = project.harness;
    harness.frames(2);
    CHECK(harness.shell.renderer_stats().debug_lines == 0);
    CHECK_FALSE(harness.shell.skeletons());
    press(harness, control(harness, "tool.physics-debug"));
    harness.frames(2); // the menu sizes itself on its first frame
    press(harness, control(harness, "debug.skeletons"));
    harness.frame(key(KeyCode::Escape, true));
    harness.frame(key(KeyCode::Escape, false));
    CHECK(harness.shell.skeletons());
    const auto before = harness.shell.renderer_stats().debug_lines;
    harness.frames(2);
    // 18 bones and three axes for each of 19 joints, each frame.
    CHECK(harness.shell.renderer_stats().debug_lines - before == 2 * (18 + 3 * 19));
    REQUIRE(harness.shell.start_play());
    const auto playing = harness.shell.renderer_stats().debug_lines;
    harness.frames(2);
    CHECK(harness.shell.renderer_stats().debug_lines - playing == 2 * (18 + 3 * 19));
    harness.shell.stop_play();
    // Saved with the editor's preferences, and read back.
    auto out = std::ostringstream{};
    write_preferences(out, EditorPreferences{.skeletons = true});
    CHECK(out.str().find("skeletons on\n") != std::string::npos);
    auto in = std::istringstream(out.str());
    const auto read = read_preferences(in);
    CHECK(read.error.empty());
    CHECK(read.preferences.skeletons);
}
