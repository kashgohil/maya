#include "editor_harness.hpp"
#include "maya/core/file_system.hpp"
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <fstream>
#include <iterator>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using Catch::Approx;


TEST_CASE("Viewport pixel sizes are whole framebuffer pixels for the panel's points", "[editor]") {
    CHECK(viewport_pixels(640.0f, 360.0f, 2.0f) == PixelSize{1280, 720});
    CHECK(viewport_pixels(640.75f, 360.4f, 2.0f) == PixelSize{1281, 720}); // never rounds up past the panel
    CHECK(viewport_pixels(640.0f, 360.0f, 1.0f) == PixelSize{640, 360});
    CHECK(viewport_pixels(100.0f, 50.0f, 1.5f) == PixelSize{150, 75});
    CHECK(viewport_pixels(0.4f, 100.0f, 2.0f).empty());
    CHECK(viewport_pixels(-5.0f, 100.0f, 2.0f).empty());
    CHECK(viewport_pixels(100.0f, 100.0f, 0.0f).empty());
    CHECK(viewport_pixels(100.0f, 100.0f, std::nanf("")).empty());
    CHECK(viewport_pixels(1e9f, 10.0f, 2.0f).width == 16384);
}

TEST_CASE("While the scene plays, a click on the game view gives the game every event until Escape", "[editor][play]") {
    InputRouter router;
    const auto game_view = RouterContext{true, true};
    const auto down = [](MouseButton button) { return MouseButtonEvent{button, true, KeyModifiers::none}; };
    // Over the game view, the right button does not fly the editor camera and scrolling does not dolly.
    auto routed = router.route({down(MouseButton::right), ScrollEvent{0, 3}}, game_view);
    CHECK_FALSE(router.navigating());
    CHECK(routed.navigation.dolly == 0.0f);
    CHECK(routed.ui.size() == 2);
    // A left click elsewhere is the UI's; on the game view it hands over the input, and is not passed on.
    routed = router.route({down(MouseButton::left)}, RouterContext{false, true});
    CHECK_FALSE(router.game_has_input());
    routed = router.route({down(MouseButton::left)}, game_view);
    CHECK(router.game_has_input());
    CHECK(routed.game_started);
    CHECK(routed.capture == true);
    CHECK(routed.ui.empty());
    CHECK(routed.game.empty());
    // Then the game gets everything, shortcuts and text included, wherever the pointer is.
    routed = router.route({KeyEvent{KeyCode::W, true, KeyModifiers::none}, TextEvent{'w'},
                           KeyEvent{KeyCode::P, true, KeyModifiers::super}, MouseMoveEvent{5, 5}, ScrollEvent{0, 1}}, RouterContext{});
    CHECK(routed.game.size() == 5);
    CHECK(routed.ui.empty());
    CHECK(routed.navigation.dolly == 0.0f);
    // Escape takes it back (the key is not passed on); the UI learns where the pointer is.
    routed = router.route({KeyEvent{KeyCode::Escape, true, KeyModifiers::none}, KeyEvent{KeyCode::Escape, false, KeyModifiers::none},
                           KeyEvent{KeyCode::A, true, KeyModifiers::none}}, game_view);
    CHECK_FALSE(router.game_has_input());
    CHECK(routed.game_ended);
    CHECK(routed.capture == false);
    CHECK(routed.game.empty());
    REQUIRE(routed.ui.size() == 3); // the pointer, the Escape release, and A
    CHECK(std::holds_alternative<MouseMoveEvent>(routed.ui[0]));
    // Losing focus takes it back too, and the game sees the focus change so it can release its keys.
    router.route({down(MouseButton::left)}, game_view);
    routed = router.route({FocusEvent{false}}, game_view);
    CHECK_FALSE(router.game_has_input());
    CHECK(routed.game.size() == 1);
    CHECK(routed.game_ended);
    // cancel() ends it, as when play stops.
    router.route({down(MouseButton::left)}, game_view);
    CHECK(router.cancel() == false);
    CHECK_FALSE(router.game_has_input());
    CHECK_FALSE(router.cancel());
}

TEST_CASE("The router gives typing to the UI and navigation input to the camera", "[editor]") {
    InputRouter router;
    const auto over_viewport = RouterContext{true};
    // Without navigation, every event goes to the UI, even over the viewport.
    auto routed = router.route({KeyEvent{KeyCode::W, true, KeyModifiers::none}, TextEvent{'w'},
                                MouseMoveEvent{10, 10}, MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}},
                               over_viewport);
    CHECK(routed.ui.size() == 4);
    CHECK_FALSE(routed.navigation.forward);
    CHECK_FALSE(routed.capture);

    // The right button elsewhere is a UI click; over the viewport it starts navigation.
    routed = router.route({MouseButtonEvent{MouseButton::right, true, KeyModifiers::none}}, RouterContext{false});
    CHECK(routed.ui.size() == 1);
    CHECK_FALSE(router.navigating());
    routed = router.route({MouseButtonEvent{MouseButton::right, false, KeyModifiers::none},
                           MouseButtonEvent{MouseButton::right, true, KeyModifiers::none}}, over_viewport);
    CHECK(router.navigating());
    CHECK(routed.capture == true);
    CHECK(routed.navigation_started);
    CHECK(routed.ui.size() == 1); // only the earlier release reached the UI

    // While navigating: movement keys and pointer motion drive the camera; text reaches nobody.
    routed = router.route({KeyEvent{KeyCode::W, true, KeyModifiers::none}, TextEvent{'w'},
                           KeyEvent{KeyCode::LeftShift, true, KeyModifiers::shift}, MouseMoveEvent{14, 7},
                           MouseMoveEvent{20, 4}, KeyEvent{KeyCode::Tab, true, KeyModifiers::none}}, RouterContext{false});
    CHECK(routed.ui.empty());
    CHECK(routed.navigation.forward);
    CHECK(routed.navigation.fast);
    CHECK(routed.navigation.look.x == 10.0f);
    CHECK(routed.navigation.look.y == -6.0f);
    // Held keys persist across frames until released.
    routed = router.route({}, RouterContext{false});
    CHECK(routed.navigation.forward);
    CHECK(routed.navigation.look.x == 0.0f);
    routed = router.route({KeyEvent{KeyCode::W, false, KeyModifiers::none}, KeyEvent{KeyCode::D, true, KeyModifiers::none}},
                          RouterContext{false});
    CHECK_FALSE(routed.navigation.forward);
    CHECK(routed.navigation.right);
}

