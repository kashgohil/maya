#include <catch2/catch_test_macros.hpp>
#include "maya/core/engine.hpp"
#include "maya/platform/desktop_application.hpp"
#include "maya/platform/window.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/file_system.hpp"
#include "maya/platform/input.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#if MAYA_TEST_PLAYER
#include "player_application.hpp"
#endif
#if MAYA_TEST_EDITOR
#include "editor_application.hpp"
#endif

namespace {
class FailingApplication final : public maya::Application {
public:
    bool on_start(maya::GraphicsDevice& device) override {
        // Acquire a real GPU resource before failing to exercise rollback.
        REQUIRE(device.create_buffer({256, maya::BufferUsage::uniform, "rollback"}));
        return false;
    }
};

constexpr auto triangle_shader = R"(
    #include <metal_stdlib>
    using namespace metal;
    vertex float4 vertexMain(uint id [[vertex_id]]) {
        const float2 positions[3] = {float2(-.5,-.5),float2(.5,-.5),float2(0,.5)};
        return float4(positions[id%3],0.5,1);
    }
    fragment float4 fragmentMain() { return float4(1,0,1,1); }
)";

maya::PipelineHandle surface_pipeline(maya::GraphicsDevice& device) {
    auto created = device.create_pipeline({triangle_shader, "vertexMain", "fragmentMain",
        {device.surface_format()}, maya::Format::undefined, {}, maya::CullMode::none,
        maya::Winding::counter_clockwise, "surface triangle"});
    INFO(created.diagnostic.message);
    REQUIRE(created);
    return created.handle;
}
maya::RenderPassDesc surface_pass(maya::TextureHandle texture) {
    return {{{texture, maya::LoadAction::clear, maya::StoreAction::store, {0.1, 0.1, 0.1, 1.0}}}, {}, "surface"};
}
}

TEST_CASE("Launch arguments separate an application's own arguments from the host's", "[desktop]") {
    char program[] = "maya_player", project[] = "My Game", scene[] = "levels/a.scene", smoke[] = "--smoke",
         count[] = "12", help[] = "--help", zero[] = "0";
    char* argv[] = {program, project, smoke, count, scene, help};
    const auto split = maya::split_arguments(6, argv);
    CHECK(split.positional == std::vector<std::string>{"My Game", "levels/a.scene"});
    REQUIRE(split.host_count() == 4);
    CHECK(split.host == std::vector<char*>{program, smoke, count, help, nullptr});
    // A count after --smoke stays with it, even an invalid one, so the host can refuse it.
    char* invalid[] = {program, smoke, zero};
    CHECK(maya::split_arguments(3, invalid).positional.empty());
    // --smoke without a count leaves the next argument positional.
    char* bare[] = {program, smoke, project};
    CHECK(maya::split_arguments(3, bare).positional == std::vector<std::string>{"My Game"});
}

TEST_CASE("Desktop sessions survive partial startup and repeated shutdown", "[desktop]") {
    maya::Window window(320, 240, "Maya lifecycle test");
    REQUIRE(window.get_native_handle() != nullptr);
    maya::Engine engine;
    CHECK_FALSE(engine.initialize(maya::GraphicsDevice::create_default(),
        window.get_native_handle(), std::make_unique<FailingApplication>()));
    for (int session = 0; session < 3; ++session) {
        REQUIRE(engine.initialize(maya::GraphicsDevice::create_default(),
            window.get_native_handle(), std::make_unique<maya::Application>()));
        REQUIRE(engine.tick(1.0f / 60.0f, false));
        engine.shutdown();
        engine.shutdown();
    }
}

TEST_CASE("Failed and overlapping windows do not invalidate surviving windows", "[desktop]") {
    for (int session = 0; session < 2; ++session) {
        maya::Window first(320, 240, "Maya window lifecycle test");
        REQUIRE(first.get_native_handle() != nullptr);
        {
            maya::Window invalid(0, 0, "Invalid window");
            CHECK(invalid.get_native_handle() == nullptr);
            CHECK(invalid.should_close());
        }
        {
            maya::Window second(320, 240, "Maya second window");
            REQUIRE(second.get_native_handle() != nullptr);
        }
        first.poll_events();
        CHECK(first.get_native_handle() != nullptr);
        CHECK_FALSE(first.should_close());
        CHECK(first.framebuffer_size().first > 0);
        // A close request can be withdrawn (an application asking about unsaved changes) and repeated.
        first.set_should_close(true);
        CHECK(first.should_close());
        first.set_should_close(false);
        first.poll_events();
        CHECK_FALSE(first.should_close());
    }
}

