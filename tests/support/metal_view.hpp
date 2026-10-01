#pragma once

// A headless Metal view for tests that compare rendered images: the renderer, an offscreen readable
// target, and the #1005 reference comparison.

#include "maya/core/file_system.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "support/png.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace maya::test {

/// A headless Metal device with the renderer and an offscreen, readable view target.
struct Gpu {
    MetalDevice device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<RenderTarget> target;
    Gpu() {
        REQUIRE(device.initialize(nullptr, {3, size_t{16} << 20}));
        auto shader = FileSystem::read_text("resources/shaders/metal/renderer.metal");
        REQUIRE_FALSE(shader.empty());
        renderer = std::make_unique<Renderer>(device, std::move(shader));
        target = std::make_unique<RenderTarget>(device, RenderTargetDesc{Format::rgba8_unorm, true, "test view"});
    }
    ~Gpu() {
        target.reset();
        renderer.reset();
        device.wait_idle();
        CHECK(device.take_gpu_errors().empty());
        device.shutdown();
    }
    /// Renders a World through a view and reads the image back as RGB.
    RgbImage render(const World& world, AssetRegistry& assets, const RenderView& view, const RenderExtractOptions& options = {}) {
        REQUIRE_FALSE(target->resize(view.width, view.height));
        const auto snapshot = extract_render_snapshot(world, assets, options);
        REQUIRE(snapshot.diagnostics.empty());
        REQUIRE_FALSE(device.begin_frame());
        const auto error = renderer->render(snapshot, view, *target);
        INFO(error.message);
        REQUIRE_FALSE(error);
        REQUIRE_FALSE(device.end_frame());
        auto pixels = std::vector<std::byte>{};
        REQUIRE_FALSE(device.read_texture(target->color(), pixels));
        auto image = RgbImage{view.width, view.height, {}};
        image.rgb.reserve(size_t(view.width) * view.height * 3);
        for (size_t i = 0; i < pixels.size(); i += 4)
            for (size_t c = 0; c < 3; ++c) image.rgb.push_back(uint8_t(pixels[i + c]));
        return image;
    }
};

/// Compares images with the blessed references in `references` (name.png): each channel within 6 on
/// at least 99.5% of pixels. Differing images and their differences are written to `diffs`. With
/// MAYA_BLESS_REFERENCES set, the images become the references instead; inspect them before committing.
inline void compare_with_references(const std::filesystem::path& references, const std::filesystem::path& diffs,
                                    const std::vector<std::pair<std::string, RgbImage>>& images) {
    const auto bless = std::getenv("MAYA_BLESS_REFERENCES") != nullptr;
    for (const auto& [name, image] : images) {
        INFO(name);
        const auto path = references / (name + ".png");
        if (bless) {
            std::filesystem::create_directories(references);
            REQUIRE(write_png(path, image));
            WARN("blessed " << path.string() << "; inspect it before committing");
            continue;
        }
        const auto reference = read_png(path);
        INFO("missing reference; render them with MAYA_BLESS_REFERENCES=1 and inspect them");
        REQUIRE(reference);
        REQUIRE(reference->width == image.width);
        REQUIRE(reference->height == image.height);
        auto outside = size_t{0};
        auto worst = 0;
        auto difference = image;
        for (size_t i = 0; i < image.rgb.size(); i += 3) {
            auto largest = 0;
            for (size_t c = 0; c < 3; ++c) largest = std::max(largest, std::abs(int(image.rgb[i + c]) - int(reference->rgb[i + c])));
            worst = std::max(worst, largest);
            if (largest > 6) ++outside;
            for (size_t c = 0; c < 3; ++c) difference.rgb[i + c] = uint8_t(std::min(255, largest * 8));
        }
        const auto fraction = double(outside) / double(image.width * image.height);
        INFO("pixels outside tolerance " << fraction * 100.0 << "%, largest channel difference " << worst);
        if (fraction > 0.005) {
            std::filesystem::create_directories(diffs);
            write_png(diffs / (name + ".actual.png"), image);
            write_png(diffs / (name + ".difference.png"), difference);
        }
        CHECK(fraction <= 0.005);
    }
}

} // namespace maya::test
