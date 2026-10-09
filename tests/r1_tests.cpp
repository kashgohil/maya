// R1, the realistic reference environment (#1040, docs/acceptance.md#r1): its five named views and frames along
// its camera path, against the references the project owner approved, from the project maya_r1 assembles in
// the build folder (CTest fixture r1_project, with RGBA8 textures).

#include "r1.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/script_assets.hpp"
#include "editor_harness.hpp"
#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "support/metal_view.hpp"
#include <array>
#include <sstream>
#include <catch2/catch_test_macros.hpp>
#include "maya/core/system_info.hpp"
#include <fstream>
#include <iterator>

using namespace maya;
using namespace maya::test;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t width = 640, height = 360;

fs::path r1_folder() { return fs::path(MAYA_R1_PROJECT); }

/// R1's scene in a play session held at a tick, as the player plays it.
struct R1 {
    std::unique_ptr<AssetRegistry> registry;
    std::unique_ptr<PlaySession> session;
    SkinBindingCache skins;
    explicit R1(GraphicsDevice& device, const math::DVec3& offset = {}) {
        auto opened = open_project(r1_folder());
        REQUIRE(opened);
        auto assets = open_project_assets(opened.project,
                                          std::make_unique<FileAssetProvider>(device, std::make_shared<CookCache>(cook_cache_folder(opened.project))));
        REQUIRE(assets);
        registry = std::move(assets.registry);
        const auto context = asset_property_context(*registry);
        auto loaded = load_scene_file(*opened.project.startup_scene, context);
        REQUIRE(loaded);
        shift_roots(loaded.document, offset); // the origin-offset sweep (#1065)
        auto started = PlaySession::start(std::move(loaded.document), context,
                                          play_systems(registry_script_sources(*registry), registry_animation_clips(*registry)));
        INFO(started.error);
        REQUIRE(started);
        session = std::move(started.session);
        session->clock().pause();
    }
    /// Runs ticks until `tick` ticks have completed.
    void run_to(uint64_t tick) {
        while (session->clock().tick() < tick) {
            session->clock().step();
            const auto frame = session->update(0.0);
            REQUIRE(frame.error.empty());
            for (const auto& message : frame.messages) UNSCOPED_INFO(message.text);
            REQUIRE(frame.messages.empty());
        }
    }
    RgbImage render(Gpu& gpu, EntityId camera) {
        const auto& world = session->world();
        const auto handle = world.find(camera);
        REQUIRE(handle);
        auto view = extract_render_view(world, *handle, width, height);
        REQUIRE(view);
        auto options = RenderExtractOptions{};
        options.skins = &skins;
        return gpu.render(world, *registry, *view, options);
    }
    EntityId path_camera() const {
        auto found = std::optional<EntityId>{};
        session->world().for_each<NameComponent, CameraComponent>([&](EntityHandle entity, const NameComponent& name, const CameraComponent&) {
            if (name.value == r1::path_camera_name) found = session->world().persistent_id(entity);
        });
        REQUIRE(found);
        return *found;
    }
};
bool assembled() { return fs::is_regular_file(r1_folder() / "r1.scene"); }
} // namespace

TEST_CASE("R1's named views and frames along its camera path render as approved", "[visual][gpu][r1]") {
    if (!assembled()) SKIP("R1 is not assembled; fetch the samples (tools/fetch_render_samples.sh) and run the r1_project fixture");
    Gpu gpu;
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    {
        auto r1 = R1(gpu.device);
        CHECK(r1.session->camera() == r1.path_camera()); // the player follows the path
        r1.run_to(1); // the first tick poses the clips' starts
        for (const auto& view : r1::views()) images.emplace_back("view-" + std::string(view.name), r1.render(gpu, view.camera));
        for (const auto tick : {1u, 150u, 300u, 450u}) {
            r1.run_to(tick);
            images.emplace_back("path-" + std::to_string(tick), r1.render(gpu, r1.path_camera()));
        }
    }
    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/r1", fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs",
                            images);
}