TEST_CASE("Navigation ends on release, Escape, focus loss, or cancellation and clears held keys", "[editor]") {
    const auto start = [](InputRouter& router) {
        router.route({MouseMoveEvent{50, 60}, MouseButtonEvent{MouseButton::right, true, KeyModifiers::none},
                      KeyEvent{KeyCode::S, true, KeyModifiers::none}}, RouterContext{true});
        REQUIRE(router.navigating());
    };
    const std::vector<std::vector<InputEvent>> endings = {
        {MouseButtonEvent{MouseButton::right, false, KeyModifiers::none}},
        {KeyEvent{KeyCode::Escape, true, KeyModifiers::none}},
        {FocusEvent{false}},
    };
    for (const auto& ending : endings) {
        InputRouter router;
        start(router);
        auto routed = router.route(ending, RouterContext{false});
        CHECK_FALSE(router.navigating());
        CHECK(routed.capture == false);
        CHECK_FALSE(routed.navigation.back); // held keys do not outlive navigation
        REQUIRE_FALSE(routed.ui.empty());
        // The UI learns where the pointer is now.
        CHECK(std::ranges::any_of(routed.ui, [](const InputEvent& e) {
            const auto* move = std::get_if<MouseMoveEvent>(&e);
            return move && move->x == 50 && move->y == 60;
        }));
        routed = router.route({KeyEvent{KeyCode::W, true, KeyModifiers::none}}, RouterContext{true});
        CHECK_FALSE(routed.navigation.forward);
        CHECK(routed.ui.size() == 1);
    }
    InputRouter router;
    CHECK_FALSE(router.cancel());
    start(router);
    CHECK(router.cancel() == false);
    CHECK_FALSE(router.navigating());
}

TEST_CASE("Scrolling over the viewport dollies the camera instead of scrolling the UI", "[editor]") {
    InputRouter router;
    auto routed = router.route({ScrollEvent{0, 2}, ScrollEvent{0, 1}}, RouterContext{true});
    CHECK(routed.navigation.dolly == 3.0f);
    CHECK(routed.ui.empty());
    CHECK_FALSE(router.navigating());
    routed = router.route({ScrollEvent{0, 2}}, RouterContext{false});
    CHECK(routed.navigation.dolly == 0.0f);
    CHECK(routed.ui.size() == 1);
}

TEST_CASE("The editor camera flies relative to its view and keeps a rigid pose", "[editor]") {
    auto camera = EditorCamera::looking_at({0, 0, 5}, {0, 0, 0});
    CHECK(camera.yaw == Approx(0.0f).margin(1e-6));
    CHECK(camera.pitch == Approx(0.0f).margin(1e-6));
    auto input = NavigationInput{};
    input.forward = true;
    camera.update(input, 0.5f);
    CHECK(camera.position.z == Approx(5.0f - 1.5f));
    input.fast = true;
    camera.update(input, 0.5f);
    CHECK(camera.position.z == Approx(3.5f - 6.0f));
    // Turning right by a quarter turn makes forward point along +X.
    auto look = NavigationInput{};
    look.look = {math::PI / 2.0f / camera.look_sensitivity, 0.0f};
    camera.update(look, 0.0f);
    CHECK(camera.forward().x == Approx(1.0f).margin(1e-5));
    // Pitch is clamped short of straight up; the pose stays a valid camera pose.
    look.look = {0.0f, -1e6f};
    camera.update(look, 0.0f);
    CHECK(camera.pitch < math::PI / 2.0f);
    CHECK(make_render_view(camera.camera, camera.pose(), 16, 9));
    auto dolly = NavigationInput{};
    dolly.dolly = 2.0f;
    const auto before = camera.position;
    camera.update(dolly, 0.0f);
    CHECK((camera.position - before).length() == Approx(2.0f * camera.speed * 0.25f));
    camera.update(input, std::nanf(""));
    CHECK(std::isfinite(camera.position.x));
}

namespace {
/// Where a point lies in the camera's view: its direction from the camera, in camera axes.
math::Vec3 seen(const EditorCamera& camera, const math::DVec3& point) {
    const auto pose = camera.pose();
    const auto axis = [&](int c) { return pose.axis(c); };
    const auto d = (point - camera.position).to_float().normalized();
    return {math::Vec3::dot(d, axis(0)), math::Vec3::dot(d, axis(1)), math::Vec3::dot(d, axis(2))};
}
} // namespace

TEST_CASE("The editor camera orbits, pans, and zooms around its pivot", "[editor]") {
    auto camera = EditorCamera::looking_at({0, 2, 6}, {0, 0, 0});
    camera.pivot = {1.0f, 0.0f, 0.0f}; // off the view's centre: orbiting must not snap to it
    const auto distance = (camera.position - camera.pivot).length();
    const auto on_screen = seen(camera, camera.pivot);
    auto orbit = NavigationInput{};
    orbit.mode = NavigationMode::orbit;
    orbit.look = {120.0f, -40.0f};
    camera.update(orbit, 0.0f);
    CHECK((camera.position - camera.pivot).length() == Approx(distance).epsilon(1e-4));
    CHECK(seen(camera, camera.pivot).x == Approx(on_screen.x).margin(1e-4));
    CHECK(seen(camera, camera.pivot).y == Approx(on_screen.y).margin(1e-4));
    CHECK(camera.yaw != Approx(0.0f).margin(1e-3)); // it turned
    // Orbiting over the top stops at the pitch limit, still at the same distance.
    orbit.look = {0.0f, 1e5f};
    camera.update(orbit, 0.0f);
    CHECK(camera.pitch > -math::PI / 2.0f);
    CHECK((camera.position - camera.pivot).length() == Approx(distance).epsilon(1e-4));

    // Pan: the view keeps its direction and the pivot moves with the camera.
    const auto yaw = camera.yaw, pitch = camera.pitch;
    const auto before = camera.position, pivot = camera.pivot;
    camera.pan_scale = 0.01f;
    auto pan = NavigationInput{};
    pan.mode = NavigationMode::pan;
    pan.look = {100.0f, 0.0f}; // drag right: the camera moves left, one metre
    camera.update(pan, 0.0f);
    CHECK(camera.yaw == yaw);
    CHECK(camera.pitch == pitch);
    CHECK((camera.position - before).length() == Approx(1.0f).epsilon(1e-4));
    CHECK((camera.pivot - pivot).length() == Approx(1.0f).epsilon(1e-4));
    CHECK(math::Vec3::dot((camera.position - before).to_float(), math::Vec3(std::cos(yaw), 0.0f, -std::sin(yaw))) == Approx(-1.0f).epsilon(1e-4));

    // Zoom: dragging right moves toward the pivot, and stops just short of it.
    auto zoom = NavigationInput{};
    zoom.mode = NavigationMode::zoom;
    zoom.look = {100.0f, 0.0f};
    const auto far = (camera.position - camera.pivot).length();
    camera.update(zoom, 0.0f);
    CHECK((camera.position - camera.pivot).length() < far);
    zoom.look = {1e5f, 0.0f};
    camera.update(zoom, 0.0f);
    CHECK((camera.position - camera.pivot).length() == Approx(0.05f).epsilon(1e-3));
    CHECK(make_render_view(camera.camera, camera.pose(), 16, 9));
}

