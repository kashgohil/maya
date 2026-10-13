// W1, the reference world (#1069, docs/acceptance.md#w1): the world maya_w1 assembles (CTest fixture w1_project,
// with RGBA8 textures, in the build folder), streamed as the player streams it, its five named views against the
// references the project owner approved, and the same views in the editor's Play.

#include "w1.hpp"
#include "maya/assets/generator_file.hpp"
#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/png.hpp"
#include "support/w1_play.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace maya;
using namespace maya::test;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t width = 640, height = 360;

fs::path w1_folder() { return fs::path(MAYA_W1_PROJECT); }
bool assembled() { return fs::is_regular_file(w1_folder() / "w1.world"); }

WorldDocument read_w1() { return read_world_file(w1_folder() / "w1.world"); }
} // namespace

TEST_CASE("W1 assembles into a world of its recipe", "[w1]") {
    if (!assembled()) SKIP("W1 is not assembled; fetch the samples (tools/fetch_world_samples.sh, tools/fetch_render_samples.sh) and run the w1_project fixture");
    const auto world = read_w1();
    CHECK(world.cell_size == 128.0);
    for (const auto& cell : world.cells) {
        CHECK(cell.index.x >= w1::first_cell);
        CHECK(cell.index.x <= w1::last_cell);
        CHECK(cell.index.z >= w1::first_cell);
        CHECK(cell.index.z <= w1::last_cell);
    }
    const auto any = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    auto whole = load_world(w1_folder() / "w1.world", any);
    REQUIRE(whole);
    // S1's camera is the persistent part's first camera: the one the player shows.
    auto persistent = load_scene_file(w1_folder() / world.persistent, any);
    REQUIRE(persistent);
    auto first_camera = std::optional<std::string>{};
    auto generators = 0, exclusions = 0, terrains = 0;
    for (const auto& entity : persistent.document.entities) {
        auto name = std::string{};
        auto camera = false;
        for (const auto& component : entity.components) {
            if (const auto* named = std::get_if<NameComponent>(&component)) name = named->value;
            camera = camera || std::holds_alternative<CameraComponent>(component);
            generators += std::holds_alternative<GeneratorComponent>(component);
            exclusions += std::holds_alternative<ScatterExclusionComponent>(component);
            terrains += std::holds_alternative<TerrainComponent>(component);
        }
        if (camera && !first_camera) first_camera = name;
    }
    CHECK(first_camera == std::string(w1::s1_camera_name));
    // At rest where S1 starts, so the player's loading screen streams around the start, before the clip poses it.
    for (const auto& entity : persistent.document.entities)
        for (const auto& component : entity.components)
            if (const auto* named = std::get_if<NameComponent>(&component); named && named->value == w1::s1_camera_name)
                for (const auto& other : entity.components)
                    if (const auto* transform = std::get_if<TransformComponent>(&other)) {
                        CHECK(std::abs(transform->translation.x - w1::s1_point(0).x) < 1e-3);
                        CHECK(std::abs(transform->translation.z - w1::s1_point(0).z) < 1e-3);
                    }
    CHECK(terrains == 1);
    CHECK(generators == 4);
    CHECK(exclusions == 2);
    // The bodies: 400 resting rocks and 20 rolling boulders, in cells along S1's walk.
    auto bodies = 0;
    for (const auto& [index, cell] : whole.cells)
        for (const auto& entity : cell.entities)
            for (const auto& component : entity.components) bodies += std::holds_alternative<RigidBodyComponent>(component);
    CHECK(bodies == 420);
    // Every scatter generator's spacing is one scatter takes (#1093), and the soak's covers its 2 x 2 cells.
    for (const auto name : {"boulders", "rocks", "soak_rocks", "deadwood"}) {
        auto file = std::ifstream(w1_folder() / "generators" / (std::string(name) + ".generator"));
        auto read = read_generator_file(file);
        INFO(name);
        REQUIRE(read);
        CHECK(scatter_spacing_valid(read.generator.spacing));
        if (std::string_view(name) == "soak_rocks") {
            CHECK(read.generator.region_min_x == w1::soak_first_cell.x);
            CHECK(read.generator.region_max_z == w1::soak_last_cell.z);
        }
    }
    // S1: 3.36 km in 9,600 ticks, the walk's 960 m with three turn-backs, then 2,400 m of flyover.
    auto walked = 0.0, flown = 0.0;
    auto turns = 0;
    for (uint32_t tick = 1; tick <= w1::s1_ticks; ++tick) {
        const auto a = w1::s1_point(tick - 1), b = w1::s1_point(tick);
        if (b.on_foot) walked += std::hypot(b.x - a.x, b.z - a.z);
        else flown += std::hypot(b.x - a.x, b.z - a.z); // the flyover starts where the walk ends
        turns += a.on_foot && b.on_foot && a.forward.x * b.forward.x < 0.0f;
    }
    CHECK(std::abs(walked - 960.0) < 1e-6);
    CHECK(std::abs(flown - 2400.0) < 1e-3);
    CHECK(turns == 3);
}

