#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/simulation/script_assets.hpp"
#include <catch2/catch_approx.hpp>

// Scripts in the editor (#1018): a script component's declared properties in the Inspector, and
// script messages and failures while playing.
using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;

namespace {
const auto spin_script = AssetId{0x6d617961, 0x20};

ImVec2 centre(const EditorLayout::Field& rect) { return {(rect.min.x + rect.max.x) / 2, (rect.min.y + rect.max.y) / 2}; }

ScriptComponent script_of(SceneEditor& scene, EntityId id) {
    return std::get<ScriptComponent>(*read_component(scene.world(), *scene.world().find(id), ComponentId::script));
}
std::optional<float> number(const ScriptComponent& script, const std::string& name) {
    for (const auto& value : script.values)
        if (value.name == name) return std::get<float>(value.data);
    return std::nullopt;
}
/// Adds a script to a project copy's catalog and returns its ID.
AssetId add_script(const ProjectCopy& copy, uint64_t low, const std::string& path, const std::string& source) {
    copy.write(path, source);
    auto catalog = copy.read("catalog.maya");
    auto line = std::ostringstream{};
    line << "script 6d617961 " << std::hex << low << " \"" << path << "\"\n";
    copy.write("catalog.maya", catalog + line.str());
    return {0x6d617961, low};
}
} // namespace

TEST_CASE("The Inspector shows a script's declared properties and edits their values with undo", "[editor][scripting]") {
    Harness harness;
    harness.frames(2);
    REQUIRE(harness.shell.open_scene(harness.shell.project()->content_root / "physics.scene"));
    auto& scene = *harness.shell.scene();
    const auto beacon = find_named(scene, "Beacon");
    scene.select(beacon);
    harness.frames(3);
    const auto* description = harness.shell.script_description(spin_script);
    REQUIRE(description);
    INFO(description->error);
    REQUIRE(*description);
    CHECK(description->properties.size() == 2);
    const auto& layout = harness.shell.layout();
    REQUIRE(layout.field("script.speed"));
    CHECK(layout.field("script.axis.x"));
    CHECK(number(script_of(scene, beacon), "speed") == 1.5f); // authored in physics.scene
    // A drag on a script property is one undo step, like any property.
    const auto history = scene.history_size();
    const auto from = centre(*layout.field("script.speed"));
    drag(harness, from, {from.x + 50.0f, from.y}, 10);
    const auto dragged = number(script_of(scene, beacon), "speed");
    REQUIRE(dragged);
    CHECK(*dragged > 1.5f);
    CHECK(scene.history_size() == history + 1);
    CHECK(scene.undo_label() == "Edit Script");
    REQUIRE(scene.undo());
    CHECK(number(script_of(scene, beacon), "speed") == 1.5f);
    // A value the script no longer declares is kept, reported, and can be removed.
    auto stale = script_of(scene, beacon);
    stale.values.push_back({"wobble", ScriptValueType::number, 2.0f});
    REQUIRE(scene.set_component(beacon, stale));
    harness.frames(2);
    const auto* remove = layout.control("script.remove_unused");
    REQUIRE(remove);
    press(harness, centre(*remove));
    harness.frames(2);
    CHECK(script_of(scene, beacon).values.size() == 1);
    // Values the script does not accept are refused by the component's validation.
    auto invalid = script_of(scene, beacon);
    invalid.values.push_back({"two words", ScriptValueType::number, 1.0f});
    CHECK_FALSE(scene.set_component(beacon, invalid));
}

TEST_CASE("A script that cannot be compiled says why in the Inspector", "[editor][scripting]") {
    const auto copy = ProjectCopy();
    const auto broken = add_script(copy, 0x30, "scripts/broken.luau", "local x = \nreturn {}");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    const auto* description = harness.shell.script_description(broken);
    REQUIRE(description);
    CHECK(description->error == "scripts/broken.luau:2: Expected identifier when parsing expression, got 'return'");
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    REQUIRE(scene.set_component(cube, ScriptComponent{AssetRef<ScriptAsset>{broken}, {}}));
    scene.select(cube);
    harness.frames(2); // the Inspector draws the error without failing
    // A script reference must name a script: a mesh is refused.
    CHECK(scene.set_component(cube, ScriptComponent{AssetRef<ScriptAsset>{AssetId{0x6d617961, 2}}, {}}).error ==
          "Asset catalog kind does not match the property");
}

TEST_CASE("While playing, script logs go to Diagnostics and a failing script is reported without stopping play", "[editor][scripting][play]") {
    const auto copy = ProjectCopy();
    const auto talker = add_script(copy, 0x31, "scripts/talker.luau", R"(
local T = {}
function T:start() maya.log("hello from " .. self.entity:name()) end
function T:fixed_update(dt)
    if maya.tick() == 5 then error("tick five went wrong") end
end
return T
)");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    REQUIRE(scene.set_component(cube, ScriptComponent{AssetRef<ScriptAsset>{talker}, {}}));
    REQUIRE(harness.shell.start_play());
    harness.frames(10);
    CHECK(harness.shell.play_session() != nullptr); // play goes on
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Red cube (scripts/talker.luau): hello from Red cube"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "scripts/talker.luau:5: tick five went wrong"));
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
}

TEST_CASE("A project's script limits apply when it plays", "[editor][scripting][play]") {
    auto project = ProjectSettings{};
    CHECK(project_script_settings(project).limits.work_per_call == ScriptLimits{}.work_per_call);
    CHECK(project_script_settings(project).limits.memory_bytes == ScriptLimits{}.memory_bytes);
    project.script_work = 3'000'000;
    project.script_memory = 128;
    CHECK(project_script_settings(project).limits.work_per_call == 3'000'000u);
    CHECK(project_script_settings(project).limits.memory_bytes == size_t{128} << 20);

    // A script that needs more work per call than the default budget, played with and without a raise.
    const auto play_heavy = [](const std::string& limit_lines) {
        const auto copy = ProjectCopy();
        const auto heavy = add_script(copy, 0x32, "scripts/heavy.luau", R"(
local H = {}
function H:fixed_update(dt)
    local total = 0
    for i = 1, 1500000 do total += 1 end
end
return H
)");
        std::ofstream(copy.folder / "project.maya", std::ios::app) << limit_lines;
        Harness harness(false);
        REQUIRE(harness.shell.open_project(copy.folder));
        harness.frames(2);
        auto& scene = *harness.shell.scene();
        REQUIRE(scene.set_component(find_named(scene, "Red cube"), ScriptComponent{AssetRef<ScriptAsset>{heavy}, {}}));
        REQUIRE(harness.shell.start_play());
        harness.frames(4);
        return logged(harness.shell.diagnostics(), DiagnosticSource::script, "work budget exceeded");
    };
    CHECK(play_heavy(""));
    CHECK_FALSE(play_heavy("script_work 3000000\n"));
}