TEST_CASE("Alt and left drag orbits, the middle button pans, and Alt and right drag zooms", "[editor]") {
    const auto over_viewport = RouterContext{true};
    const auto press = [](MouseButton button, bool down, KeyModifiers modifiers = KeyModifiers::none) {
        return MouseButtonEvent{button, down, modifiers};
    };
    const struct {
        MouseButton button;
        KeyModifiers modifiers;
        NavigationMode mode;
    } cases[] = {{MouseButton::left, KeyModifiers::alt, NavigationMode::orbit}, {MouseButton::middle, KeyModifiers::none, NavigationMode::pan},
                 {MouseButton::right, KeyModifiers::alt, NavigationMode::zoom}, {MouseButton::right, KeyModifiers::none, NavigationMode::fly}};
    for (const auto& [button, modifiers, mode] : cases) {
        INFO(int(mode));
        InputRouter router;
        auto routed = router.route({MouseMoveEvent{40, 30}, press(button, true, modifiers), MouseMoveEvent{50, 25}}, over_viewport);
        CHECK(router.navigation() == mode);
        CHECK(routed.navigation.mode == mode);
        CHECK(routed.capture == true);
        REQUIRE(routed.navigation_point);
        CHECK(routed.navigation_point->x == 40.0f); // where the drag started, for the pivot
        CHECK(routed.navigation.look.x == 10.0f);
        CHECK(routed.ui.size() == 1); // only the move before the press; the press is not a click
        // WASD moves only while flying, and never reaches the UI while navigating.
        routed = router.route({KeyEvent{KeyCode::W, true, KeyModifiers::none}}, over_viewport);
        CHECK(routed.navigation.forward == (mode == NavigationMode::fly));
        CHECK(routed.ui.empty());
        // Another button's release does not end it; its own does.
        routed = router.route({press(button == MouseButton::middle ? MouseButton::left : MouseButton::middle, false)}, over_viewport);
        CHECK(router.navigating());
        routed = router.route({press(button, false)}, over_viewport);
        CHECK_FALSE(router.navigating());
        CHECK(routed.capture == false);
        CHECK(routed.navigation.mode == NavigationMode::none);
    }
    // A plain left click still selects, and none of these start outside the viewport.
    InputRouter router;
    auto routed = router.route({press(MouseButton::left, true)}, over_viewport);
    CHECK_FALSE(router.navigating());
    CHECK(routed.ui.size() == 1);
    routed = router.route({press(MouseButton::left, false), press(MouseButton::middle, true)}, RouterContext{false});
    CHECK_FALSE(router.navigating());
}

TEST_CASE("Orbiting in the viewport turns around the selection without changing it", "[editor]") {
    Harness harness;
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto cube = find_named(scene, "Red cube");
    scene.select(cube);
    harness.frames(2);
    const auto pivot = harness.shell.navigation_pivot(harness.viewport_center());
    const auto distance = (harness.shell.camera().position - pivot).length();
    const auto start = harness.viewport_center();
    harness.frame({MouseMoveEvent{start.x, start.y}});
    harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::alt}});
    for (int i = 1; i <= 10; ++i) harness.frame({MouseMoveEvent{start.x + i * 12.0f, start.y + i * 3.0f}});
    CHECK(harness.shell.camera().pivot.x == Approx(pivot.x));
    CHECK((harness.shell.camera().position - pivot).length() == Approx(distance).epsilon(1e-3));
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::alt}});
    CHECK(scene.selection() == std::vector<EntityId>{cube}); // the drag did not pick
    // With nothing selected, a middle drag pans: the view keeps its direction.
    scene.clear_selection();
    harness.frames(2);
    const auto yaw = harness.shell.camera().yaw;
    const auto before = harness.shell.camera().position;
    harness.frame({MouseMoveEvent{start.x, start.y}});
    harness.frame({MouseButtonEvent{MouseButton::middle, true, KeyModifiers::none}});
    for (int i = 1; i <= 5; ++i) harness.frame({MouseMoveEvent{start.x + i * 10.0f, start.y}});
    harness.frame({MouseButtonEvent{MouseButton::middle, false, KeyModifiers::none}});
    CHECK(harness.shell.camera().yaw == yaw);
    CHECK((harness.shell.camera().position - before).length() > 0.1f);
}