TEST_CASE("W1's watch camera follows CesiumMan into his cell, and holds still while the cell is out", "[w1]") {
    if (!assembled()) SKIP("W1 is not assembled; fetch the samples and run the w1_project fixture");
    // The cross-cell reference (#1069): the watch camera, in the persistent part, names CesiumMan's walk loop in
    // cell 42_43 by persistent ID, a weak reference that resolves while the cell is in.
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        const auto w1 = w1_from_sources(device, w1_folder());
        auto& world = w1->session->world();
        const auto watch = world.find(w1::watch_camera());
        REQUIRE(watch);
        const auto facing = [&] { // a root: its rotation is its world rotation
            auto rotation = math::Quat{};
            world.with<TransformComponent>(*watch, [&](const TransformComponent& transform) { rotation = transform.rotation; });
            return rotation.rotate({0.0f, 0.0f, -1.0f});
        };
        const auto walker = [&]() -> std::optional<math::DVec3> {
            auto found = std::optional<math::DVec3>{};
            world.for_each<NameComponent>([&](EntityHandle entity, const NameComponent& name) {
                if (name.value == "Walk loop") found = world.world_matrix(entity)->translation;
            });
            return found;
        };
        const auto start = w1::s1_point(0);
        w1->settle_at({start.x, 0.0, start.z});
        w1->run_to(120);
        // Looking at him (a metre above his loop), as he walks.
        const auto aim = [&] {
            const auto at = *walker(), from = world.world_matrix(*watch)->translation;
            return math::Vec3{float(at.x - from.x), float(at.y + 1.0 - from.y), float(at.z - from.z)}.normalized();
        };
        CHECK(math::Vec3::dot(facing(), aim()) > 0.999f);
        const auto before = facing();
        w1->run_to(240);
        CHECK(math::Vec3::dot(facing(), aim()) > 0.999f);
        CHECK(math::Vec3::dot(facing(), before) < 0.9999f); // it turned
        // His cell out (the flyover's end, 2.6 km away): the reference does not resolve, and the camera holds still.
        const auto far = w1::s1_point(w1::s1_ticks);
        w1->settle_at({far.x, 0.0, far.z});
        REQUIRE(w1->cells->state({42, 43}) == CellState::unloaded);
        CHECK_FALSE(walker());
        const auto held = facing();
        w1->run_to(300);
        CHECK(math::Vec3::dot(facing(), held) > 0.999999f);
        // Back: it resolves again, to the walk loop as it was when the cell went out, and follows him on.
        w1->settle_at({start.x, 0.0, start.z});
        REQUIRE(walker());
        w1->run_to(320);
        CHECK(math::Vec3::dot(facing(), aim()) > 0.999f);
    }
    device.shutdown();
}

TEST_CASE("W1's named views render as approved", "[visual][gpu][w1][acceptance]") {
    if (!assembled()) SKIP("W1 is not assembled; fetch the samples and run the w1_project fixture");
    Gpu gpu;
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    const auto assets = w1_assets(gpu.device, w1_folder());
    for (const auto& view : w1::views()) { // each from a fresh start, as a Play that goes there does
        const auto w1 = w1_from_sources(gpu.device, w1_folder(), assets);
        w1->start(width, height);
        w1->settle_at(view.from);
        images.emplace_back(std::string(view.name), w1->render(gpu, view.camera, width, height));
    }
    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/w1", fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs",
                            images);
}

