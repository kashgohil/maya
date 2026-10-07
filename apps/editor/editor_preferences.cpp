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
} // namespace

void write_preferences(std::ostream& output, const EditorPreferences& preferences) {
    auto out = std::ostringstream{};
    out << "maya-editor-preferences 1\n";
    out << "physics-debug";
    for (const auto& [category, name] : categories)
        if (preferences.physics_debug.has(category)) out << ' ' << name;
    out << "\nphysics-debug-groups " << std::hex << preferences.physics_debug.groups << std::dec << '\n';
    out << "debug-view " << debug_view_name(preferences.debug_view) << '\n';
    out << "skeletons " << (preferences.skeletons ? "on" : "off") << '\n';
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
        } else if (key == "skeletons") {
            auto value = std::string{};
            words >> value;
            if (value == "on" || value == "off") result.preferences.skeletons = value == "on";
            else result.error = "skeletons needs on or off";
        } else if (key == "debug-view" || key == "exposure-view" || key == "shadow-view") {
            // One view at a time (#1037); files from #1032 and #1034 kept exposure and shadow views apart.
            auto name = std::string{};
            words >> name;
            const auto known = debug_view_named(name);
            if (known && key == "debug-view") result.preferences.debug_view = *known;
            else if (known && *known != DebugView::none) result.preferences.debug_view = *known;
            else if (!known) result.error = key + " needs a debug view's name, such as none, base-color, or false-color";
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
