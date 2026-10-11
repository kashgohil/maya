#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "support/metal_view.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unistd.h>

using namespace maya;
using namespace maya::editor;

namespace {
using Pixel = std::array<int, 4>; // as stored: BGRA
Pixel pixel(const std::vector<std::byte>& pixels, uint32_t width, uint32_t x, uint32_t y) {
    const auto* p = pixels.data() + (size_t{y} * width + x) * 4;
    return {int(p[0]), int(p[1]), int(p[2]), int(p[3])};
}
int brightness(Pixel p) { return p[0] + p[1] + p[2]; }

/// Writes the window image when MAYA_EDITOR_CAPTURE names a .ppm path, for manual review.
void capture(const std::vector<std::byte>& pixels, uint32_t width, uint32_t height) {
    const char* path = std::getenv("MAYA_EDITOR_CAPTURE");
    if (!path) return;
    auto out = std::ofstream(path, std::ios::binary);
    out << "P6 " << width << ' ' << height << " 255\n";
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        const auto* p = pixels.data() + i * 4;
        const char rgb[] = {char(p[2]), char(p[1]), char(p[0])};
        out.write(rgb, 3);
    }
}
} // namespace

TEST_CASE("Metal editor draws docked panels and the scene viewport at Retina scale", "[gpu][editor]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr, {3, size_t{16} << 20}));
    {
        const auto read = [](const char* relative) {
            auto file = std::ifstream(*FileSystem::resolve(relative), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), {});
        };
        auto shell = EditorShell(device, FileSystem::read_text("resources/shaders/metal/renderer.metal"),
                                 FileSystem::read_text("resources/shaders/metal/editor_ui.metal"), {},
                                 {read("resources/fonts/Inter-Regular.ttf"), read("resources/fonts/Inter-SemiBold.ttf"),
                                  read("resources/fonts/GeistMono-Regular.ttf"), read("resources/fonts/Phosphor-Light.ttf")});
        const auto project = FileSystem::resolve("samples/basic_scene/project.maya");
        REQUIRE(project);
        REQUIRE(shell.open_project(*project));

        auto metrics = WindowMetrics{800, 500, 1600, 1000};
        auto window = TextureHandle{};
        const auto render = [&](int frames, std::vector<InputEvent> events = {}) {
            if (!device.describe(window) || device.describe(window)->width != metrics.framebuffer_width) {
                device.destroy(window);
                auto created = device.create_texture({metrics.framebuffer_width, metrics.framebuffer_height,
                    Format::bgra8_unorm, TextureUsage::render_target | TextureUsage::readback, "window"});
                REQUIRE(created);
                window = created.handle;
            }
            for (int frame = 0; frame < frames; ++frame) {
                shell.update(1.0f / 60.0f, frame == 0 ? events : std::vector<InputEvent>{}, metrics);
                REQUIRE_FALSE(device.begin_frame());
                const auto error = shell.render(window);
                INFO(error.message);
                REQUIRE_FALSE(error);
                REQUIRE_FALSE(device.end_frame());
            }
            auto pixels = std::vector<std::byte>{};
            REQUIRE_FALSE(device.read_texture(window, pixels));
            return pixels;
        };

        auto pixels = render(4);
        capture(pixels, metrics.framebuffer_width, metrics.framebuffer_height);
        const auto& layout = shell.layout();
        const auto center_x = uint32_t((layout.viewport_min.x + layout.viewport_max.x) * 0.5f * 2.0f);
        const auto center_y = uint32_t((layout.viewport_min.y + layout.viewport_max.y) * 0.5f * 2.0f);
        // The viewport shows the lit scene; panels show UI; nothing is stretched.
        CHECK(brightness(pixel(pixels, 1600, center_x, center_y)) > 150);
        CHECK(shell.viewport().width() == shell.viewport_request().width);
        CHECK(shell.viewport_request().width < 1600);
        const auto panel = pixel(pixels, 1600, 20, 900); // the hierarchy panel, below its entries
        CHECK(brightness(panel) < 150);
        CHECK(shell.ui_renderer().stats().missing_textures == 0);
        CHECK(shell.extraction().skipped == 0);
        // Performance keeps each pass's GPU time by name (#1026): the viewport, the UI, and thumbnails.
        render(2);
        device.wait_idle();
        render(1); // takes the timings of the frames that completed
        const auto& passes = shell.performance().gpu_passes;
        for (const auto* label : {"view", "editor ui", "texture thumbnail"}) {
            INFO(label);
            REQUIRE(passes.contains(label));
            CHECK(passes.at(label).samples.summary().mean > 0.0);
        }

        // The Assets panel's texture thumbnails, from each texture's mips in the display's encoding.
        const auto thumbnail = [&](uint64_t low) {
            const auto target = shell.thumbnails().row_target({0x6d617961, low});
            REQUIRE(target.valid());
            device.wait_idle();
            auto texels = std::vector<std::byte>{};
            REQUIRE_FALSE(device.read_texture(target, texels));
            return texels;
        };
        const auto rgba = [](const std::vector<std::byte>& texels, uint32_t x, uint32_t y) { // stored as RGBA
            const auto* p = texels.data() + (size_t{y} * TextureThumbnails::row_size + x) * 4;
            return Pixel{int(p[0]), int(p[1]), int(p[2]), int(p[3])};
        };
        const auto near = [](Pixel value, Pixel expected, int tolerance) {
            for (size_t i = 0; i < 4; ++i)
                if (std::abs(value[i] - expected[i]) > tolerance) return false;
            return true;
        };
        const auto grid = thumbnail(0x50); // the sample's 256-texel tile grid, ASTC 6x6 sRGB
        CHECK(near(rgba(grid, 5, 5), {200, 192, 180, 255}, 8)); // tile (0, 0) as authored, not darkened by linear values
        CHECK(near(rgba(grid, 15, 5), {194, 186, 174, 255}, 8)); // tile (1, 0)
        const auto normal = thumbnail(0x51); // its normal map: flat tiles point straight up
        CHECK(near(rgba(normal, 5, 5), {128, 128, 255, 255}, 6));

        // Resize and scale change: the viewport follows; the rendering stays valid.
        metrics = {1000, 600, 1000, 600};
        pixels = render(3);
        CHECK(shell.viewport().allocations() == 2);
        const auto& resized = shell.layout();
        CHECK(brightness(pixel(pixels, 1000, uint32_t((resized.viewport_min.x + resized.viewport_max.x) / 2),
                               uint32_t((resized.viewport_min.y + resized.viewport_max.y) / 2))) > 150);

        // Minimize and recover.
        metrics = {0, 0, 0, 0};
        shell.update(1.0f / 60.0f, {}, metrics);
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(shell.render(window)); // nothing to draw while minimized
        REQUIRE_FALSE(device.end_frame());
        metrics = {1000, 600, 1000, 600};
        render(2);
        CHECK(shell.viewport().allocations() == 2);
        CHECK(shell.diagnostics().count(DiagnosticSource::gpu) == 0);
        CHECK(shell.diagnostics().count(DiagnosticSource::renderer) == 0);
    }
    device.wait_idle();
    CHECK(device.take_gpu_errors().empty());
    device.shutdown();
}

