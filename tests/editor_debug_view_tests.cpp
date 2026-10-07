// Debug views in the editor (#1037, docs/editor.md#physics-debug-views): every view chosen from the eye
// menu, one at a time, saved as a preference and restored, and the debug pipelines built only for a view
// that needs them.

#include "editor_harness.hpp"
#include <filesystem>
#include <sstream>
#include <unistd.h>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
namespace fs = std::filesystem;

namespace {
ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
size_t debug_pipelines(const Harness& harness) {
    return size_t(std::ranges::count_if(harness.device.pipelines, [](const PipelineDesc& desc) {
        return desc.label.ends_with("(debug view)");
    }));
}
constexpr auto every_view = std::array{DebugView::none, DebugView::luminance, DebugView::false_color, DebugView::base_color,
                                       DebugView::normals, DebugView::shading_normals, DebugView::metallic, DebugView::roughness,
                                       DebugView::occlusion, DebugView::emissive, DebugView::direct_light, DebugView::environment_light,
                                       DebugView::lighting, DebugView::cascades, DebugView::texels};
} // namespace

TEST_CASE("Debug views have stable names, and preferences keep one of them", "[editor][debug-views]") {
    for (const auto view : every_view) {
        INFO(debug_view_name(view));
        CHECK(debug_view_named(debug_view_name(view)) == view);
        auto preferences = EditorPreferences{};
        preferences.debug_view = view;
        auto text = std::stringstream{};
        write_preferences(text, preferences);
        CHECK(text.str().find("\ndebug-view " + std::string(debug_view_name(view)) + "\n") != std::string::npos);
        CHECK(read_preferences(text).preferences.debug_view == view);
    }
    CHECK_FALSE(debug_view_named("sepia"));
    CHECK_FALSE(debug_view_named(""));
}

TEST_CASE("Debug views come from the eye menu one at a time, are saved, and build their pipelines only when needed", "[editor][debug-views]") {
    const auto folder = fs::temp_directory_path() / ("maya-debug-view-" + std::to_string(::getpid()));
    fs::remove_all(folder);
    const auto file = folder / "editor.preferences";
    {
        Harness harness;
        harness.shell.use_preferences_file(file);
        harness.frames(3);
        CHECK(harness.shell.debug_view() == DebugView::none);
        CHECK(debug_pipelines(harness) == 0); // nothing built for views not shown
        press(harness, control(harness, "tool.physics-debug"));
        harness.frames(2); // the menu sizes itself on its first frame
        // The exposure views need only the tone-mapping pass.
        press(harness, control(harness, "debug.view.false-color"));
        harness.frames(2);
        CHECK(harness.shell.debug_view() == DebugView::false_color);
        CHECK(debug_pipelines(harness) == 0);
        // Every other view is drawn by the lit pass's debug variants; choosing one replaces the last.
        for (const auto view : every_view) {
            if (!lit_debug_view(view)) continue;
            INFO(debug_view_name(view));
            press(harness, control(harness, "debug.view." + std::string(debug_view_name(view))));
            harness.frames(2);
            CHECK(harness.shell.debug_view() == view);
        }
        CHECK(debug_pipelines(harness) > 0);
        CHECK(fs::exists(file));
    }
    // A new editor starts with the last one chosen.
    Harness again;
    again.shell.use_preferences_file(file);
    CHECK(again.shell.debug_view() == DebugView::texels);
    again.frames(2);
    press(again, control(again, "tool.physics-debug"));
    again.frames(2);
    press(again, control(again, "debug.view.none"));
    again.frames(2);
    CHECK(again.shell.debug_view() == DebugView::none);
    fs::remove_all(folder);
}
