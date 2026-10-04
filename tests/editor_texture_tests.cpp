#include "editor_harness.hpp"
#include <chrono>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
constexpr auto grid = AssetId{0x6d617961, 0x50}, grid_normal = AssetId{0x6d617961, 0x51};

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
AssetState state(Harness& harness, AssetId texture) { return harness.shell.assets()->info(texture)->state; }
size_t count(const std::vector<std::string>& labels, std::string_view label) { return size_t(std::ranges::count(labels, label)); }
size_t reports(Harness& harness, std::string_view text) {
    return size_t(std::ranges::count_if(harness.shell.diagnostics().entries(), [&](const DiagnosticEntry& entry) {
        return entry.source == DiagnosticSource::asset && entry.message.find(text) != std::string::npos;
    }));
}
/// Records the label of every texture the editor creates.
std::vector<std::string>& watch_textures(Harness& harness) {
    static auto labels = std::vector<std::string>{};
    labels.clear();
    harness.device.fail_texture = [](const TextureDesc& desc) {
        labels.push_back(desc.label);
        return false;
    };
    return labels;
}
} // namespace

TEST_CASE("The Assets panel lists textures, loads them a frame at a time, and draws each thumbnail once", "[editor][textures]") {
    Harness harness;
    auto& labels = watch_textures(harness);
    harness.frames(1);
    REQUIRE(harness.shell.layout().control("asset.textures/grid.texture"));
    REQUIRE(harness.shell.layout().control("asset.textures/grid_normal.texture"));
    // Cooking can take tens of milliseconds, so one texture loads a frame.
    CHECK(int(state(harness, grid) == AssetState::ready) + int(state(harness, grid_normal) == AssetState::ready) == 1);
    harness.frames(1);
    CHECK(state(harness, grid) == AssetState::ready);
    CHECK(state(harness, grid_normal) == AssetState::ready);
    CHECK(harness.shell.thumbnails().cached() == 2);
    CHECK(count(labels, "thumbnail grid") == 1);
    CHECK(count(labels, "thumbnail grid_normal") == 1);
    CHECK(std::ranges::any_of(harness.device.pipelines, [](const PipelineDesc& desc) {
        return desc.fragment_entry == "thumbnailFragment" && desc.color_formats == std::vector{Format::rgba8_unorm};
    }));
    const auto residency = harness.shell.assets()->residency();
    CHECK(residency.textures == 2);
    auto bytes = size_t{0};
    for (const auto texture : {grid, grid_normal}) bytes += harness.shell.assets()->acquire(AssetRef<TextureAsset>{texture}).lease.value().gpu_bytes();
    CHECK(residency.texture_gpu_bytes == bytes);
    CHECK(bytes > 0);

    // A thumbnail is drawn once per texture version, not every frame.
    const auto slot = harness.shell.assets()->acquire(AssetRef<TextureAsset>{grid}).lease.value().texture().handle().slot;
    const auto sampled = [&] { return std::ranges::count(harness.device.sampled, slot); };
    CHECK(sampled() == 1);
    harness.frames(5);
    CHECK(sampled() == 1);
    // Hovering shows a larger preview, drawn once.
    const auto row = control(harness, "asset.textures/grid.texture");
    harness.frame({MouseMoveEvent{row.x, row.y}});
    harness.frames(3);
    CHECK(sampled() == 2);
    CHECK(count(labels, "thumbnail grid") == 2);

    // A reload is a new version, so its thumbnail is drawn again.
    REQUIRE(harness.shell.assets()->reload(AssetRef<TextureAsset>{grid}));
    harness.frames(2);
    CHECK(std::ranges::count(harness.device.sampled, harness.shell.assets()->acquire(AssetRef<TextureAsset>{grid}).lease.value().texture().handle().slot) >= 1);
}