TEST_CASE("R1 renders the same at every distance from the origin", "[visual][gpu][r1][precision]") {
    if (!assembled()) SKIP("R1 is not assembled; fetch the samples (tools/fetch_render_samples.sh) and run the r1_project fixture");
    // #1065: R1 and its cameras moved kilometres out render as at the origin, within the references' tolerance.
    Gpu gpu;
    const auto render = [&](double distance) {
        auto r1 = R1(gpu.device, offset_toward(distance));
        r1.run_to(150);
        auto images = std::vector<RgbImage>{r1.render(gpu, r1.path_camera())};
        for (const auto& view : r1::views()) images.push_back(r1.render(gpu, view.camera));
        return images;
    };
    const auto origin = render(0.0);
    for (const auto distance : offset_distances) {
        if (distance == 0.0) continue;
        const auto far = render(distance);
        for (size_t i = 0; i < far.size(); ++i)
            compare_images((i == 0 ? std::string("path-150") : "view " + std::string(r1::views()[i - 1].name)) + " at " +
                               std::to_string(int(distance)) + " m",
                           origin[i], far[i]);
    }
}

TEST_CASE("R1 shows the same image in the editor's Play as in the player, along its camera path", "[visual][gpu][r1]") {
    if (!assembled()) SKIP("R1 is not assembled; fetch the samples (tools/fetch_render_samples.sh) and run the r1_project fixture");
    constexpr uint64_t tick = 300;
    // The editor on Metal, playing R1 in its Game view, held at the tick.
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
    REQUIRE(shell.open_project(r1_folder()));
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
    for (int i = 0; i < 3; ++i) frame();
    REQUIRE(shell.start_play());
    shell.set_game_view(true);
    auto* play = shell.play_session();
    REQUIRE(play);
    play->clock().pause();
    while (play->clock().tick() < tick) {
        play->clock().step();
        frame();
    }
    // Everything the Game view draws is loaded, as behind a loading screen, so both images are complete.
    preload_render_assets(play->world(), *shell.assets());
    shell.assets()->wait_idle();
    frame(); // shown paused, at the tick
    REQUIRE(shell.showing_game());
    auto pixels = std::vector<std::byte>{};
    device.wait_idle();
    REQUIRE_FALSE(device.read_texture(window.handle, pixels));
    const auto& layout = shell.layout();
    const auto view_width = shell.viewport().width(), view_height = shell.viewport().height();
    const auto left = uint32_t(layout.viewport_min.x), top = uint32_t(layout.viewport_min.y); // points are pixels at scale 1
    REQUIRE(uint32_t(layout.viewport_max.x) - left == view_width);
    // The player's path at the same size and tick.
    auto r1 = R1(device);
    r1.run_to(tick);
    const auto& world = r1.session->world();
    auto view = extract_render_view(world, *world.find(r1.path_camera()), view_width, view_height);
    REQUIRE(view);
    auto options = RenderExtractOptions{};
    options.skins = &r1.skins;
    const auto player = gpu.render(world, *r1.registry, *view, options);
    // Between the Game view's tool bar and its "Click to play" hint, every pixel the same.
    // MAYA_R1_DIFF=<folder> writes both images and their difference when they are not.
    auto differing = size_t{0}, compared = size_t{0};
    for (uint32_t y = view_height / 8; y < view_height * 7 / 8; ++y)
        for (uint32_t x = 0; x < view_width; ++x) {
            const auto* p = pixels.data() + (size_t(top + y) * metrics.framebuffer_width + left + x) * 4; // BGRA
            const auto* q = player.rgb.data() + (size_t(y) * view_width + x) * 3;
            ++compared;
            if (std::abs(int(p[2]) - int(q[0])) > 1 || std::abs(int(p[1]) - int(q[1])) > 1 || std::abs(int(p[0]) - int(q[2])) > 1) ++differing;
        }
    INFO(differing << " of " << compared << " pixels differ");
    if (differing > 0 && std::getenv("MAYA_R1_DIFF")) {
        auto editor = RgbImage{view_width, view_height, {}}, mask = RgbImage{view_width, view_height, {}};
        for (uint32_t y = 0; y < view_height; ++y)
            for (uint32_t x = 0; x < view_width; ++x) {
                const auto* p = pixels.data() + (size_t(top + y) * metrics.framebuffer_width + left + x) * 4;
                const auto* q = player.rgb.data() + (size_t(y) * view_width + x) * 3;
                editor.rgb.insert(editor.rgb.end(), {uint8_t(p[2]), uint8_t(p[1]), uint8_t(p[0])});
                const auto d = std::max({std::abs(int(p[2]) - int(q[0])), std::abs(int(p[1]) - int(q[1])), std::abs(int(p[0]) - int(q[2]))});
                mask.rgb.insert(mask.rgb.end(), {uint8_t(std::min(255, d * 8)), uint8_t(std::min(255, d * 8)), uint8_t(std::min(255, d * 8))});
            }
        write_png(fs::path(std::getenv("MAYA_R1_DIFF")) / "editor.png", editor);
        write_png(fs::path(std::getenv("MAYA_R1_DIFF")) / "player.png", player);
        write_png(fs::path(std::getenv("MAYA_R1_DIFF")) / "difference.png", mask);
    }
    CHECK(differing == 0);
    shell.stop_play();
    device.destroy(window.handle);
}