TEST_CASE("Surface presentation follows resizes and is independent of offscreen passes", "[desktop]") {
    maya::Window window(320, 240, "Maya surface test");
    REQUIRE(window.get_native_handle());
    maya::MetalDevice device;
    for (int session = 0; session < 2; ++session) {
        REQUIRE(device.initialize(window.get_native_handle()));
        REQUIRE(device.surface_format() == maya::Format::bgra8_unorm);
        // The layer shows its drawable at the display's density (2 on Retina), not as 1x content.
        const auto [points_width, points_height] = window.window_size();
        CHECK(device.surface_scale() == double(window.framebuffer_size().first) / double(points_width));
        CHECK(device.surface_scale() >= 1.0);
        const auto pipeline = surface_pipeline(device);
        const auto offscreen = device.create_texture({64, 64, maya::Format::bgra8_unorm,
            maya::TextureUsage::render_target, "offscreen"});
        REQUIRE(offscreen);
        for (const auto [width, height] : {std::pair{320u, 240u}, {200u, 100u}, {640u, 480u}}) {
            device.resize(width, height);
            device.resize(0, 0); // minimized/zero-sized notifications are ignored
            for (int frame = 0; frame < 10; ++frame) {
                window.poll_events();
                REQUIRE_FALSE(device.begin_frame());
                // Offscreen work never depends on acquiring the drawable.
                REQUIRE_FALSE(device.begin_render_pass(surface_pass(offscreen.handle)));
                REQUIRE_FALSE(device.set_pipeline(pipeline));
                REQUIRE_FALSE(device.draw(3));
                REQUIRE_FALSE(device.end_render_pass());
                if (frame % 3 != 2) { // some frames skip presentation entirely
                    const auto surface = device.acquire_surface();
                    INFO(surface.diagnostic.message);
                    REQUIRE(surface);
                    CHECK(surface.target.width == width);
                    CHECK(surface.target.height == height);
                    // The drawable belongs to the window, not to tracked allocations: only the offscreen
                    // texture is counted.
                    CHECK(device.stats().texture_bytes == 64 * 64 * 4);
                    REQUIRE_FALSE(device.begin_render_pass(surface_pass(surface.target.texture)));
                    REQUIRE_FALSE(device.set_pipeline(pipeline));
                    REQUIRE_FALSE(device.draw(3));
                    REQUIRE_FALSE(device.end_render_pass());
                }
                REQUIRE_FALSE(device.end_frame());
            }
        }
        device.wait_idle();
        CHECK(device.take_gpu_errors().empty());
        CHECK(device.stats().completed_frames == 30);
        // Shut down with an acquired, unpresented surface and an open pass.
        REQUIRE_FALSE(device.begin_frame());
        const auto surface = device.acquire_surface();
        REQUIRE(surface);
        REQUIRE_FALSE(device.begin_render_pass(surface_pass(surface.target.texture)));
        device.shutdown();
        CHECK(device.native_texture_count() == 0);
    }
}

TEST_CASE("Presented frames report when the display showed them, a refresh period apart", "[desktop]") {
    maya::Window window(160, 120, "Maya present timing test");
    REQUIRE(window.get_native_handle());
    window.set_floating(true); // a covered window's frames are not shown when expected
    maya::MetalDevice device;
    REQUIRE(device.initialize(window.get_native_handle()));
    CHECK(device.present_timing_supported());
    const auto refresh = device.display_refresh_rate();
    REQUIRE(refresh);
    CHECK(*refresh >= 30.0);
    const auto [width, height] = window.framebuffer_size();
    device.resize(uint32_t(width), uint32_t(height));
    const auto pipeline = surface_pipeline(device);
    const auto first = device.stats().submitted_frames + 1;
    constexpr int frames = 30;
    for (int frame = 0; frame < frames; ++frame) {
        window.poll_events();
        REQUIRE_FALSE(device.begin_frame());
        const auto surface = device.acquire_surface();
        REQUIRE(surface);
        REQUIRE_FALSE(device.begin_render_pass(surface_pass(surface.target.texture)));
        REQUIRE_FALSE(device.set_pipeline(pipeline));
        REQUIRE_FALSE(device.draw(3));
        REQUIRE_FALSE(device.end_render_pass());
        REQUIRE_FALSE(device.end_frame());
    }
    device.wait_idle();
    // The last frames are shown at later refreshes. Presented frames report, shown or not, except that
    // Metal occasionally never reports one (about one run in four here): such frames are unreported.
    auto presents = std::vector<maya::PresentTiming>{};
    for (int attempt = 0; attempt < 1500 && presents.size() < frames; ++attempt) {
        window.poll_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (const auto& present : device.take_present_timings()) presents.push_back(present);
    }
    CHECK(presents.size() >= frames - 2);
    CHECK(presents.size() <= frames);
    std::ranges::sort(presents, {}, &maya::PresentTiming::frame);
    REQUIRE_FALSE(presents.empty());
    CHECK(presents.front().frame >= first);
    CHECK(presents.back().frame < first + frames);
    CHECK(std::ranges::adjacent_find(presents, {}, &maya::PresentTiming::frame) == presents.end()); // one report each
    auto previous = std::optional<double>{};
    auto intervals = std::vector<double>{};
    for (const auto& present : presents) {
        if (!present.presented) continue;
        // Shown in order on the host clock. Two frames can share a refresh (e.g. while the new window
        // first appears), so single intervals may be 0; the typical one is a refresh period.
        if (previous) {
            CHECK(*present.presented >= *previous);
            intervals.push_back(*present.presented - *previous);
        }
        previous = present.presented;
    }
    // Submitted faster than the display refreshes, drawables are replaced before they are shown and
    // report so; how many depends on timing (fewer are shown in slower builds), so only enough shown
    // frames to judge their intervals are required.
    CHECK(intervals.size() >= 4);
    std::ranges::sort(intervals);
    REQUIRE_FALSE(intervals.empty());
    const auto median = intervals[intervals.size() / 2];
    CHECK(median > 0.75 / *refresh);
    CHECK(median < 1.25 / *refresh);
    device.shutdown();
}