TEST_CASE("A texture that cannot load is reported once, shows the placeholder, and reloads from its menu", "[editor][textures]") {
    const auto copy = ProjectCopy();
    const auto valid = copy.read("textures/grid.texture");
    copy.write("textures/grid.texture", "maya-texture 1\nsource \"grid.png\"\nusage albedo\n");
    Harness harness(false);
    auto& labels = watch_textures(harness);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(4);
    CHECK(state(harness, grid) == AssetState::failed);
    CHECK(state(harness, grid_normal) == AssetState::ready);
    CHECK(reports(harness, "grid.texture: line 3: usage must be color, data, or normal") == 1);
    CHECK(count(labels, "texture placeholder") == 2); // the thumbnails' and the renderer's
    CHECK(count(labels, "thumbnail texture placeholder") == 1);

    // Fixed on disk, it reloads from the row's menu.
    copy.write("textures/grid.texture", valid);
    press(harness, control(harness, "asset.textures/grid.texture"), MouseButton::right);
    harness.frames(1);
    press(harness, control(harness, "asset.reload"));
    harness.frames(2);
    CHECK(state(harness, grid) == AssetState::ready);
    CHECK(reports(harness, "Reloaded textures/grid.texture") == 1);
    CHECK(count(labels, "thumbnail grid") == 1);
}

TEST_CASE("Textures are used through materials, never assigned to objects", "[editor][textures]") {
    Harness harness;
    harness.frames(2);
    auto& scene = *harness.shell.scene();
    const auto entity = capture_state(scene.world()).entities.begin()->first;
    const auto refused = harness.shell.assign_asset(entity, grid);
    CHECK_FALSE(refused);
    CHECK(refused.error == "Textures are used through materials, not assigned to objects");
    // Double-clicking a texture row with an object selected changes nothing.
    scene.select(entity);
    const auto row = control(harness, "asset.textures/grid.texture");
    harness.frame({MouseMoveEvent{row.x, row.y}});
    for (int i = 0; i < 2; ++i) {
        harness.frame({MouseButtonEvent{MouseButton::left, true, KeyModifiers::none}});
        harness.frame({MouseButtonEvent{MouseButton::left, false, KeyModifiers::none}});
    }
    harness.frames(1);
    CHECK_FALSE(scene.dirty()); // no edit was applied
}

TEST_CASE("A texture whose file or source image changes outside the editor reloads by itself", "[editor][textures]") {
    const auto copy = ProjectCopy();
    Harness harness(false);
    REQUIRE(harness.shell.open_project(copy.folder));
    harness.frames(3); // the thumbnails load the textures, a frame each
    REQUIRE(state(harness, grid) == AssetState::ready);
    harness.shell.check_asset_files(); // as loaded
    const auto generation = [&] { return harness.shell.assets()->info(grid)->generation; };
    const auto before = generation();
    const auto touch = [&](const char* path, int seconds) {
        fs::last_write_time(copy.content / path, fs::file_time_type::clock::now() + std::chrono::seconds(seconds));
    };
    // The source image replaced by another program: cooked again.
    fs::copy_file(copy.content / "textures/grid_normal.png", copy.content / "textures/grid.png", fs::copy_options::overwrite_existing);
    touch("textures/grid.png", 5);
    harness.shell.check_asset_files();
    CHECK(generation() == before + 1);
    CHECK(reports(harness, "Reloaded textures/grid.texture") == 1);
    harness.shell.check_asset_files(); // unchanged since
    CHECK(generation() == before + 1);
    // Its texture file changed: read again, with the new settings.
    auto text = copy.read("textures/grid.texture");
    text.replace(text.find("filter linear linear"), 20, "filter nearest nearest");
    copy.write("textures/grid.texture", text);
    touch("textures/grid.texture", 9);
    harness.shell.check_asset_files();
    CHECK(generation() == before + 2);
    CHECK(harness.shell.assets()->acquire(AssetRef<TextureAsset>{grid}).lease.value().sampler().desc().mag_filter == Filter::nearest);
    // Reload from the menu reads both now, so the watcher does not read them again.
    press(harness, control(harness, "asset.textures/grid.texture"), MouseButton::right);
    harness.frames(1);
    press(harness, control(harness, "asset.reload"));
    harness.frames(1);
    const auto reloaded = generation();
    harness.shell.check_asset_files();
    CHECK(generation() == reloaded);
}
