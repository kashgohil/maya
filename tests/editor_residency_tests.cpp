// Residency in the editor (#1063, docs/editor.md#residency): content leaves when a scene closes or Play
// stops, the Residency panel shows it against the budgets, budgets that cannot be met are reported, and
// the cook cache can be pruned from the project menu.

#include "editor_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;

namespace {
const auto cube = AssetRef<MeshAsset>{{0x6d617961, 2}};
const auto cutout = AssetRef<TextureAsset>{{0x6d617961, 0x53}}; // in materials.scene, not basic.scene
ImVec2 center(const EditorLayout::Field* control) {
    REQUIRE(control);
    return {(control->min.x + control->max.x) * 0.5f, (control->min.y + control->max.y) * 0.5f};
}
} // namespace

TEST_CASE("Closing a scene releases what nothing has used since, once a frame is drawn", "[editor][residency]") {
    Harness harness;
    harness.frames(4);
    auto& assets = *harness.shell.assets();
    REQUIRE(assets.info(cube.id)->state == AssetState::ready);
    REQUIRE(assets.resident(ResidencyCategory::meshes).total() > 0);
    REQUIRE(harness.shell.new_scene());
    // The frame the scene closed draws the new one; its content goes after that frame.
    harness.frames(3);
    CHECK(assets.info(cube.id)->state == AssetState::unloaded);
    CHECK(assets.resident(ResidencyCategory::meshes).total() == 0);
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "unused assets"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "after closing the scene"));
    // Opening it again loads it again.
    REQUIRE(harness.shell.open_scene("basic.scene"));
    harness.settle();
    harness.frames(2);
    CHECK(assets.info(cube.id)->state == AssetState::ready);
}

TEST_CASE("The Residency panel shows each category against its budget and releases unused content when asked", "[editor][residency]") {
    Harness harness;
    harness.frames(2);
    harness.shell.show_residency();
    harness.frames(3);
    const auto& layout = harness.shell.layout();
    for (const auto* category : {"meshes", "textures", "environments", "animation", "cells", "other", "renderer"}) {
        INFO(category);
        CHECK(layout.control(std::string("residency.") + category));
    }
    REQUIRE(layout.control("residency.release"));
    // A texture loaded and then dropped: resident, unused, and released when asked.
    auto& assets = *harness.shell.assets();
    {
        const auto wait = AssetRegistry::ExplicitWait(assets);
        REQUIRE(assets.acquire(cutout));
    }
    REQUIRE(assets.info(cutout.id)->state == AssetState::ready);
    press(harness, center(layout.control("residency.release")));
    harness.frames(3);
    CHECK(assets.info(cutout.id)->state == AssetState::unloaded);
    CHECK(assets.info(cube.id)->state == AssetState::ready); // drawn every frame: kept
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "after asking to release unused content"));
}

TEST_CASE("A budget that content in use exceeds is reported once, naming the largest", "[editor][residency]") {
    Harness harness;
    harness.frames(2);
    auto budgets = default_residency_budgets();
    budgets.bytes[size_t(ResidencyCategory::meshes)] = 1; // smaller than any mesh the scene draws
    harness.shell.assets()->set_budgets(budgets);
    harness.frames(8);
    CHECK(harness.shell.assets()->info(cube.id)->state == AssetState::ready); // drawn: never released
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Resident meshes are"));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "all of it is in use"));
    const auto entry = std::ranges::find_if(harness.shell.diagnostics().entries(), [](const DiagnosticEntry& entry) {
        return entry.message.find("Resident meshes are") != std::string::npos;
    });
    REQUIRE(entry != harness.shell.diagnostics().entries().end());
    CHECK(entry->count == 1); // once, while it stays over
}

TEST_CASE("The project menu prunes the cook cache in the background", "[editor][residency][cache]") {
    Harness harness;
    harness.frames(2);
    press(harness, center(harness.shell.layout().control("project_menu")));
    harness.frames(3); // a popup sizes itself on its first frames
    press(harness, center(harness.shell.layout().control("project.prune_cook_cache")));
    CHECK(logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Pruning the cook cache"));
    // The job reports when it ends: what it removed, or that the cache is empty. It waits its turn on the
    // background tier, behind the textures the Assets panel cooks, so this waits by time, not frames.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (std::chrono::steady_clock::now() < deadline) {
        harness.frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Pruned the cook cache") ||
            logged(harness.shell.diagnostics(), DiagnosticSource::asset, "The cook cache is empty"))
            break;
    }
    CHECK((logged(harness.shell.diagnostics(), DiagnosticSource::asset, "Pruned the cook cache") ||
           logged(harness.shell.diagnostics(), DiagnosticSource::asset, "The cook cache is empty")));
}