TEST_CASE("Metal keeps encoded mesh resources alive after the final asset lease is released", "[desktop][assets]") {
    maya::Window window(320,240,"Maya asset retirement test");
    REQUIRE(window.get_native_handle());
    maya::MetalDevice device;
    REQUIRE(device.initialize(window.get_native_handle()));
    const auto source=maya::FileSystem::resolve("samples/basic_scene/assets/pyramid.obj");
    REQUIRE(source);
    maya::AssetRegistry registry(source->parent_path(),std::make_unique<maya::FileAssetProvider>(device));
    const auto reference=maya::AssetRef<maya::MeshAsset>{{0x6d617961,1}};
    REQUIRE_FALSE(registry.register_asset(reference,"pyramid.obj"));
    const auto pipeline=surface_pipeline(device);
    const auto baseline=device.native_buffer_count(); // per-frame upload buffers
    for (int frame=0;frame<3;++frame) {
        auto asset=registry.acquire(reference); REQUIRE(asset);
        REQUIRE_FALSE(device.begin_frame());
        const auto surface=device.acquire_surface(); REQUIRE(surface);
        REQUIRE_FALSE(device.begin_render_pass(surface_pass(surface.target.texture)));
        REQUIRE_FALSE(device.set_pipeline(pipeline));
        REQUIRE_FALSE(asset.lease.value().mesh().draw());
        asset={}; CHECK(registry.evict_unused() == 1);
        CHECK(device.stats().buffers == 0); // handles revoked
        CHECK(device.native_buffer_count() == baseline + 2); // the encoding frame still owns the native buffers
        REQUIRE_FALSE(device.end_render_pass());
        REQUIRE_FALSE(device.end_frame());
        device.wait_idle();
        CHECK(device.native_buffer_count() == baseline);
    }
    CHECK(device.take_gpu_errors().empty());
    device.shutdown(); // drains submitted work before destroying the device
}

TEST_CASE("Player and editor render their offscreen views through window resizes", "[desktop][renderer]") {
    auto applications = std::vector<std::pair<const char*, std::unique_ptr<maya::Application>(*)()>>{};
#if MAYA_TEST_PLAYER
    applications.emplace_back("player", [] { return maya::player::create_player_application(); });
#endif
#if MAYA_TEST_EDITOR
    applications.emplace_back("editor", [] { return maya::editor::create_editor_application(); });
#endif
    if (applications.empty()) SKIP("Player and editor targets are not built");
    for (const auto& [name, create] : applications) {
        INFO(name);
        maya::Window window(320, 240, "Maya view resize test");
        REQUIRE(window.get_native_handle());
        maya::Engine engine;
        REQUIRE(engine.initialize(std::make_unique<maya::MetalDevice>(), window.get_native_handle(), create()));
        // Each size reallocates the view once; zero sizes (minimized) are ignored.
        for (const auto [width, height] : {std::pair{320u, 240u}, {640u, 200u}, {0u, 0u}, {90u, 300u}, {1u, 1u}}) {
            REQUIRE(engine.resize(width, height));
            for (int frame = 0; frame < 5; ++frame) {
                window.poll_events();
                // As the desktop host does: publish window metrics, tick, then clear the frame's events.
                const auto [points_width, points_height] = window.window_size();
                const auto scale = float(window.framebuffer_size().first) / float(std::max(points_width, 1));
                maya::Input::instance().set_window_metrics({float(width) / scale, float(height) / scale, width, height});
                REQUIRE(engine.tick(1.0f / 60.0f, false));
                maya::Input::instance().update();
            }
        }
        maya::Input::instance().reset();
        engine.shutdown();
    }
}