namespace {
struct Counts {
    size_t buffers, textures, samplers, pending;
    size_t meshes, textures_resident, environments, skins, clips;
    size_t leased; // resident versions also held outside the registry: the editor's frames and thumbnails
    bool operator==(const Counts&) const = default;
};
std::ostream& operator<<(std::ostream& out, const Counts& c) {
    return out << "buffers " << c.buffers << ", textures " << c.textures << ", samplers " << c.samplers << ", pending " << c.pending
               << "; resident meshes " << c.meshes << ", textures " << c.textures_resident << ", environments " << c.environments
               << ", skins " << c.skins << ", clips " << c.clips << ", leased " << c.leased;
}
Counts counts(editor::testing::Harness& harness) {
    const auto stats = harness.device.stats();
    const auto residency = harness.shell.assets() ? harness.shell.assets()->residency() : AssetResidency{};
    return {stats.buffers, stats.textures, stats.samplers, stats.pending_retirements, residency.meshes, residency.textures,
            residency.environments, residency.skins, residency.animations, residency.leased};
}
} // namespace

TEST_CASE("Reimporting, reloading, and playing R1 again and again return to the same resources", "[r1][steady]") {
    if (!assembled()) SKIP("R1 is not assembled; fetch the samples (tools/fetch_render_samples.sh) and run the r1_project fixture");
    using namespace editor::testing;
    Harness harness(false);
    const auto settle = [&] { harness.frames(4); }; // retired resources pass through the frames in flight
    REQUIRE(harness.shell.open_project(r1_folder()));
    settle();
    REQUIRE(harness.shell.scene());
    CHECK(harness.shell.extraction().skipped == 0);
    const auto shown = counts(harness);
    REQUIRE(shown.meshes > 0);
    REQUIRE(shown.skins == 1);
    // Reimporting a model, unchanged: the same IDs, and the same resources once the old versions retire.
    for (int round = 0; round < 3; ++round) {
        const auto imported = harness.shell.import_model("models/CesiumMan.glb", false);
        REQUIRE(imported);
        settle();
        INFO("after a reimport: " << counts(harness) << "; shown: " << shown);
        CHECK(counts(harness) == shown);
    }
    // Reloading every texture the scene shows, as a changed file would.
    for (const auto& record : harness.shell.assets()->records())
        if (record.kind == AssetKind::texture && harness.shell.assets()->info(record.id)->state == AssetState::ready)
            REQUIRE(harness.shell.assets()->reload(AssetRef<TextureAsset>{record.id}));
    settle();
    INFO("after reloads: " << counts(harness) << "; shown: " << shown);
    CHECK(counts(harness) == shown);
    // Play and Stop, 10 times, playing 60 ticks each: CesiumMan walks and the camera turns. Each Play loads
    // the two clips; stopping releases them, and whatever else no one uses since (#1063), so the editor
    // returns to what it showed.
    const auto play = [&] {
        REQUIRE(harness.shell.start_play());
        harness.frames(60);
        harness.shell.stop_play();
        settle();
    };
    play();
    INFO("after the first Play: " << counts(harness) << "; shown: " << shown);
    CHECK(counts(harness) == shown);
    for (int round = 1; round < 10; ++round) play();
    INFO("after ten: " << counts(harness));
    CHECK(counts(harness) == shown);
    CHECK(harness.shell.extraction().skipped == 0);
}

