// Lights in the editor (#1034, docs/editor.md#lights): point and spot lights from the Create menu, the
// Inspector showing the settings each kind uses, and the view's light limits reported. The shadow views are
// with the other debug views (editor_debug_view_tests.cpp).

#include "editor_harness.hpp"

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
LightComponent light_of(SceneEditor& scene, EntityId id) {
    return std::get<LightComponent>(*read_component(scene.world(), *scene.world().find(id), ComponentId::light));
}
/// Creates an entity from the Hierarchy's Create menu.
void create(Harness& harness, std::string_view item) {
    press(harness, control(harness, "hierarchy.create"));
    harness.frames(2); // the menu sizes itself on its first frame
    press(harness, control(harness, item));
    harness.frames(2);
}
} // namespace

TEST_CASE("Point and spot lights come from the Create menu, and the Inspector shows the settings each kind uses", "[editor][lights]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto& layout = harness.shell.layout();

    create(harness, "create.point-light");
    const auto point = find_named(scene, "Point light");
    CHECK(scene.primary() == point);
    CHECK(light_of(scene, point).kind == LightKind::point);
    CHECK(light_of(scene, point).intensity == 30.0f); // candela
    CHECK(scene.undo_label() == "Create Point light");
    harness.frames(2);
    CHECK(layout.field("light.intensity"));
    CHECK(layout.field("light.range"));
    CHECK_FALSE(layout.field("light.inner_cone"));
    CHECK_FALSE(layout.field("light.cast_shadows")); // point lights cast none yet
    CHECK_FALSE(layout.field("light.shadow_bias"));
    CHECK_FALSE(layout.field("light.shadow_distance"));

    create(harness, "create.spot-light");
    const auto spot = find_named(scene, "Spot light");
    CHECK(light_of(scene, spot).kind == LightKind::spot);
    harness.frames(2);
    CHECK(layout.field("light.inner_cone"));
    CHECK(layout.field("light.outer_cone"));
    CHECK(layout.field("light.cast_shadows"));
    CHECK(layout.field("light.shadow_bias"));
    CHECK(layout.field("light.shadow_normal_bias"));
    CHECK_FALSE(layout.field("light.shadow_distance")); // the sun's cascades only
    auto unshadowed = light_of(scene, spot);
    unshadowed.cast_shadows = false;
    REQUIRE(scene.set_component(spot, unshadowed));
    harness.frames(2);
    CHECK(layout.field("light.cast_shadows"));
    CHECK_FALSE(layout.field("light.shadow_bias"));

    // The sample's sun: no range or cones, and its shadows reach the shadow distance.
    scene.select(find_named(scene, "Sun"));
    harness.frames(2);
    CHECK_FALSE(layout.field("light.range"));
    CHECK_FALSE(layout.field("light.inner_cone"));
    CHECK(layout.field("light.shadow_distance"));
    CHECK(layout.field("light.shadow_bias"));
}

TEST_CASE("Diagnostics shows the view's lights, and the ones it leaves out or draws without shadows", "[editor][lights]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    // Twenty dim point lights and six bright spot lights casting shadows, all reaching the view.
    for (int i = 0; i < 20; ++i) REQUIRE(scene.create("Lamp " + std::to_string(i), std::nullopt, {LightComponent{LightKind::point, {1.0f}, 1.0f, 50.0f}}));
    for (int i = 0; i < 6; ++i) REQUIRE(scene.create("Spot " + std::to_string(i), std::nullopt, {LightComponent{LightKind::spot, {1.0f}, 1000.0f, 50.0f}}));
    harness.frames(2);
    const auto& lights = harness.shell.lights();
    CHECK(lights.local == max_local_lights);
    CHECK(lights.dropped == 26 - max_local_lights);
    CHECK(lights.unshadowed == 6 - max_shadowed_spot_lights);
    CHECK(lights.sun);
    const auto count = [&](RenderIssue code, std::string_view excerpt) {
        return std::ranges::count_if(harness.shell.frame_problems(), [&](const RenderDiagnostic& problem) {
            return problem.code == code && problem.message.find(excerpt) != std::string::npos;
        });
    };
    CHECK(count(RenderIssue::light_limit, "'Lamp ") == 26 - max_local_lights);
    CHECK(count(RenderIssue::shadow_limit, "'Spot ") == 6 - max_shadowed_spot_lights);
    // Fewer lights: nothing to report.
    REQUIRE(scene.undo());
    for (int i = 0; i < 15; ++i) REQUIRE(scene.undo());
    harness.frames(2);
    CHECK(harness.shell.lights().dropped == 0);
    CHECK(harness.shell.lights().unshadowed == 0);
    CHECK(count(RenderIssue::light_limit, "") == 0);
}
