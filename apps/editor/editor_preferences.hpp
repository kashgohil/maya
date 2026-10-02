#pragma once

#include "maya/renderer/render_snapshot.hpp"
#include "maya/simulation/physics_debug.hpp"
#include <filesystem>
#include <iosfwd>
#include <string>

namespace maya::editor {

/// The editor's own settings for the person using it, kept apart from projects: a versioned text file
/// ("maya-editor-preferences 1"), in ~/Library/Application Support/Maya by default.
struct EditorPreferences {
    PhysicsDebugOptions physics_debug{}; // the viewport's physics debug views
    ExposureView exposure_view = ExposureView::none; // the viewport's exposure view (docs/renderer.md#exposure-views)
    bool operator==(const EditorPreferences&) const = default;
};

void write_preferences(std::ostream& output, const EditorPreferences& preferences);
struct PreferencesReadResult {
    EditorPreferences preferences; // defaults for anything the file does not set
    std::string error; // why the file was not read in full, or empty
};
/// Unknown lines are skipped, so a newer editor's file still opens.
PreferencesReadResult read_preferences(std::istream& input);
/// Where the editor keeps preferences unless told otherwise, or empty without a home folder.
std::filesystem::path default_preferences_file();

} // namespace maya::editor
