// Milestone acceptance for physics and behavior (#1024, docs/acceptance.md#milestone-2-physics-and-behavior):
// the reference interaction authored in the editor, played, edited, replayed, reset, and broken, then
// run by the real maya_player from another directory (CTest maya_physics_acceptance_player). A ball
// rolls down a ramp into a stack of crates; a trigger zone's script opens a kinematic door when
// something enters.

#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/script_assets.hpp"
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;

namespace {
const auto cube_mesh = AssetId{0x6d617961, 2};
const auto amber = AssetId{0x6d617961, 0x10};
const auto red = AssetId{0x6d617961, 0x11};
const auto ground = AssetId{0x6d617961, 0x13};
const auto door_script = AssetId{0x6d617961, 0x30};
constexpr auto scene_name = "levels/door.scene";

fs::path project_folder() { return fs::path(MAYA_ACCEPTANCE_DIR) / "Physics Game"; }

/// The zone's script, first version: the door rises by `lift` metres once something enters.
const auto rising = R"(local Door = {
    properties = {
        door = { type = "entity", label = "Door" },
        lift = { type = "number", default = 2, min = 0, max = 10, unit = "m", label = "Lift" },
    },
}
function Door:start()
    self.moving = false
end
function Door:on_trigger_enter(other, contact)
    if self.moving or self.door == nil then return end
    self.moving = true
    self.base = self.door:position()
    maya.log("the door rises for " .. other:name())
end
function Door:fixed_update(dt)
    if not self.moving then return end
    local p = self.door:position()
    local top = self.base.y + self.lift
    if p.y < top then self.door:move_kinematic(vector.create(p.x, math.min(p.y + 3 * dt, top), p.z)) end
end
return Door
)";
/// Second version, edited while the editor is open: the door slides aside instead.
const auto sliding = R"(local Door = {
    properties = {
        door = { type = "entity", label = "Door" },
        lift = { type = "number", default = 2, min = 0, max = 10, unit = "m", label = "Lift" },
    },
}
function Door:start()
    self.moving = false
end
function Door:on_trigger_enter(other, contact)
    if self.moving or self.door == nil then return end
    self.moving = true
    self.base = self.door:position()
    maya.log("the door slides open for " .. other:name())
end
function Door:fixed_update(dt)
    if not self.moving then return end
    local p = self.door:position()
    local side = self.base.z + self.lift
    if p.z < side then self.door:move_kinematic(vector.create(p.x, p.y, math.min(p.z + 3 * dt, side))) end
end
return Door
)";

TransformComponent placed(math::Vec3 at, math::Vec3 scale = math::Vec3(1.0f), math::Quat rotation = {}) {
    auto transform = TransformComponent{};
    transform.translation = at;
    transform.scale = scale;
    transform.rotation = rotation;
    return transform;
}
MeshRendererComponent cube(AssetId material) { return {AssetRef<MeshAsset>{cube_mesh}, AssetRef<MaterialAsset>{material}, true}; }
ColliderComponent collider(ColliderShape shape = ColliderShape::box) {
    auto value = ColliderComponent{};
    value.shape = shape;
    return value;
}

std::optional<ScriptComponent> script_of(SceneEditor& scene, EntityId id) {
    const auto value = read_component(scene.world(), *scene.world().find(id), ComponentId::script);
    return value ? std::optional(std::get<ScriptComponent>(*value)) : std::nullopt;
}
math::Vec3 position_in(const World& world, EntityId id) {
    const auto handle = world.find(id);
    REQUIRE(handle);
    auto position = math::Vec3(0.0f);
    world.with<TransformComponent>(*handle, [&](const TransformComponent& t) { position = t.translation; });
    return position;
}
std::string scene_text(Harness& harness) {
    auto out = std::ostringstream{};
    REQUIRE(write_scene(out, harness.shell.scene()->document(), asset_property_context(*harness.shell.assets())).empty());
    return out.str();
}
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
/// Plays until `done` or `ticks` ticks pass, at the editor's frame rate.
void play_until(Harness& harness, uint64_t ticks, const std::function<bool()>& done) {
    REQUIRE(harness.shell.play_session());
    while (harness.shell.play_session()->clock().tick() < ticks && !done()) {
        harness.frame();
        REQUIRE(harness.shell.play_session());
    }
}
size_t logs(const DiagnosticLog& log, std::string_view text) {
    return size_t(std::ranges::count_if(log.entries(), [&](const DiagnosticEntry& e) {
        return e.source == DiagnosticSource::script && e.message.find(text) != std::string::npos;
    }));
}
} // namespace

