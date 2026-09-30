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
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    if (!found) found = harness.shell.layout().field(key);
    INFO(key);
    REQUIRE(found);
    return centre(*found);
}
void double_click(Harness& harness, ImVec2 at) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    for (int i = 0; i < 2; ++i) {
        harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
        harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    }
}
bool has_script(SceneEditor& scene, EntityId id) {
    return read_component(scene.world(), *scene.world().find(id), ComponentId::script).has_value();
}
/// Rewrites a script file; the next check sees its new time and size.
void rewrite(const ProjectCopy& copy, const std::string& path, const std::string& source) {
    copy.write(path, source);
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

TEST_CASE("Scripts are listed in the Assets panel, attached by dragging, and opened in an external editor", "[editor][scripting]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    REQUIRE_FALSE(has_script(scene, cube));
    // Dropped on a hierarchy row, a script is attached as one undo step.
    drag(harness, control(harness, "asset.scripts/spin.luau"), row_center(harness, cube));
    REQUIRE(has_script(scene, cube));
    CHECK(script_of(scene, cube).script.id == spin_script);
    REQUIRE(scene.undo());
    CHECK_FALSE(has_script(scene, cube));
    // Dropped on the object in the viewport, too.
    harness.frames(2);
    const auto position = std::get<TransformComponent>(*read_component(scene.world(), *scene.world().find(cube), ComponentId::transform));
    drag(harness, control(harness, "asset.scripts/spin.luau"), on_screen(harness, position.translation));
    CHECK(has_script(scene, cube));
    // Another script replaces it; the values it declares carry over, the others go.
    const auto other = add_script(copy, 0x33, "scripts/other.luau", "return { properties = { speed = { type = \"number\" } } }");
    harness.shell.refresh_project();
    auto spinning = script_of(scene, cube);
    spinning.values = {{"speed", ScriptValueType::number, 2.5f}, {"axis", ScriptValueType::vector, math::Vec3(1.0f)}};
    REQUIRE(scene.set_component(cube, spinning));
    REQUIRE(harness.shell.assign_asset(cube, other));
    CHECK(script_of(scene, cube).script.id == other);
    CHECK(script_of(scene, cube).values == std::vector<ScriptValue>{{"speed", ScriptValueType::number, 2.5f}});

    // Double-clicking a script, or Open script in the Inspector, opens its file elsewhere.
    harness.frames(2);
    double_click(harness, control(harness, "asset.scripts/spin.luau"));
    REQUIRE(harness.opened.size() == 1);
    CHECK(harness.opened[0] == copy.content / "scripts/spin.luau");
    scene.select(cube);
    harness.frames(3);
    press(harness, control(harness, "script.open"));
    harness.frames(1);
    REQUIRE(harness.opened.size() == 2);
    CHECK(harness.opened[1] == copy.content / "scripts/other.luau");
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Opened scripts/other.luau"));
    // When the system cannot open it, the editor says why.
    harness.open_result = "no application opens other.luau";
    harness.shell.open_script(other);
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Couldn't open scripts/other.luau: no application opens other.luau"));
}

TEST_CASE("Edited scripts reload while authoring, and a broken or missing file keeps the last good version", "[editor][scripting]") {
    const auto copy = ProjectCopy();
    const auto mover = add_script(copy, 0x34, "scripts/mover.luau", "return { properties = { speed = { type = \"number\" } } }");
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    REQUIRE(scene.set_component(cube, ScriptComponent{AssetRef<ScriptAsset>{mover}, {{"speed", ScriptValueType::number, 5.0f}}}));
    scene.select(cube);
    harness.frames(3);
    CHECK(harness.shell.layout().field("script.speed"));

    // A new property appears at the next check, which the editor makes a few times a second.
    rewrite(copy, "scripts/mover.luau", "return { properties = { speed = { type = \"number\" }, height = { type = \"vector\" } } }");
    harness.frames(20);
    CHECK(harness.shell.script_description(mover)->properties.size() == 2);
    CHECK(harness.shell.layout().field("script.height.x"));
    CHECK(number(script_of(scene, cube), "speed") == 5.0f); // the authored value is kept

    // A property the script no longer declares keeps its value in the scene, reported as unused.
    rewrite(copy, "scripts/mover.luau", "return { properties = { height = { type = \"vector\" } } }");
    harness.shell.check_script_files();
    harness.frames(2);
    CHECK_FALSE(harness.shell.layout().field("script.speed"));
    CHECK(harness.shell.layout().control("script.remove_unused"));
    CHECK(number(script_of(scene, cube), "speed") == 5.0f);

    // A syntax error: reported with its file and line, while the last good version stays in use.
    rewrite(copy, "scripts/mover.luau", "return { properties = {\n");
    harness.shell.check_script_files();
    harness.frames(2);
    CHECK(harness.shell.script_error(mover).starts_with("scripts/mover.luau:2:"));
    REQUIRE(*harness.shell.script_description(mover));
    CHECK(harness.shell.script_description(mover)->properties.size() == 1);
    CHECK(harness.shell.layout().field("script.height.x"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "scripts/mover.luau:2:"));
    CHECK(harness.shell.prompt() == EditorPrompt::none); // no notice while editing
    rewrite(copy, "scripts/mover.luau", "return { properties = { height = { type = \"vector\" }, jump = { type = \"boolean\" } } }");
    harness.shell.check_script_files();
    CHECK(harness.shell.script_error(mover).empty());
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "scripts/mover.luau compiles again"));

    // Renamed away, the file is missing; the scene keeps working with the last good version.
    fs::rename(copy.content / "scripts/mover.luau", copy.content / "scripts/renamed.luau");
    harness.shell.check_script_files();
    harness.frames(2);
    CHECK(harness.shell.script_error(mover) == "scripts/mover.luau is missing from the content folder");
    CHECK(harness.shell.script_description(mover)->properties.size() == 2);
    // Opening the project again reports it on load.
    Harness reopened(false);
    REQUIRE(reopened.shell.open_project(copy.folder));
    CHECK(logged(reopened.shell.diagnostics(), DiagnosticSource::script, "scripts/mover.luau is missing from the content folder"));
    CHECK_FALSE(*reopened.shell.script_description(mover));
    // The Assets panel lists it among the missing files, as it does meshes and materials.
    harness.shell.refresh_project();
    CHECK(std::ranges::find(harness.shell.missing_asset_files(), mover) != harness.shell.missing_asset_files().end());
    // Renamed back, it is found again.
    fs::rename(copy.content / "scripts/renamed.luau", copy.content / "scripts/mover.luau");
    harness.shell.check_script_files();
    CHECK(harness.shell.script_error(mover).empty());
}

