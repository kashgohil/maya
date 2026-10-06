#include "editor_preferences.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <sstream>

namespace maya::editor {
namespace {

constexpr std::pair<PhysicsDebugCategory, const char*> categories[] = {
    {PhysicsDebugCategory::colliders, "colliders"}, {PhysicsDebugCategory::contacts, "contacts"},
    {PhysicsDebugCategory::body_state, "body-state"}, {PhysicsDebugCategory::triggers, "triggers"},
    {PhysicsDebugCategory::queries, "queries"}};
constexpr std::pair<ExposureView, const char*> exposure_views[] = {
    {ExposureView::none, "none"}, {ExposureView::luminance, "luminance"}, {ExposureView::false_color, "false-color"}};
constexpr std::pair<ShadowView, const char*> shadow_views[] = {
    {ShadowView::none, "none"}, {ShadowView::cascades, "cascades"}, {ShadowView::texels, "texels"}};

} // namespace

void write_preferences(std::ostream& output, const EditorPreferences& preferences) {
    auto out = std::ostringstream{};
    out << "maya-editor-preferences 1\n";
    out << "physics-debug";
    for (const auto& [category, name] : categories)
        if (preferences.physics_debug.has(category)) out << ' ' << name;
    out << "\nphysics-debug-groups " << std::hex << preferences.physics_debug.groups << std::dec << '\n';
    for (const auto& [view, name] : exposure_views)
        if (view == preferences.exposure_view) out << "exposure-view " << name << '\n';
    for (const auto& [view, name] : shadow_views)
        if (view == preferences.shadow_view) out << "shadow-view " << name << '\n';
    output << out.str();
}

PreferencesReadResult read_preferences(std::istream& input) {
    auto result = PreferencesReadResult{};
    auto line = std::string{};
    if (!std::getline(input, line) || line != "maya-editor-preferences 1") {
        result.error = "Not Maya editor preferences, or another version";
        return result;
    }
    while (std::getline(input, line)) {
        auto words = std::istringstream(line);
        auto key = std::string{};
        words >> key;
        if (key == "physics-debug") {
            auto& options = result.preferences.physics_debug;
            options.categories = 0;
            for (auto name = std::string{}; words >> name;)
                for (const auto& [category, known] : categories)
                    if (name == known) options.categories |= uint8_t(category);
        } else if (key == "physics-debug-groups") {
            auto text = std::string{};
            words >> text;
            auto groups = uint16_t{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), groups, 16);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) result.preferences.physics_debug.groups = groups;
            else result.error = "physics-debug-groups needs a hexadecimal mask";
        } else if (key == "exposure-view") {
            auto name = std::string{};
            words >> name;
            const auto known = std::ranges::find(exposure_views, name, [](const auto& entry) { return std::string(entry.second); });
            if (known != std::end(exposure_views)) result.preferences.exposure_view = known->first;
            else result.error = "exposure-view needs none, luminance, or false-color";
        } else if (key == "shadow-view") {
            auto name = std::string{};
            words >> name;
            const auto known = std::ranges::find(shadow_views, name, [](const auto& entry) { return std::string(entry.second); });
            if (known != std::end(shadow_views)) result.preferences.shadow_view = known->first;
            else result.error = "shadow-view needs none, cascades, or texels";
        }
    }
    return result;
}

std::filesystem::path default_preferences_file() {
    const auto* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home) / "Library/Application Support/Maya/editor.preferences";
}

} // namespace maya::editor