TEST_CASE("The dock layout leaves room for panels and the viewport matches its panel at Retina scale", "[editor]") {
    Harness harness;
    harness.frames(3);
    const auto request = harness.shell.viewport_request();
    REQUIRE_FALSE(request.empty());
    // Hierarchy, inspector, assets, and diagnostics panels take space; the viewport is smaller than the window.
    CHECK(request.width < harness.metrics.framebuffer_width);
    CHECK(request.height < harness.metrics.framebuffer_height);
    const auto docked = harness.with_context([] {
        auto names = std::vector<std::string>{};
        for (const auto* name : {"###Hierarchy", "###Viewport", "###Inspector", "###Assets", "###Diagnostics"}) {
            const auto* window = ImGui::FindWindowByName(name);
            if (window && window->DockIsActive) names.emplace_back(name);
        }
        return names;
    });
    CHECK(docked.size() == 5);
    // Tabs are taller than other frames; each panel reserves exactly its tab bar's height, so panel
    // content never overlaps the tabs.
    const auto mismatched = harness.with_context([] {
        auto count = 0;
        for (const auto* name : {"###Hierarchy", "###Viewport", "###Inspector", "###Assets", "###Diagnostics"}) {
            const auto* window = ImGui::FindWindowByName(name);
            if (!window || !window->DockNode || !window->DockNode->TabBar) { ++count; continue; }
            const auto expected = ImGui::GetFontSize() + theme::tab_padding.y * 2.0f;
            if (window->TitleBarHeight != expected || window->DockNode->TabBar->BarRect.GetHeight() != expected) ++count;
        }
        return count;
    });
    CHECK(mismatched == 0);
    // One texel per framebuffer pixel: the image in points times the scale is the target size.
    const auto& layout = harness.shell.layout();
    CHECK((layout.viewport_max.x - layout.viewport_min.x) * 2.0f == Approx(float(request.width)));
    CHECK((layout.viewport_max.y - layout.viewport_min.y) * 2.0f == Approx(float(request.height)));
    CHECK(harness.shell.viewport().width() == request.width);
    CHECK(harness.shell.viewport().height() == request.height);

    // Steady frames never reallocate.
    harness.frames(20);
    CHECK(harness.shell.viewport().allocations() == 1);

    // A window resize reallocates once; the same window at 1x halves the pixels.
    harness.resize_window({1000, 700, 2000, 1400});
    harness.frames(5);
    CHECK(harness.shell.viewport().allocations() == 2);
    const auto retina = harness.shell.viewport_request();
    harness.resize_window({1000, 700, 1000, 700});
    harness.frames(5);
    CHECK(harness.shell.viewport().allocations() == 3);
    const auto standard = harness.shell.viewport_request();
    CHECK(std::abs(int(retina.width) - 2 * int(standard.width)) <= 2);
    CHECK(std::abs(int(retina.height) - 2 * int(standard.height)) <= 2);

    // Minimizing pauses the viewport without reallocating; restoring resumes it.
    const auto frames = harness.shell.frames();
    harness.resize_window({0, 0, 0, 0});
    harness.frames(3);
    CHECK(harness.shell.frames() == frames);
    CHECK(harness.shell.viewport().allocations() == 3);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::viewport, "minimized"));
    harness.resize_window({1000, 700, 1000, 700});
    harness.frames(2);
    CHECK(harness.shell.frames() == frames + 2);
    CHECK(harness.shell.viewport().allocations() == 3);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::viewport, "restored"));
}

TEST_CASE("Typing into an inspector field never moves the camera; navigation takes focus from it", "[editor]") {
    Harness harness;
    harness.frames(3);
    const auto& layout = harness.shell.layout();
    const auto field = ImVec2{layout.camera_speed_min.x + 6.0f, (layout.camera_speed_min.y + layout.camera_speed_max.y) / 2};
    click(harness, field);
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    harness.frame();
    REQUIRE(harness.shell.ui_wants_text());

    // WASD while editing text: the keys and characters go to the field, not the camera.
    const auto start = harness.shell.camera().position;
    for (const auto [code, text] : {std::pair{KeyCode::W, 'w'}, {KeyCode::A, 'a'}, {KeyCode::S, 's'}, {KeyCode::D, 'd'}}) {
        harness.frame(key(code, true, text));
        harness.frames(5); // held down
        harness.frame(key(code, false));
    }
    for (const auto digit : {'7', '5'}) {
        harness.frame(key(KeyCode(int(KeyCode::Num0) + (digit - '0')), true, uint32_t(digit)));
        harness.frame(key(KeyCode(int(KeyCode::Num0) + (digit - '0')), false));
    }
    CHECK(same(harness.shell.camera().position, start));
    CHECK(harness.shell.ui_wants_text());
    CHECK_FALSE(harness.captured);
    CHECK(active_text(harness) == "wasd75"); // selected on activation, then replaced by the typing

    // Holding the right button over the viewport starts navigation and ends text entry.
    click(harness, harness.viewport_center(), MouseButton::right);
    CHECK(harness.shell.navigating());
    CHECK(harness.captured);
    CHECK_FALSE(harness.shell.ui_wants_text());
    harness.frame(key(KeyCode::W, true));
    harness.frames(10);
    const auto moved = harness.shell.camera().position;
    CHECK((moved - start).length() > 0.1f);
    CHECK(math::Vec3::dot((moved - start).to_float().normalized(), harness.shell.camera().forward()) == Approx(1.0f));

    // Releasing the button returns the cursor; a key still held no longer moves the camera.
    harness.frame({MouseButtonEvent{MouseButton::right, false, KeyModifiers::none}});
    CHECK_FALSE(harness.shell.navigating());
    CHECK_FALSE(harness.captured);
    const auto stopped = harness.shell.camera().position;
    harness.frames(10);
    CHECK(same(harness.shell.camera().position, stopped));
}

TEST_CASE("Escape, focus loss, and a disappearing viewport release a captured cursor", "[editor]") {
    Harness harness;
    harness.frames(3);
    const auto start_navigation = [&] {
        click(harness, harness.viewport_center(), MouseButton::right);
        REQUIRE(harness.shell.navigating());
        REQUIRE(harness.captured);
    };
    start_navigation();
    harness.frame(key(KeyCode::Escape, true));
    CHECK_FALSE(harness.shell.navigating());
    CHECK_FALSE(harness.captured);
    harness.frame({KeyEvent{KeyCode::Escape, false, KeyModifiers::none},
                   MouseButtonEvent{MouseButton::right, false, KeyModifiers::none}});

    start_navigation();
    harness.frame({FocusEvent{false}});
    CHECK_FALSE(harness.shell.navigating());
    CHECK_FALSE(harness.captured);
    harness.frame({FocusEvent{true}, MouseButtonEvent{MouseButton::right, false, KeyModifiers::none}});

    start_navigation();
    harness.resize_window({0, 0, 0, 0});
    harness.frame();
    CHECK_FALSE(harness.shell.navigating());
    CHECK_FALSE(harness.captured);
}

