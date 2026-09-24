#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/null_device.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>

using namespace maya;
using namespace maya::editor;
using Catch::Approx;

namespace {
/// Null backend that records UI encoding and can fail chosen resources.
class EditorDevice final : public NullGraphicsDevice {
public:
    explicit EditorDevice(DeviceOptions options = {3, size_t{16} << 20}) { REQUIRE(initialize(nullptr, options)); }
    ~EditorDevice() override { shutdown(); }

    std::function<bool(const TextureDesc&)> fail_texture;
    std::function<bool(const PipelineDesc&)> fail_pipeline;
    std::vector<ScissorRect> scissors;
    std::vector<uint32_t> sampled; // texture slots bound for sampling
    std::vector<PipelineDesc> pipelines;
    size_t indexed_draws = 0;

protected:
    RhiDiagnostic backend_create_texture(uint32_t slot, const TextureDesc& desc, const void* data) override {
        if (fail_texture && fail_texture(desc)) return {RhiError::out_of_memory, "injected texture failure: " + desc.label};
        return NullGraphicsDevice::backend_create_texture(slot, desc, data);
    }
    RhiDiagnostic backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) override {
        if (fail_pipeline && fail_pipeline(desc)) return {RhiError::shader_compilation, "injected pipeline failure: " + desc.label};
        pipelines.push_back(desc);
        return NullGraphicsDevice::backend_create_pipeline(slot, desc);
    }
    void backend_set_scissor(const ScissorRect& rect) override { scissors.push_back(rect); }
    void backend_set_texture(uint32_t, uint32_t slot) override { sampled.push_back(slot); }
    void backend_draw_indexed(uint32_t slot, IndexType type, uint32_t count, size_t offset, uint32_t instances) override {
        ++indexed_draws;
        NullGraphicsDevice::backend_draw_indexed(slot, type, count, offset, instances);
    }
};

/// A shell on the null device with a 1280x720-point window at 2x (Retina) scale.
struct Harness {
    explicit Harness(bool open_sample = true, DeviceOptions options = {3, size_t{16} << 20})
        : device(options), shell(device, "renderer source", "ui source") {
        if (open_sample) {
            const auto catalog = FileSystem::resolve("samples/basic_scene/assets/catalog.maya");
            REQUIRE(catalog);
            REQUIRE(shell.open_scene(*catalog, catalog->parent_path() / "basic.scene"));
        }
        resize_window(metrics);
    }
    void resize_window(const WindowMetrics& value) {
        metrics = value;
        if (window.valid()) device.destroy(window);
        if (value.framebuffer_width == 0 || value.framebuffer_height == 0) return;
        const auto created = device.create_texture({value.framebuffer_width, value.framebuffer_height,
            Format::bgra8_unorm, TextureUsage::render_target, "window"});
        REQUIRE(created);
        window = created.handle;
    }
    /// One host frame: route and build the UI, then render it into the window, as the editor does.
    void frame(std::vector<InputEvent> events = {}, float delta_time = 1.0f / 60.0f) {
        shell.update(delta_time, events, metrics);
        if (const auto capture = shell.take_capture_request()) captured = *capture;
        REQUIRE_FALSE(device.begin_frame());
        if (window.valid()) {
            const auto error = shell.render(window);
            INFO(error.message);
            REQUIRE_FALSE(error);
        }
        REQUIRE_FALSE(device.end_frame());
    }
    void frames(int count) { for (int i = 0; i < count; ++i) frame(); }
    ImVec2 viewport_center() const {
        const auto& layout = shell.layout();
        return {(layout.viewport_min.x + layout.viewport_max.x) / 2, (layout.viewport_min.y + layout.viewport_max.y) / 2};
    }
    template<class F> auto with_context(F&& inspect) {
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(shell.context());
        auto result = inspect();
        ImGui::SetCurrentContext(previous);
        return result;
    }

    WindowMetrics metrics{1280, 720, 2560, 1440};
    EditorDevice device;
    EditorShell shell;
    TextureHandle window;
    bool captured = false;
};

/// Moves the pointer in one frame and presses a button in the next, as a person would. Hover state
/// is decided by the frame the pointer moved in.
void click(Harness& harness, ImVec2 at, MouseButton button = MouseButton::left) {
    harness.frame({MouseMoveEvent{at.x, at.y}});
    harness.frame({MouseButtonEvent{button, true, KeyModifiers::none}});
}
std::string active_text(Harness& harness) {
    return harness.with_context([] {
        const auto& g = *ImGui::GetCurrentContext();
        return g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId ? std::string(g.InputTextState.TextA.Data) : std::string{};
    });
}
std::vector<InputEvent> key(KeyCode code, bool down, uint32_t text = 0) {
    auto events = std::vector<InputEvent>{KeyEvent{code, down, KeyModifiers::none}};
    if (down && text) events.push_back(TextEvent{text});
    return events;
}
bool same(const math::Vec3& a, const math::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool logged(const DiagnosticLog& log, DiagnosticSource source, std::string_view text) {
    return std::ranges::any_of(log.entries(), [&](const DiagnosticEntry& entry) {
        return entry.source == source && entry.message.find(text) != std::string::npos;
    });
}
} // namespace

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
    CHECK(math::Vec3::dot((moved - start).normalized(), harness.shell.camera().forward()) == Approx(1.0f));

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
    SECTION("A missing scene leaves an empty editor with an explanation") {
        Harness harness(false);
        CHECK_FALSE(harness.shell.open_scene("/nonexistent/catalog.maya", "/nonexistent/basic.scene"));
        harness.frames(2);
        CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::scene, "Cannot read asset catalog"));
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