TEST_CASE("The Residency panel matches its reference", "[gpu][editor][visual][residency]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr, {3, size_t{16} << 20}));
    {
        const auto read = [](const char* relative) {
            auto file = std::ifstream(*FileSystem::resolve(relative), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), {});
        };
        auto shell = EditorShell(device, FileSystem::read_text("resources/shaders/metal/renderer.metal"),
                                 FileSystem::read_text("resources/shaders/metal/editor_ui.metal"), {},
                                 {read("resources/fonts/Inter-Regular.ttf"), read("resources/fonts/Inter-SemiBold.ttf"),
                                  read("resources/fonts/GeistMono-Regular.ttf"), read("resources/fonts/Phosphor-Light.ttf")});
        const auto project = FileSystem::resolve("samples/basic_scene/project.maya");
        REQUIRE(project);
        REQUIRE(shell.open_project(*project));
        // Fixed figures: the platform's (Metal's allocation, the process's footprint) change from run to run.
        constexpr auto mib = size_t{1} << 20;
        auto figures = EditorShell::ResidencyFigures{};
        figures.report.budgets = default_residency_budgets();
        figures.report.bytes[size_t(ResidencyCategory::meshes)] = {12 * mib, 96 * mib};
        figures.report.bytes[size_t(ResidencyCategory::textures)] = {0, 812 * mib}; // over its 768 MiB
        figures.report.bytes[size_t(ResidencyCategory::environments)] = {0, 48 * mib};
        figures.report.bytes[size_t(ResidencyCategory::animation)] = {3 * mib, 0};
        figures.report.bytes[size_t(ResidencyCategory::cells)] = {14 * mib, 0};
        figures.report.bytes[size_t(ResidencyCategory::other)] = {mib / 4, 0};
        figures.report.bytes[size_t(ResidencyCategory::renderer)] = {0, 150 * mib};
        figures.report.tracked_gpu = 1106 * mib;
        figures.report.unattributed_gpu = 0;
        figures.report.upload_gpu = 48 * mib;
        figures.report.reported_gpu = 1190 * mib;
        figures.report.footprint = 1420 * mib;
        figures.largest = {{"models/FlightHelmet/textures/lenses_basecolor.png#color", ResidencyCategory::textures, {0, 85 * mib}, true},
                           {"textures/terrain_albedo.texture", ResidencyCategory::textures, {0, 64 * mib}, true},
                           {"environments/workshop.environment", ResidencyCategory::environments, {0, 48 * mib}, true},
                           {"models/Sponza.gltf#mesh/0/12", ResidencyCategory::meshes, {2 * mib, 18 * mib}, false}};
        figures.release.released = 37;
        figures.release.released_for_budget = 12;
        figures.release.over_budget[size_t(ResidencyCategory::textures)] = true;
        figures.viewport_versions = 41;
        figures.viewport_bytes = 960 * mib;
        figures.loading = 2;
        figures.ready = 1;
        // Tall, so the bottom panels' share (28%) shows the whole panel.
        auto metrics = WindowMetrics{1200, 3400, 2400, 6800};
        auto window = device.create_texture({metrics.framebuffer_width, metrics.framebuffer_height, Format::bgra8_unorm,
                                             TextureUsage::render_target | TextureUsage::readback, "window"});
        REQUIRE(window);
        const auto frames = [&](int count) {
            for (int frame = 0; frame < count; ++frame) {
                shell.update(1.0f / 60.0f, {}, metrics);
                REQUIRE_FALSE(device.begin_frame());
                REQUIRE_FALSE(shell.render(window.handle));
                REQUIRE_FALSE(device.end_frame());
            }
        };
        frames(3); // the dock layout is built first
        shell.pin_residency(figures);
        shell.show_residency();
        frames(4);
        auto pixels = std::vector<std::byte>{};
        REQUIRE_FALSE(device.read_texture(window.handle, pixels));
        const auto panel = shell.layout().control("residency.panel");
        REQUIRE(panel);
        // The panel, in framebuffer pixels (2x).
        const auto left = uint32_t(panel->min.x * 2.0f), top = uint32_t(panel->min.y * 2.0f);
        const auto width = uint32_t((panel->max.x - panel->min.x) * 2.0f), height = uint32_t((panel->max.y - panel->min.y) * 2.0f);
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        auto image = test::RgbImage{width, height, {}};
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto* p = pixels.data() + (size_t(top + y) * metrics.framebuffer_width + left + x) * 4; // BGRA
                image.rgb.insert(image.rgb.end(), {uint8_t(p[2]), uint8_t(p[1]), uint8_t(p[0])});
            }
        test::compare_with_references(std::filesystem::path(MAYA_SOURCE_DIR) / "tests/references/editor",
                                      std::filesystem::path(MAYA_SOURCE_DIR) / "build/visual-diffs", {{"residency", image}});
        device.destroy(window.handle);
    }
    device.shutdown();
}