TEST_CASE("UI drawing is clipped to the window and samples the viewport texture", "[editor]") {
    Harness harness;
    harness.frames(3);
    const auto& device = harness.device;
    REQUIRE(device.indexed_draws > 0);
    // Every UI draw is clipped; panels clip to less than the whole window. (The scene's draws are
    // not scissored, and it draws into its own pass, so compare against UI draws only.)
    CHECK(device.scissors.size() == harness.shell.ui_renderer().stats().draws);
    CHECK(std::ranges::any_of(device.scissors, [&](const ScissorRect& rect) {
        return rect.width < harness.metrics.framebuffer_width / 2;
    }));
    for (const auto& rect : device.scissors) {
        CHECK(rect.width > 0);
        CHECK(rect.x + rect.width <= harness.metrics.framebuffer_width);
        CHECK(rect.y + rect.height <= harness.metrics.framebuffer_height);
    }
    CHECK(std::ranges::find(device.sampled, harness.shell.viewport().color().slot) != device.sampled.end());
    const auto ui = std::ranges::find(device.pipelines, std::string("editor ui"), &PipelineDesc::label);
    REQUIRE(ui != device.pipelines.end());
    CHECK(ui->blend == BlendMode::alpha);
    CHECK(ui->color_formats == std::vector<Format>{Format::bgra8_unorm});
    CHECK(harness.shell.ui_renderer().stats().missing_textures == 0);
    CHECK(harness.shell.extraction().mesh_renderers == 4);
}

EditorFonts read_fonts() {
    const auto read = [](const char* relative) {
        const auto path = FileSystem::resolve(relative);
        REQUIRE(path);
        auto file = std::ifstream(*path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    return {read("resources/fonts/Inter-Regular.ttf"), read("resources/fonts/Inter-SemiBold.ttf"),
            read("resources/fonts/GeistMono-Regular.ttf"), read("resources/fonts/Phosphor-Light.ttf")};
}

TEST_CASE("The editor loads Inter and Geist Mono and falls back to the built-in font", "[editor]") {
    const auto fonts = read_fonts();
    REQUIRE(fonts.regular.size() > 1000);
    const auto broken = EditorFonts{"not a font", fonts.semibold, std::string(300, 'x'), "not icons"};
    for (const auto& [data, failures] : {std::pair{fonts, size_t{0}}, {broken, size_t{3}}}) {
        EditorDevice device;
        EditorShell shell(device, "renderer source", "ui source", {}, data);
        shell.update(1.0f / 60.0f, {}, {1280, 720, 2560, 1440});
        CHECK(std::ranges::count_if(shell.diagnostics().entries(), [](const DiagnosticEntry& entry) {
            return entry.message.find("could not be loaded") != std::string::npos;
        }) == long(failures));
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(shell.context());
        const auto& atlas = *ImGui::GetIO().Fonts;
        REQUIRE(atlas.Fonts.Size == 4); // body, strong, caption, mono
        // Rasterized at twice the point size for a 2x display; the built-in font is 13 px.
        CHECK(atlas.Fonts[0]->FontSize == (failures ? 26.0f : 28.0f));
        CHECK(atlas.Fonts[2]->FontSize == 22.0f); // 11 pt captions
        CHECK(ImGui::GetIO().FontGlobalScale == 0.5f);
        // Icons are merged into the body font only when the icon font loaded.
        const auto* folder = atlas.Fonts[0]->FindGlyphNoFallback(0xE25A);
        CHECK((folder != nullptr) == (failures == 0));
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("Renderer, resource, and scene problems appear in diagnostics without stopping the editor", "[editor]") {
    SECTION("A missing project leaves an empty editor with an explanation") {
        Harness harness(false);
        CHECK_FALSE(harness.shell.open_project("/nonexistent/project.maya"));
        harness.frames(2);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::project, "Cannot read project file"));
        CHECK(harness.shell.prompt() == EditorPrompt::notice);
        CHECK(harness.shell.viewport_request().empty());
        CHECK(harness.shell.viewport().allocations() == 0);
    }
    SECTION("A viewport target that cannot be allocated") {
        Harness harness;
        harness.device.fail_texture = [](const TextureDesc& desc) { return desc.label == "editor viewport color"; };
        harness.frames(3);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::viewport, "injected texture failure"));
        CHECK(harness.shell.ui_renderer().stats().draws > 0); // the rest of the editor still draws
        harness.device.fail_texture = {};
        harness.frames(2);
        CHECK(harness.shell.viewport().valid()); // recovers on a later frame
    }
    SECTION("A renderer pipeline that fails to compile") {
        Harness harness;
        harness.device.fail_pipeline = [](const PipelineDesc& desc) { return desc.label == "lit mesh"; };
        harness.frames(3);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::renderer, "injected pipeline failure"));
        const auto entry = std::ranges::find(harness.shell.diagnostics().entries(), DiagnosticSource::renderer,
                                             &DiagnosticEntry::source);
        REQUIRE(entry != harness.shell.diagnostics().entries().end());
        CHECK(entry->count == 2); // one entry, counted on each frame after the viewport's first layout
    }
    SECTION("Upload memory too small for the UI is returned as a frame error") {
        Harness harness(true, {3, 2048});
        auto error = RhiDiagnostic{};
        for (int frame = 0; frame < 3; ++frame) { // windows are hidden during their first frame
            harness.shell.update(1.0f / 60.0f, {}, harness.metrics);
            REQUIRE_FALSE(harness.device.begin_frame());
            error = harness.shell.render(harness.window);
            CHECK_FALSE(harness.device.end_frame()); // the pass was closed, so the frame ends cleanly
        }
        CHECK(error.code == RhiError::out_of_memory);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::ui, "upload memory exhausted"));
    }
}