TEST_CASE("Opening and closing R1's scene again and again returns to the same resident bytes", "[r1][residency]") {
    if (!assembled()) SKIP("R1 is not assembled; fetch the samples (tools/fetch_render_samples.sh) and run the r1_project fixture");
    using namespace editor::testing;
    Harness harness(false);
    // Frames until nothing is loading or waiting to retire: the scene shown whole, its old versions gone.
    const auto settle = [&] {
        for (int frame = 0; frame < 2000; ++frame) {
            harness.frames(1);
            if (harness.shell.load_stats().in_flight == 0 && harness.shell.extraction().pending == 0 &&
                harness.device.stats().pending_retirements == 0 && frame >= 4)
                return;
        }
        FAIL("R1 never settled");
    };
    struct Resident {
        std::array<ResidentBytes, residency_category_count> bytes{};
        size_t buffer_bytes = 0, texture_bytes = 0;
        bool operator==(const Resident& other) const {
            return buffer_bytes == other.buffer_bytes && texture_bytes == other.texture_bytes &&
                   std::ranges::equal(bytes, other.bytes, [](const ResidentBytes& a, const ResidentBytes& b) { return a.cpu == b.cpu && a.gpu == b.gpu; });
        }
    };
    const auto resident = [&] {
        auto result = Resident{harness.shell.assets()->residency().bytes, harness.device.stats().buffer_bytes, harness.device.stats().texture_bytes};
        return result;
    };
    const auto text = [](const Resident& r) {
        auto out = std::ostringstream{};
        for (size_t c = 0; c < residency_category_count; ++c) out << residency_category_name(ResidencyCategory(c)) << ' ' << r.bytes[c].total() << ", ";
        out << "buffers " << r.buffer_bytes << ", textures " << r.texture_bytes;
        return out.str();
    };
    REQUIRE(harness.shell.open_project(r1_folder()));
    // The Residency tab in front of the Assets panel: its thumbnails load textures of their own, whenever
    // their rows show, which is not what closing a scene is about.
    harness.shell.show_residency();
    settle();
    const auto scene = harness.shell.scene_path();
    REQUIRE(!scene.empty());
    const auto shown = resident();
    REQUIRE(shown.bytes[size_t(ResidencyCategory::textures)].gpu > 0);
    // MAYA_R1_CYCLES sets how many (the soak, #1063: 50); a few by default. MAYA_R1_SOAK_OUT=<file> writes
    // each cycle's resident bytes, closed and open, and the process's footprint, as JSON lines.
    const auto cycles = std::getenv("MAYA_R1_CYCLES") ? std::max(1, std::atoi(std::getenv("MAYA_R1_CYCLES"))) : 3;
    auto soak = std::getenv("MAYA_R1_SOAK_OUT") ? std::ofstream(std::getenv("MAYA_R1_SOAK_OUT")) : std::ofstream{};
    const auto record = [&](int cycle, const char* moment, const Resident& r) {
        if (!soak) return;
        auto total = size_t{0};
        for (const auto& bytes : r.bytes) total += bytes.total();
        const auto memory = process_memory();
        soak << "{\"cycle\": " << cycle << ", \"moment\": \"" << moment << "\", \"resident_bytes\": " << total
             << ", \"textures_bytes\": " << r.bytes[size_t(ResidencyCategory::textures)].total()
             << ", \"meshes_bytes\": " << r.bytes[size_t(ResidencyCategory::meshes)].total() << ", \"buffer_bytes\": " << r.buffer_bytes
             << ", \"texture_bytes\": " << r.texture_bytes << ", \"footprint_bytes\": " << (memory ? memory->footprint : 0) << "}\n";
    };
    record(-1, "first open", shown);
    auto closed = std::optional<Resident>{};
    for (int cycle = 0; cycle < cycles; ++cycle) {
        REQUIRE(harness.shell.new_scene()); // R1's scene closes: its content is released once a frame is drawn
        settle();
        const auto after_close = resident();
        record(cycle, "closed", after_close);
        INFO("cycle " << cycle << ", closed: " << text(after_close));
        auto left = std::ostringstream{};
        for (const auto& asset : harness.shell.assets()->largest(4))
            left << asset.path << " (" << asset.bytes.total() << (asset.leased ? ", leased" : "") << "); ";
        const auto loads = harness.shell.assets()->load_stats();
        INFO("left: " << left.str() << "loads started " << loads.started << ", cancelled " << loads.cancelled << ", finalized " << loads.finalized
                      << "; released " << harness.shell.assets()->release_stats().released);
        CHECK(after_close.bytes[size_t(ResidencyCategory::meshes)].total() == 0);
        CHECK(after_close.bytes[size_t(ResidencyCategory::textures)].total() == 0);
        if (!closed) closed = after_close;
        CHECK(after_close == *closed);
        REQUIRE(harness.shell.open_scene(scene));
        settle();
        record(cycle, "open", resident());
        INFO("cycle " << cycle << ", open again: " << text(resident()) << "; first: " << text(shown));
        CHECK(resident() == shown);
    }
}