TEST_CASE("The cells overlay and the Streaming panel match their references", "[gpu][editor][visual][worlds]") {
    // A copy of the sample project holding a 5 × 5-cell world: a crate in each cell, and one cell whose file
    // is damaged, so it fails to load.
    const auto folder = std::filesystem::temp_directory_path() / ("maya-world-visual-" + std::to_string(::getpid()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto sample = FileSystem::resolve("samples/basic_scene/project.maya");
    REQUIRE(sample);
    std::filesystem::copy(sample->parent_path() / "assets", folder / "assets", std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(*sample, folder / "project.maya");
    {
        const auto any = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
        auto scene = SceneDocument{};
        const auto id = [](uint64_t low) { return EntityId{0x0e1d, low}; };
        auto camera = TransformComponent{{0, 2, 0}, {}, math::Vec3(1.0f)};
        scene.entities.push_back({id(1), std::nullopt, {NameComponent{"Camera"}, camera, CameraComponent{}}});
        auto sun = TransformComponent{{0, 10, 0}, math::Quat::from_axis_angle(math::Vec3{1, 0.3f, 0.2f}.normalized(), -0.9f), math::Vec3(1.0f)};
        scene.entities.push_back({id(2), std::nullopt, {NameComponent{"Sun"}, sun, LightComponent{}}});
        for (int x = -2; x <= 2; ++x)
            for (int z = -2; z <= 2; ++z) {
                auto crate = TransformComponent{{x * 128.0 + 64.0, 4.0, z * 128.0 + 64.0}, {}, math::Vec3(8.0f)};
                scene.entities.push_back({id(100 + uint64_t((x + 2) * 5 + z + 2)), std::nullopt,
                                          {NameComponent{"Crate " + cell_name({x, z})}, crate,
                                           MeshRendererComponent{AssetRef<MeshAsset>{{0x6d617961, 2}}, AssetRef<MaterialAsset>{{0x6d617961, 0x11}}, true}}});
            }
        REQUIRE(save_world(folder / "assets/levels/overlay.world", scene, any).empty());
        std::ofstream(folder / "assets/levels/overlay/cells/1_1.scene") << "not a scene\n";
    }
    MetalDevice device;
    REQUIRE(device.initialize(nullptr, {3, size_t{16} << 20}));
    {
        const auto read = [](const char* relative) {
            auto file = std::ifstream(*FileSystem::resolve(relative), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), {});
        };
        auto shell = EditorShell(device, FileSystem::read_text("resources/shaders/metal/renderer.metal"),
                                 FileSystem::read_text("resources/shaders/metal/editor_ui.metal"), {},
                                 {read("resources/fonts/Inter-Regular.ttf"), read("resources/fonts/Inter-SemiBold.ttf"),
                                  read("resources/fonts/GeistMono-Regular.ttf"), read("resources/fonts/Phosphor-Light.ttf")});
        REQUIRE(shell.open_project(folder / "project.maya"));
        shell.camera() = EditorCamera::looking_at({0.0, 700.0, 260.0}, {0.0, 0.0, -20.0}); // all within the far clip
        REQUIRE(shell.open_scene("levels/overlay.world"));
        auto& cells = *shell.cells();
        cells.settings().load_radius = 360.0; // from the camera, 260 m south: all but the farthest row
        shell.set_cells_tool(true);
        // The overlay in a usual window; the panel in a tall one, where the bottom panels' share shows all of it.
        auto metrics = WindowMetrics{1280, 720, 2560, 1440};
        const auto wide = device.create_texture({metrics.framebuffer_width, metrics.framebuffer_height, Format::bgra8_unorm,
                                                 TextureUsage::render_target | TextureUsage::readback, "window"});
        const auto tall = device.create_texture({2400, 6800, Format::bgra8_unorm, TextureUsage::render_target | TextureUsage::readback, "tall window"});
        REQUIRE(wide);
        REQUIRE(tall);
        auto window = wide;
        const auto frames = [&](int count) {
            for (int frame = 0; frame < count; ++frame) {
                shell.update(1.0f / 60.0f, {}, metrics);
                REQUIRE_FALSE(device.begin_frame());
                REQUIRE_FALSE(shell.render(window.handle));
                REQUIRE_FALSE(device.end_frame());
            }
        };
        frames(3);
        cells.settle();
        // A cell locked by hand far off, and one changed: each as the overlay marks it.
        REQUIRE(cells.hold({-2, -2}, CellHold::loaded).empty());
        REQUIRE(shell.scene()->rename(EntityId{0x0e1d, 100 + 2 * 5 + 1 + 2}, "Edited crate")); // cell 0_1
        cells.settle();
        CHECK(cells.state({1, 1}) == EditorCellState::failed);
        CHECK(cells.state({0, -2}) == EditorCellState::unloaded);
        CHECK(cells.state({-2, -2}) == EditorCellState::active);
        CHECK(cells.changed({0, 1}));
        frames(2);
        for (int i = 0; i < 600 && (shell.extraction().pending > 0 || shell.extraction().pending_textures > 0); ++i) { // the crates' meshes and material
            shell.assets()->wait_idle();
            frames(1);
        }
        REQUIRE(shell.extraction().pending == 0);
        frames(2);
        const auto capture = [&](ImVec2 min, ImVec2 max) {
            auto pixels = std::vector<std::byte>{};
            REQUIRE_FALSE(device.read_texture(window.handle, pixels));
            const auto left = uint32_t(min.x * 2.0f), top = uint32_t(min.y * 2.0f);
            const auto width = uint32_t((max.x - min.x) * 2.0f), height = uint32_t((max.y - min.y) * 2.0f);
            REQUIRE(width > 0);
            REQUIRE(height > 0);
            auto image = test::RgbImage{width, height, {}};
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    const auto* p = pixels.data() + (size_t(top + y) * metrics.framebuffer_width + left + x) * 4; // BGRA
                    image.rgb.insert(image.rgb.end(), {uint8_t(p[2]), uint8_t(p[1]), uint8_t(p[0])});
                }
            return image;
        };
        const auto overlay = capture(shell.layout().viewport_min, shell.layout().viewport_max);
        // The panel with fixed figures: timings change from run to run.
        constexpr auto mib = size_t{1} << 20;
        auto figures = EditorCellStats{};
        figures.cells[size_t(EditorCellState::active)] = 10;
        figures.cells[size_t(EditorCellState::loading)] = 2;
        figures.cells[size_t(EditorCellState::unloaded)] = 12;
        figures.cells[size_t(EditorCellState::failed)] = 1;
        figures.changed = 1;
        figures.held = 1;
        figures.loads_in_flight = 2;
        figures.frame_ms = 0.42;
        figures.longest_frame_ms = 1.87;
        figures.last_activation_ms = 0.35;
        figures.longest_activation_ms = 2.4;
        figures.last_error = "Cell 1_1: overlay/cells/1_1.scene: line 1: Expected maya-scene 1";
        auto residency = EditorShell::ResidencyFigures{};
        residency.report.budgets = default_residency_budgets();
        residency.report.bytes[size_t(ResidencyCategory::cells)] = {6 * mib, 0};
        residency.report.bytes[size_t(ResidencyCategory::meshes)] = {mib, 3 * mib};
        residency.report.bytes[size_t(ResidencyCategory::renderer)] = {0, 120 * mib};
        shell.pin_residency(residency);
        shell.pin_streaming(figures);
        metrics = WindowMetrics{1200, 3400, 2400, 6800};
        window = tall;
        frames(4);
        const auto* panel = shell.layout().control("streaming.panel");
        REQUIRE(panel);
        const auto streaming = capture(panel->min, panel->max);
        test::compare_with_references(std::filesystem::path(MAYA_SOURCE_DIR) / "tests/references/editor",
                                      std::filesystem::path(MAYA_SOURCE_DIR) / "build/visual-diffs",
                                      {{"cells-overlay", overlay}, {"streaming", streaming}});
        device.destroy(wide.handle);
        device.destroy(tall.handle);
    }
    device.shutdown();
    std::filesystem::remove_all(folder);
}