TEST_CASE("The hierarchy shows the scene in order and selects rows with the mouse", "[editor][hierarchy]") {
    Harness harness;
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto& rows = harness.shell.layout().hierarchy_rows;
    REQUIRE(rows.size() == scene.world().size());
    for (size_t i = 0; i < scene.roots().size(); ++i) CHECK(rows[i].id == scene.roots()[i]); // flat sample scene
    const auto pyramid = find_named(scene, "Pyramid"), sun = find_named(scene, "Sun");
    press(harness, row_center(harness, pyramid));
    CHECK(scene.selection() == std::vector{pyramid});
    harness.frame({KeyEvent{KeyCode::LeftSuper, true, KeyModifiers::super}});
    press(harness, row_center(harness, sun));
    harness.frame({KeyEvent{KeyCode::LeftSuper, false, KeyModifiers::none}});
    CHECK(scene.selection() == std::vector{pyramid, sun}); // command-click adds
    press(harness, {row_center(harness, sun).x, rows.back().max.y + 40.0f}); // empty space clears
    CHECK(scene.selection().empty());
}

TEST_CASE("Delete removes the selected subtree; command-Z brings it back with its selection", "[editor][hierarchy]") {
    Harness harness;
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto ground = find_named(scene, "Ground");
    const auto entities = scene.world().size();
    press(harness, row_center(harness, ground));
    harness.frame(key(KeyCode::Backspace, true));
    harness.frame(key(KeyCode::Backspace, false));
    CHECK_FALSE(scene.world().find(ground));
    CHECK(scene.world().size() == entities - 1);
    CHECK(scene.selection().empty());
    CHECK(scene.dirty());
    harness.frames(1);
    CHECK_FALSE(harness.shell.layout().row(ground));
    chord(harness, {KeyCode::LeftSuper}, KeyCode::Z);
    CHECK(scene.world().find(ground));
    CHECK(scene.selection() == std::vector{ground});
    CHECK_FALSE(scene.dirty());
    chord(harness, {KeyCode::LeftSuper, KeyCode::LeftShift}, KeyCode::Z);
    CHECK_FALSE(scene.world().find(ground));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::edit, "failed") == false);
}

TEST_CASE("Inline renaming takes the keyboard; Backspace edits the name, not the scene", "[editor][hierarchy]") {
    Harness harness;
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto sun = find_named(scene, "Sun");
    const auto at = row_center(harness, sun);
    press(harness, at);
    harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}}); // second click: double-click
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    harness.frames(1);
    REQUIRE(harness.shell.renaming() == sun);
    REQUIRE(harness.shell.ui_wants_text());
    for (int i = 0; i < 3; ++i) { // the whole name is selected; Backspace edits text only
        harness.frame(key(KeyCode::Backspace, true));
        harness.frame(key(KeyCode::Backspace, false));
    }
    for (const auto c : std::string("Lamp")) harness.frame({TextEvent{uint32_t(c)}});
    harness.frame(key(KeyCode::Enter, true));
    harness.frame(key(KeyCode::Enter, false));
    CHECK(scene.world().find(sun));
    CHECK(scene.display_name(sun) == "Lamp");
    CHECK_FALSE(harness.shell.renaming());
    CHECK(scene.undo_label() == "Rename");
    // Undo shortcuts do nothing while a text field has the keyboard.
    click(harness, {harness.shell.layout().camera_speed_min.x + 6.0f,
                    (harness.shell.layout().camera_speed_min.y + harness.shell.layout().camera_speed_max.y) / 2.0f});
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    REQUIRE(harness.shell.ui_wants_text());
    chord(harness, {KeyCode::LeftSuper}, KeyCode::Z);
    CHECK(scene.display_name(sun) == "Lamp");
}

TEST_CASE("Dragging a row onto another reparents it and keeps its world pose", "[editor][hierarchy]") {
    Harness harness;
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto red = find_named(scene, "Red cube"), ground = find_named(scene, "Ground");
    const auto pose = *scene.world().world_matrix(*scene.world().find(red));
    const auto from = row_center(harness, red), to = row_center(harness, ground);
    harness.frame({MouseMoveEvent{from.x, from.y}});
    harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
    for (int step = 1; step <= 8; ++step) { // drag past the threshold, over several frames
        const auto t = float(step) / 8.0f;
        harness.frame({MouseMoveEvent{from.x + 4.0f * t, from.y + (to.y - from.y) * t}});
    }
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    harness.frames(1);
    REQUIRE(scene.record(red));
    CHECK(scene.record(red)->parent == ground);
    const auto moved = *scene.world().world_matrix(*scene.world().find(red));
    for (int i = 0; i < 16; ++i) CHECK(moved.matrix().elements[i] == Approx(pose.matrix().elements[i]).margin(1e-4));
    CHECK(scene.undo_label() == "Move Red cube");
}

TEST_CASE("Opening a scene replaces the editing session: history and selection start empty", "[editor][hierarchy]") {
    Harness harness;
    harness.frames(3);
    auto* first = harness.shell.scene();
    first->select(first->roots().front());
    REQUIRE(first->delete_selection());
    REQUIRE(first->can_undo());
    REQUIRE(harness.shell.open_scene("basic.scene"));
    harness.frames(2);
    auto* second = harness.shell.scene();
    CHECK_FALSE(second->can_undo());
    CHECK(second->selection().empty());
    CHECK_FALSE(second->dirty());
    CHECK(second->world().size() == 6);
    // A failed open keeps the current session.
    REQUIRE(second->create("Kept"));
    CHECK_FALSE(harness.shell.open_scene("nonexistent.scene"));
    CHECK(harness.shell.scene() == second);
    CHECK(second->can_undo());
}

namespace {
TransformComponent transform_of(SceneEditor& scene, EntityId id) {
    auto value = read_component(scene.world(), *scene.world().find(id), ComponentId::transform);
    REQUIRE(value);
    return std::get<TransformComponent>(*value);
}
} // namespace