TEST_CASE("The reference interaction is authored, played, edited, replayed, reset, and broken in the editor", "[acceptance][physics][author]") {
    // A fresh copy of the sample project, kept for the player run.
    auto error = std::error_code{};
    fs::remove_all(project_folder(), error);
    fs::create_directories(project_folder());
    fs::copy_file(sample_project(), project_folder() / "project.maya");
    fs::copy(sample_project().parent_path() / "assets", project_folder() / "assets", fs::copy_options::recursive);
    const auto content = project_folder() / "assets";
    REQUIRE(fs::exists(content / "catalog.maya"));
    // The script, written as a person would in a text editor, and listed in the catalog.
    std::ofstream(content / "scripts/door.luau") << rising;
    std::ofstream(content / "catalog.maya", std::ios::app) << "script 6d617961 30 \"scripts/door.luau\"\n";

    auto ids = std::map<std::string, EntityId>{};
    {
        Harness harness(false);
        REQUIRE(harness.shell.open_project(project_folder()));
        REQUIRE(harness.shell.new_scene());
        harness.frames(2);
        auto& scene = *harness.shell.scene();
        // Floor, a ramp tilted down toward the crates, the ball at its top, a stack of three crates,
        // the zone past them, and the door beyond.
        const auto create = [&](const std::string& name, std::vector<ComponentValue> components) {
            const auto result = scene.create(name, std::nullopt, std::move(components));
            INFO(name << ": " << result.error);
            REQUIRE(result);
            ids[name] = find_named(scene, name);
        };
        create("Floor", {placed({0, -0.5f, 0}, {30, 1, 12}), cube(ground), collider()});
        create("Ramp", {placed({-5, 2.0f, 0}, {8, 0.4f, 3}, math::Quat::from_axis_angle({0, 0, 1}, -0.5f)), cube(ground), collider()});
        auto ball = collider(ColliderShape::sphere);
        ball.radius = 0.4f;
        auto rolling = RigidBodyComponent{};
        rolling.density = 20000.0f; // a heavy ball, so the crates topple
        create("Ball", {placed({-8, 4.6f, 0}), ball, rolling});
        auto light = RigidBodyComponent{};
        light.density = 200.0f;
        for (const auto i : {0, 1, 2})
            create("Crate " + std::to_string(i + 1), {placed({1.5f, 0.5f + float(i), 0}), cube(amber), collider(), light});
        // The zone starts 0.3 m past the stack, so nothing is in it at the start.
        auto zone = collider();
        zone.sensor = true;
        create("Zone", {placed({3.8f, 1, 0}, {3, 2, 6}), zone});
        // A moving body has unit scale: its collider is sized, and a scaled child carries the mesh.
        auto door = RigidBodyComponent{};
        door.motion = BodyMotion::kinematic;
        auto slab = collider();
        slab.half_extents = {0.2f, 1.5f, 2.0f};
        create("Door", {placed({8, 1.5f, 0}), slab, door});
        const auto panel = scene.create("Door panel", ids["Door"], {placed({0, 0, 0}, {0.4f, 3, 4}), cube(red)});
        INFO(panel.error);
        REQUIRE(panel);

        // The script, dragged from the Assets panel onto the zone's Hierarchy row.
        harness.frames(2);
        drag(harness, control(harness, "asset.scripts/door.luau"), row_center(harness, ids["Zone"]));
        REQUIRE(script_of(scene, ids["Zone"]));
        CHECK(script_of(scene, ids["Zone"])->script.id == door_script);
        // Its door, then its lift edited in the Inspector by dragging the field.
        auto attached = *script_of(scene, ids["Zone"]);
        attached.values = {{"door", ScriptValueType::entity, ids["Door"]}, {"lift", ScriptValueType::number, 2.0f}};
        REQUIRE(scene.set_component(ids["Zone"], attached));
        scene.select(ids["Zone"]);
        harness.frames(3);
        // The script's fields are below the transform and collider: scroll the Inspector to them.
        const auto* lift = harness.shell.layout().field("script.lift");
        REQUIRE(lift);
        for (auto last = -1.0f; lift->max.y != last;) { // until the Inspector is scrolled to its end
            last = lift->max.y;
            harness.frame({MouseMoveEvent{lift->min.x + 10.0f, 200.0f}, ScrollEvent{0.0f, -2.0f}});
            harness.frame();
            lift = harness.shell.layout().field("script.lift");
            REQUIRE(lift);
        }
        const auto from = ImVec2{(lift->min.x + lift->max.x) / 2, (lift->min.y + lift->max.y) / 2};
        drag(harness, from, {from.x + 40.0f, from.y}, 10);
        const auto edited = std::get<float>(std::ranges::find(script_of(scene, ids["Zone"])->values, "lift", &ScriptValue::name)->data);
        CHECK(edited > 2.0f);
        CHECK(scene.undo_label() == "Edit Script");
        REQUIRE(harness.shell.save_scene(scene_name).empty());
        CHECK_FALSE(scene.dirty());
    }

    // Reopened in a fresh editor, the scene is as it was saved.
    Harness harness(false);
    REQUIRE(harness.shell.open_project(project_folder()));
    REQUIRE(harness.shell.open_scene(scene_name));
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    for (const auto& [name, id] : ids) CHECK(find_named(scene, name) == id);
    const auto authored = scene_text(harness);
    const auto lift = std::get<float>(std::ranges::find(script_of(scene, ids["Zone"])->values, "lift", &ScriptValue::name)->data);
    const auto door_at = position_in(scene.world(), ids["Door"]);

    // Play: the ball rolls down, the crates fall into the zone, and the door rises by the edited lift.
    INFO(harness.shell.prompt_message());
    REQUIRE(harness.shell.start_play(true));
    harness.shell.set_game_view(false);
    play_until(harness, 900, [&] {
        return position_in(harness.shell.play_session()->world(), ids["Door"]).y >= door_at.y + lift - 1e-3f;
    });
    const auto opened_at = harness.shell.play_session()->clock().tick();
    INFO("the door opened at tick " << opened_at);
    CHECK(logs(harness.shell.diagnostics(), "the door rises for") == 1);
    CHECK(position_in(harness.shell.play_session()->world(), ids["Door"]).y == Approx(door_at.y + lift).margin(1e-3));
    CHECK(position_in(harness.shell.play_session()->world(), ids["Ball"]).x > -2.0f); // it left the ramp
    play_until(harness, opened_at + 60, [] { return false; });
    harness.shell.stop_play();
    harness.frames(2);
    CHECK(scene_text(harness) == authored); // Stop returns to the authored scene
    const auto first_recording = harness.shell.last_recording();
    REQUIRE(first_recording);

    // The script's behavior is edited on disk while the editor is open: now the door slides aside.
    std::ofstream(content / "scripts/door.luau") << sliding;
    harness.shell.check_script_files();
    harness.frames(2);
    // A recording made with the old script is refused, not replayed differently.
    CHECK_FALSE(harness.shell.replay_last_play());
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play, "Couldn't replay: scripts/door.luau has changed since the recording"));
    harness.frames(2);

    REQUIRE(harness.shell.start_play(true));
    harness.shell.set_game_view(false);
    play_until(harness, 900, [&] {
        return position_in(harness.shell.play_session()->world(), ids["Door"]).z >= door_at.z + lift - 1e-3f;
    });
    CHECK(logs(harness.shell.diagnostics(), "the door slides open for") == 1);
    const auto slid = position_in(harness.shell.play_session()->world(), ids["Door"]);
    CHECK(slid.z == Approx(door_at.z + lift).margin(1e-3));
    CHECK(slid.y == Approx(door_at.y).margin(1e-3)); // no longer rises
    harness.shell.stop_play();
    harness.frames(2);
    CHECK(scene_text(harness) == authored);

    // Replaying that Play repeats it exactly.
    REQUIRE(harness.shell.replay_last_play());
    for (int i = 0; i < 2000 && !harness.shell.play_session()->replay().finished; ++i) harness.frame();
    harness.frame();
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::play, "The replay matches the recording"));
    harness.shell.stop_play();
    harness.frames(2);

    // Repeated Play and Stop return to the authored scene and the empty-session baseline.
    REQUIRE(harness.shell.start_play());
    harness.frames(5);
    harness.shell.stop_play();
    harness.frames(4);
    const auto before = harness.device.stats();
    const auto physics_before = physics_memory().live_bytes;
    for (int round = 0; round < 25; ++round) {
        REQUIRE(harness.shell.start_play());
        harness.frames(20);
        harness.shell.stop_play();
        harness.frames(1);
        REQUIRE(script_memory_in_use() == 0);
    }
    harness.frames(4);
    CHECK(harness.device.stats().buffers == before.buffers);
    CHECK(harness.device.stats().textures == before.textures);
    CHECK(harness.device.stats().pending_retirements == before.pending_retirements);
    CHECK(physics_memory().live_bytes == physics_before);
    CHECK(scene_text(harness) == authored);
    CHECK_FALSE(scene.dirty());

    // A script error mid-play is reported, and the session survives: the zone's script breaks when
    // something enters, while the rest of the scene plays on.
    const auto broken = std::string(sliding).replace(std::string(sliding).find("    self.moving = true"), 0, "    error(\"the door is jammed\")\n");
    std::ofstream(content / "scripts/door.luau") << broken;
    harness.shell.check_script_files();
    REQUIRE(harness.shell.start_play());
    harness.shell.set_game_view(false);
    play_until(harness, 900, [&] { return logs(harness.shell.diagnostics(), "the door is jammed") > 0; });
    CHECK(logs(harness.shell.diagnostics(), "the door is jammed") == 1);
    CHECK(harness.shell.prompt() == EditorPrompt::notice); // "A script stopped"
    const auto failed_at = harness.shell.play_session()->clock().tick();
    play_until(harness, failed_at + 120, [] { return false; });
    REQUIRE(harness.shell.play_session()); // still playing
    CHECK(harness.shell.play_session()->clock().tick() == failed_at + 120);
    CHECK(position_in(harness.shell.play_session()->world(), ids["Door"]).z == Approx(door_at.z).margin(1e-3)); // jammed shut
    harness.shell.stop_play();

    // The sliding door is the version the player runs.
    std::ofstream(content / "scripts/door.luau") << sliding;
    harness.shell.check_script_files();
    harness.frames(2);
    CHECK(harness.shell.script_error(door_script).empty());
}