TEST_CASE("While playing, a changed script replaces its instances at the next tick; a broken one leaves them running", "[editor][scripting][play]") {
    const auto copy = ProjectCopy();
    const auto version = [](const std::string& start) {
        return "local T = {}\nT.properties = { count = { type = \"integer\" } }\nfunction T:start() maya.log(" + start +
               ") end\nfunction T:fixed_update(dt) self.count += 1 end\nfunction T:stop() maya.log('stopping') end\nreturn T\n";
    };
    const auto talker = add_script(copy, 0x35, "scripts/talker.luau", version("'v1'"));
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    REQUIRE(scene.set_component(find_named(scene, "Red cube"), ScriptComponent{AssetRef<ScriptAsset>{talker}, {}}));
    REQUIRE(harness.shell.start_play());
    harness.frames(3);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Red cube (scripts/talker.luau): v1"));

    // A new version: the old instance stops, the new one starts with the count it had reached.
    rewrite(copy, "scripts/talker.luau", version("'v2 at ' .. self.count"));
    harness.shell.check_script_files();
    harness.frames(2);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Red cube (scripts/talker.luau): stopping"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Red cube (scripts/talker.luau): v2 at "));
    CHECK_FALSE(logged(harness.shell.diagnostics(), DiagnosticSource::script, "v2 at 0"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Reloaded scripts/talker.luau (1 instance)"));

    // A broken version is shown, and the running one goes on.
    rewrite(copy, "scripts/talker.luau", "local T = {\n");
    harness.shell.check_script_files();
    CHECK(harness.shell.prompt() == EditorPrompt::notice);
    harness.frames(3);
    REQUIRE(harness.shell.play_session());
    CHECK(harness.shell.play_session()->error().empty());

    // While paused, the swap waits for a step.
    harness.shell.toggle_pause();
    rewrite(copy, "scripts/talker.luau", version("'v3'"));
    harness.shell.check_script_files();
    harness.frames(3);
    CHECK_FALSE(logged(harness.shell.diagnostics(), DiagnosticSource::script, "v3"));
    harness.shell.step_play();
    harness.frames(2);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Red cube (scripts/talker.luau): v3"));

    // The next Play uses the current good version.
    harness.shell.stop_play();
    rewrite(copy, "scripts/talker.luau", version("'v4'"));
    harness.shell.check_script_files();
    REQUIRE(harness.shell.start_play());
    harness.frames(2);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Red cube (scripts/talker.luau): v4"));
}

TEST_CASE("Two hundred edits to a script while playing keep memory flat and the session running", "[editor][scripting][play]") {
    const auto copy = ProjectCopy();
    const auto version = [](int n) {
        return "local S = {}\nS.properties = { speed = { type = \"number\" } }\nlocal names = {}\n"
               "for i = 1, 100 do names[i] = 'edit " + std::to_string(n) + " ' .. i end\n"
               "function S:fixed_update(dt) self.speed += #names end\nreturn S\n";
    };
    const auto edited = add_script(copy, 0x36, "scripts/edited.luau", version(0));
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    for (const auto* name : {"Red cube", "Pyramid"})
        REQUIRE(scene.set_component(find_named(scene, name), ScriptComponent{AssetRef<ScriptAsset>{edited}, {}}));
    REQUIRE(harness.shell.start_play());
    harness.frames(2);
    auto early = size_t{0};
    for (int n = 1; n <= 200; ++n) {
        rewrite(copy, "scripts/edited.luau", version(n));
        harness.shell.check_script_files();
        harness.frame();
        if (n == 20) early = harness.shell.play_script_memory();
    }
    REQUIRE(harness.shell.play_session());
    CHECK(harness.shell.play_session()->error().empty());
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::script, "Reloaded scripts/edited.luau (2 instances)"));
    CHECK_FALSE(std::ranges::any_of(harness.shell.diagnostics().entries(), [](const DiagnosticEntry& entry) {
        return entry.source == DiagnosticSource::script && entry.message.find("error") != std::string::npos;
    }));
    INFO("script memory after 20 edits " << early << " bytes, after 200 " << harness.shell.play_script_memory());
    CHECK(harness.shell.play_script_memory() < early + early / 4);
}
