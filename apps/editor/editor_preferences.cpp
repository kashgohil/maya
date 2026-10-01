#include "editor_preferences.hpp"
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