TEST_CASE("Clicking the viewport picks the nearest object and steps through overlapping ones", "[editor][tools]") {
    Harness harness;
    harness.shell.camera() = EditorCamera::looking_at({0.0f, 2.5f, 7.0f}, {0.0f, 0.0f, 0.0f});
    harness.frames(3);
    auto& scene = *harness.shell.scene();
    const auto pyramid = find_named(scene, "Pyramid"), ground = find_named(scene, "Ground");
    // A point on the pyramid's left, clear of the gizmo that appears at its origin once selected;
    // the ground lies behind it.
    const auto center = on_screen(harness, {-0.3f, 0.05f, 0.0f});
    press(harness, center);
    CHECK(scene.selection() == std::vector{pyramid});
    harness.frames(1);
    CHECK(harness.shell.layout().row(pyramid)); // the hierarchy shows the picked entity
    press(harness, center);
    CHECK(scene.selection() == std::vector{ground}); // the same spot again: the next object behind
    press(harness, center);
    CHECK(scene.selection() == std::vector{pyramid});
    // Command-click adds; clicking empty sky clears.
    harness.frame({KeyEvent{KeyCode::LeftSuper, true, KeyModifiers::super}});
    press(harness, on_screen(harness, {1.6f, -0.15f, 0.0f})); // the blue cube
    harness.frame({KeyEvent{KeyCode::LeftSuper, false, KeyModifiers::none}});
    CHECK(scene.selection().size() == 2);
    press(harness, {center.x, harness.shell.layout().viewport_min.y + 60.0f});
    CHECK(scene.selection().empty());
    // Camera and light icons are clickable.
    REQUIRE_FALSE(harness.shell.layout().icons.empty());
    const auto [icon_entity, icon_at] = harness.shell.layout().icons.front();
    press(harness, icon_at);
    CHECK(scene.selection() == std::vector{icon_entity});
}

TEST_CASE("Inspector drags are one undo step; invalid values are refused with a reason", "[editor][tools]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto pyramid = find_named(scene, "Pyramid");
    scene.select(pyramid);
    harness.frames(2);
    const auto history = scene.history_size();
    const auto before = transform_of(scene, pyramid);
    const auto* field = harness.shell.layout().field("transform.translation.x");
    REQUIRE(field);
    const auto from = ImVec2{(field->min.x + field->max.x) / 2, (field->min.y + field->max.y) / 2};
    drag(harness, from, {from.x + 120.0f, from.y}, 20); // twenty frames of changes
    const auto after = transform_of(scene, pyramid);
    CHECK(after.translation.x > before.translation.x + 0.2f);
    CHECK(after.translation.y == before.translation.y);
    CHECK(scene.history_size() == history + 1);
    CHECK(scene.undo_label() == "Edit Transform");
    REQUIRE(scene.undo());
    CHECK(transform_of(scene, pyramid).translation.x == before.translation.x);

    // A camera whose near clip would pass its far clip is refused, and the reason is shown.
    const auto camera = find_named(scene, "Camera");
    scene.select(camera);
    harness.frames(2);
    const auto* near_field = harness.shell.layout().field("camera.near_clip");
    REQUIRE(near_field);
    const auto near = ImVec2{(near_field->min.x + near_field->max.x) / 2, (near_field->min.y + near_field->max.y) / 2};
    harness.frame({MouseMoveEvent{near.x, near.y}});
    harness.frame({KeyEvent{KeyCode::LeftSuper, true, KeyModifiers::super}}); // command-click types a value
    harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::super}});
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::super}});
    harness.frame({KeyEvent{KeyCode::LeftSuper, false, KeyModifiers::none}});
    REQUIRE(harness.shell.ui_wants_text());
    harness.frame({KeyEvent{KeyCode::LeftSuper, true, KeyModifiers::super}});
    harness.frame(key(KeyCode::A, true)); // select all
    harness.frame(key(KeyCode::A, false));
    harness.frame({KeyEvent{KeyCode::LeftSuper, false, KeyModifiers::none}});
    for (const auto c : std::string("99999")) harness.frame({TextEvent{uint32_t(c)}});
    harness.frame(key(KeyCode::Enter, true));
    harness.frame(key(KeyCode::Enter, false));
    const auto value = std::get<CameraComponent>(*read_component(scene.world(), *scene.world().find(camera), ComponentId::camera));
    CHECK(value.near_clip == Approx(0.1f));
    CHECK(harness.shell.edit_error().find("clip") != std::string::npos);
}

TEST_CASE("Gizmo drags move along an axis as one undo step and respect the hierarchy", "[editor][tools]") {
    Harness harness;
    harness.shell.camera() = EditorCamera::looking_at({0.0f, 2.0f, 5.0f}, {0.0f, 0.0f, 0.0f});
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto pyramid = find_named(scene, "Pyramid");
    scene.select(pyramid);
    harness.frames(2);
    REQUIRE(harness.shell.layout().gizmo_origin);
    const auto origin = *harness.shell.layout().gizmo_origin;
    const auto world = *scene.world().world_matrix(*scene.world().find(pyramid));
    const auto tip = on_screen(harness, world.translation + math::DVec3{1.0, 0.0, 0.0});
    auto direction = ImVec2{tip.x - origin.x, tip.y - origin.y};
    const auto length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    direction = {direction.x / length, direction.y / length};
    // Find the X handle along its screen direction.
    auto grab = std::optional<ImVec2>{};
    for (float distance = 20.0f; distance < 200.0f && !grab; distance += 4.0f) {
        const auto at = ImVec2{origin.x + direction.x * distance, origin.y + direction.y * distance};
        harness.frame({MouseMoveEvent{at.x, at.y}});
        harness.frame();
        if (harness.shell.gizmo_hovered()) grab = at;
    }
    REQUIRE(grab);
    const auto before = transform_of(scene, pyramid);
    const auto history = scene.history_size();
    drag(harness, *grab, {grab->x + direction.x * 60.0f, grab->y + direction.y * 60.0f});
    const auto after = transform_of(scene, pyramid);
    CHECK(after.translation.x > before.translation.x + 0.05f);
    CHECK(after.translation.y == Approx(before.translation.y).margin(1e-4));
    CHECK(after.translation.z == Approx(before.translation.z).margin(1e-4));
    CHECK(scene.history_size() == history + 1);
    CHECK(scene.undo_label() == "Move Pyramid");
    CHECK_FALSE(harness.shell.gizmo_active());
    REQUIRE(scene.undo());
    CHECK(transform_of(scene, pyramid).translation.x == before.translation.x);
}

