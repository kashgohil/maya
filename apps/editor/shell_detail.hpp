#pragma once
// Internal helpers shared by the editor shell's source files.

#include "editor_icons.hpp"
#include "editor_theme.hpp"
#include "maya/assets/asset.hpp"
#include <cstdio>
#include <string>

namespace maya::editor::detail {

inline std::string format(const char* pattern, auto... values) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, values...);
    return buffer;
}

inline std::string id_text(uint64_t high, uint64_t low) {
    char text[40];
    std::snprintf(text, sizeof(text), "%llx:%llx", static_cast<unsigned long long>(high), static_cast<unsigned long long>(low));
    return text;
}

/// What a "MAYA_ASSET" drag from the Assets panel carries.
struct AssetPayload {
    AssetId id;
    AssetKind kind;
};

// Panel titles carry an icon; the part after ### is the stable window ID used by the dock layout.
inline const std::string hierarchy_title = std::string(icon::tree_structure) + "  Hierarchy###Hierarchy";
inline const std::string viewport_title = std::string(icon::cube_focus) + "  Viewport###Viewport";
inline const std::string inspector_title = std::string(icon::sliders) + "  Inspector###Inspector";
inline const std::string assets_title = std::string(icon::folder) + "  Assets###Assets";
inline const std::string diagnostics_title = std::string(icon::pulse) + "  Diagnostics###Diagnostics";

inline const char* const panel_titles[] = {hierarchy_title.c_str(), viewport_title.c_str(), inspector_title.c_str(),
                                    assets_title.c_str(), diagnostics_title.c_str()};

/// Begins a docked panel with the tab padding, so its reserved title height matches its tab bar, and
/// muted text: ImGui may draw the node's tab bar here (see theme::decorate_tabs).
inline bool begin_panel(const std::string& title, ImGuiWindowFlags flags = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, theme::tab_padding);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
    const auto open = ImGui::Begin(title.c_str(), nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return open;
}

/// Draws an icon in a color, then continues on the same line.
inline void icon_text(const char* glyph, ImU32 color, float spacing = 8.0f) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, spacing);
}


/// Applies the theme's colors and line weights to ImGuizmo (viewport_tools.cpp).
void style_gizmo();

} // namespace maya::editor::detail
