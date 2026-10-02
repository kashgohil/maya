#include "editor_shell.hpp"
#include "maya/core/file_system.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>

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