TEST_CASE("W1's views look the same in the editor's Play as in the player", "[visual][gpu][w1][acceptance][editor-play]") {
    if (!assembled()) SKIP("W1 is not assembled; fetch the samples and run the w1_project fixture");
    // The editor on Metal plays W1 in its Game view, held after its first ticks. For each view, S1's camera, the
    // one Play shows, is put where the view's camera is, and the editor streams around it frame by frame until
    // nothing more changes; the player's side starts the same way and settles there.
    Gpu gpu;
    auto& device = gpu.device;
    const auto read = [](const char* relative) {
        auto file = std::ifstream(*FileSystem::resolve(relative), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    auto shell = editor::EditorShell(device, FileSystem::read_text("resources/shaders/metal/renderer.metal"),
                                     FileSystem::read_text("resources/shaders/metal/editor_ui.metal"), {},
                                     {read("resources/fonts/Inter-Regular.ttf"), read("resources/fonts/Inter-SemiBold.ttf"),
                                      read("resources/fonts/GeistMono-Regular.ttf"), read("resources/fonts/Phosphor-Light.ttf")});
    REQUIRE(shell.open_project(w1_folder()));
    REQUIRE(shell.cells()); // its startup world
    const auto metrics = WindowMetrics{1280, 800, 1280, 800};
    auto window = device.create_texture({metrics.framebuffer_width, metrics.framebuffer_height, Format::bgra8_unorm,
                                         TextureUsage::render_target | TextureUsage::readback, "window"});
    REQUIRE(window);
    const auto frame = [&] {
        shell.update(1.0f / 60.0f, {}, metrics);
        REQUIRE_FALSE(device.begin_frame());
        const auto error = shell.render(window.handle);
        INFO(error.message);
        REQUIRE_FALSE(error);
        REQUIRE_FALSE(device.end_frame());
    };
    // The editor streams frame by frame within its budgets: frames until a frame changes nothing.
    const auto settled = [&] {
        const auto* cells = shell.play_streamer();
        const auto* terrain = shell.terrain();
        const auto* scatter = shell.play_scatter();
        const auto& c = cells->stats();
        const auto& t = terrain->stats();
        return c.cells[size_t(CellState::loading)] == 0 && c.cells[size_t(CellState::activating)] == 0 &&
               c.cells[size_t(CellState::deactivating)] == 0 && c.waiting == 0 && t.loading == 0 && t.activated == 0 && t.deactivated == 0 &&
               t.uploaded == 0 && scatter->stats().cells_waiting == 0 && scatter->stats().cells_loading == 0;
    };
    const auto settle = [&] {
        auto quiet = 0;
        for (int i = 0; i < 4000 && quiet < 3; ++i) {
            frame();
            shell.assets()->wait_idle();
            quiet = settled() ? quiet + 1 : 0;
        }
        INFO("cells loading " << shell.play_streamer()->stats().cells[size_t(CellState::loading)] << ", activating "
                              << shell.play_streamer()->stats().cells[size_t(CellState::activating)] << ", deactivating "
                              << shell.play_streamer()->stats().cells[size_t(CellState::deactivating)] << ", waiting "
                              << shell.play_streamer()->stats().waiting << "; tiles loading " << shell.terrain()->stats().loading << ", activated "
                              << shell.terrain()->stats().activated << ", deactivated " << shell.terrain()->stats().deactivated << ", uploaded "
                              << shell.terrain()->stats().uploaded << "; scatter waiting " << shell.play_scatter()->stats().cells_waiting
                              << ", loading " << shell.play_scatter()->stats().cells_loading);
        REQUIRE(quiet == 3);
        preload_render_assets(shell.play_session()->world(), *shell.assets());
        shell.assets()->wait_idle();
        for (int i = 0; i < 4; ++i) frame(); // what the first frames asked for, loaded and drawn
    };
    for (int i = 0; i < 3; ++i) frame();
    const auto player_assets = w1_assets(device, w1_folder()); // the player's side's, kept between the views
    {
        // With the Scene view shown and the editor camera far from the play camera, the cells around the play camera
        // still go in: Play's terrain streams around the play camera too, so they are not held waiting for it.
        REQUIRE(shell.start_play());
        shell.set_game_view(false);
        shell.camera() = editor::EditorCamera({7800.0, 300.0, 4400.0});
        settle();
        CHECK(shell.play_streamer()->stats().cells[size_t(CellState::active)] > 0);
        shell.stop_play();
    }
    for (const auto& view : w1::views()) {
        INFO(view.name);
        REQUIRE(shell.start_play());
        shell.set_game_view(true);
        auto* play = shell.play_session();
        REQUIRE(play);
        play->clock().pause();
        // Everything loaded before the first tick, as the player's side has it, so each frame draws all it sees: a
        // level of detail depends on the frames before (docs/renderer.md#levels-of-detail).
        preload_render_assets(play->world(), *shell.assets());
        shell.assets()->wait_idle();
        while (play->clock().tick() < W1Play::start_ticks) { // as the player's side starts
            play->clock().step();
            frame();
        }
        // S1's camera where the view's is.
        auto& world = play->world();
        const auto s1 = world.find(*play->camera());
        const auto pose = world.find(view.camera);
        REQUIRE(s1);
        REQUIRE(pose);
        auto commands = world.commands();
        auto placed = TransformComponent{};
        REQUIRE(world.with<TransformComponent>(*pose, [&](const TransformComponent& value) { placed = value; }));
        commands.set_transform(*s1, placed);
        REQUIRE(world.commit(commands));
        settle();
        REQUIRE(shell.showing_game());
        auto pixels = std::vector<std::byte>{};
        device.wait_idle();
        REQUIRE_FALSE(device.read_texture(window.handle, pixels));
        const auto& layout = shell.layout();
        const auto view_width = shell.viewport().width(), view_height = shell.viewport().height();
        const auto left = uint32_t(layout.viewport_min.x), top = uint32_t(layout.viewport_min.y);
        // The player's side, from the view's own camera, at the Game view's size.
        const auto player = [&] {
            const auto w1 = w1_from_sources(device, w1_folder(), player_assets);
            w1->start(view_width, view_height);
            w1->settle_at(view.from);
            return w1->render(gpu, view.camera, view_width, view_height);
        }();
        // Between the Game view's tool bar and its "Click to play" hint, every pixel the same within one level, but for
        // a handful (all but 0.01%, as for the package): the editor streams frame by frame, the player behind a loading
        // screen, and an unoptimized build has shown 2 of 279,000 pixels differing in the flyover.
        auto differing = size_t{0}, compared = size_t{0};
        auto editor = RgbImage{view_width, view_height, {}};
        for (uint32_t y = 0; y < view_height; ++y)
            for (uint32_t x = 0; x < view_width; ++x) {
                const auto* p = pixels.data() + (size_t(top + y) * metrics.framebuffer_width + left + x) * 4; // BGRA
                editor.rgb.insert(editor.rgb.end(), {uint8_t(p[2]), uint8_t(p[1]), uint8_t(p[0])});
                if (y < view_height / 8 || y >= view_height * 7 / 8) continue;
                const auto* q = player.rgb.data() + (size_t(y) * view_width + x) * 3;
                ++compared;
                if (std::abs(int(p[2]) - int(q[0])) > 1 || std::abs(int(p[1]) - int(q[1])) > 1 || std::abs(int(p[0]) - int(q[2])) > 1) ++differing;
            }
        INFO(differing << " of " << compared << " pixels differ");
        if (differing > 0 && std::getenv("MAYA_W1_DIFF")) {
            write_png(fs::path(std::getenv("MAYA_W1_DIFF")) / (std::string(view.name) + "-editor.png"), editor);
            write_png(fs::path(std::getenv("MAYA_W1_DIFF")) / (std::string(view.name) + "-player.png"), player);
        }
        CHECK(differing * 10000 <= compared);
        shell.stop_play();
    }
    device.destroy(window.handle);
}