TEST_CASE("Picking and gizmo drags work alike at every distance from the origin", "[editor][tools][precision]") {
    // #1065: the view, picking, and the gizmo are camera-relative, so an object 100 km out is picked and
    // moved as one at the origin is.
    auto moves = std::vector<math::DVec3>{};
    for (const auto distance : {0.0, 1000.0, 11600.0, 100000.0}) {
        INFO("offset " << distance << " m");
        Harness harness;
        harness.frames(2);
        auto& scene = *harness.shell.scene();
        const auto pyramid = find_named(scene, "Pyramid");
        auto placed = transform_of(scene, pyramid);
        placed.translation += math::DVec3{distance * 0.6, 0.0, -distance * 0.8};
        if (distance > 0.0) REQUIRE(scene.set_component(pyramid, placed));
        const auto at = placed.translation;
        harness.shell.camera() = EditorCamera::looking_at(at + math::DVec3{0.0, 2.0, 5.0}, at);
        scene.clear_selection();
        harness.frames(2);
        // Clicking it selects it.
        press(harness, on_screen(harness, at + math::DVec3{0.0, 0.3, 0.0}));
        harness.frames(1);
        REQUIRE(scene.primary() == pyramid);
        harness.frames(1);
        REQUIRE(harness.shell.layout().gizmo_origin);
        const auto origin = *harness.shell.layout().gizmo_origin;
        // The gizmo sits on it, and its X handle drags it along X.
        const auto shown = on_screen(harness, at);
        CHECK(origin.x == Approx(shown.x).margin(0.5));
        CHECK(origin.y == Approx(shown.y).margin(0.5));
        const auto tip = on_screen(harness, at + math::DVec3{1.0, 0.0, 0.0});
        auto direction = ImVec2{tip.x - origin.x, tip.y - origin.y};
        const auto length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        direction = {direction.x / length, direction.y / length};
        auto grab = std::optional<ImVec2>{};
        for (float step = 20.0f; step < 200.0f && !grab; step += 4.0f) {
            const auto point = ImVec2{origin.x + direction.x * step, origin.y + direction.y * step};
            harness.frame({MouseMoveEvent{point.x, point.y}});
            harness.frame();
            if (harness.shell.gizmo_hovered()) grab = point;
        }
        REQUIRE(grab);
        drag(harness, *grab, {grab->x + direction.x * 60.0f, grab->y + direction.y * 60.0f});
        moves.push_back(transform_of(scene, pyramid).translation - at);
    }
    for (const auto& move : moves) {
        CHECK(move.x > 0.05);
        CHECK(move.x == Approx(moves.front().x).margin(1e-4)); // the same drag moves it the same distance
        CHECK(std::abs(move.y) < 1e-6);
        CHECK(std::abs(move.z) < 1e-6);
    }
}

TEST_CASE("Gizmo matrices become validated local transforms under their parents", "[editor][tools]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    // A parent rotated about Z with nonuniform scale, and an unrotated child.
    REQUIRE(scene.create("Parent"));
    const auto parent = *scene.primary();
    REQUIRE(scene.set_component(parent, TransformComponent{{1, 0, 0}, math::Quat::from_axis_angle({0, 0, 1}, 0.6f), {1, 3, 1}}));
    REQUIRE(scene.create("Child", parent));
    const auto child = *scene.primary();
    const auto world = *scene.world().world_matrix(*scene.world().find(child));
    // Moving in world space is fine: the translation maps back into the parent.
    auto moved = world;
    moved.translation.x += 2.0;
    REQUIRE(harness.shell.apply_world_matrix(child, moved));
    const auto result = *scene.world().world_matrix(*scene.world().find(child));
    CHECK(result.translation.x == Approx(world.translation.x + 2.0).margin(1e-4));
    CHECK(result.translation.y == Approx(world.translation.y).margin(1e-4));
    // Rotating it in world space would need shear from this parent: refused, nothing changes.
    const auto history = scene.history_size();
    const auto sheared = math::Affine::from_matrix(math::Quat::from_axis_angle({1, 0, 0}, 0.7f).to_mat4() * result.matrix());
    CHECK_FALSE(harness.shell.apply_world_matrix(child, sheared));
    CHECK(harness.shell.edit_error().find("shear") != std::string::npos);
    CHECK(scene.history_size() == history);
}

TEST_CASE("Tool keys act only over the viewport and never while typing", "[editor][tools]") {
    Harness harness;
    harness.frames(2);
    const auto& layout = harness.shell.layout();
    const auto center = ImVec2{(layout.viewport_min.x + layout.viewport_max.x) / 2, (layout.viewport_min.y + layout.viewport_max.y) / 2};
    harness.frame({MouseMoveEvent{center.x, center.y}});
    harness.frame(key(KeyCode::E, true, 'e'));
    harness.frame(key(KeyCode::E, false));
    CHECK(harness.shell.gizmo_operation() == GizmoOperation::rotate);
    harness.frame(key(KeyCode::R, true, 'r'));
    harness.frame(key(KeyCode::R, false));
    CHECK(harness.shell.gizmo_operation() == GizmoOperation::scale);
    harness.frame(key(KeyCode::X, true, 'x'));
    harness.frame(key(KeyCode::X, false));
    CHECK(harness.shell.gizmo_local());
    // Over the hierarchy, W does nothing.
    const auto* row = layout.hierarchy_rows.empty() ? nullptr : &layout.hierarchy_rows.front();
    REQUIRE(row);
    harness.frame({MouseMoveEvent{row->min.x + 40.0f, row->min.y + 5.0f}});
    harness.frame(key(KeyCode::W, true, 'w'));
    harness.frame(key(KeyCode::W, false));
    CHECK(harness.shell.gizmo_operation() == GizmoOperation::scale);
    // While typing in a field, even with the pointer over the viewport, W is text.
    click(harness, {layout.camera_speed_min.x + 6.0f, (layout.camera_speed_min.y + layout.camera_speed_max.y) / 2.0f});
    harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    REQUIRE(harness.shell.ui_wants_text());
    harness.frame({MouseMoveEvent{center.x, center.y}});
    harness.frame(key(KeyCode::W, true, 'w'));
    harness.frame(key(KeyCode::W, false));
    CHECK(harness.shell.gizmo_operation() == GizmoOperation::scale);
}
