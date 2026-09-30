#pragma once
// Internal helpers shared by the editor shell's source files.

#include "editor_icons.hpp"
#include "editor_theme.hpp"
#include "maya/assets/asset.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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


/// Where to draw `glyph` so its drawn shape, not its line box, is centred on `center`. Icons are
/// merged into the text font with a baseline offset that suits inline text, so their line box is
/// off-centre for a glyph standing on its own.
inline ImVec2 glyph_origin(const char* glyph, ImVec2 center) {
    auto codepoint = 0u;
    ImTextCharFromUtf8(&codepoint, glyph, nullptr);
    auto* font = ImGui::GetFont();
    const auto* found = font->FindGlyph(ImWchar(codepoint));
    if (!found) {
        const auto size = ImGui::CalcTextSize(glyph);
        return {center.x - size.x * 0.5f, center.y - size.y * 0.5f};
    }
    const auto scale = ImGui::GetFontSize() / font->FontSize;
    return {std::round(center.x - (found->X0 + found->X1) * 0.5f * scale), std::round(center.y - (found->Y0 + found->Y1) * 0.5f * scale)};
}

/// One item of a hint line: optional text before an icon (such as "Alt"), the icon, and its label.
struct HintItem {
    std::string prefix;
    const char* icon = nullptr;
    std::string label;
};
/// Lays out hint items on one line from `origin` (the top of the text line), each icon centred on
/// the text's capital height rather than sitting on its baseline. Draws them when `draw` is given;
/// returns the line's width either way.
inline float hint_line(ImDrawList* draw, ImVec2 origin, const std::vector<HintItem>& items, ImU32 color) {
    constexpr float icon_gap = 6.0f, item_gap = 18.0f;
    auto* font = ImGui::GetFont();
    const auto scale = ImGui::GetFontSize() / font->FontSize;
    const auto* capital = font->FindGlyph('H');
    const auto middle = origin.y + (capital ? (capital->Y0 + capital->Y1) * 0.5f * scale : ImGui::GetTextLineHeight() * 0.5f);
    auto x = origin.x;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (i > 0) x += item_gap;
        if (!item.prefix.empty()) {
            if (draw) draw->AddText({x, origin.y}, color, item.prefix.c_str());
            x += ImGui::CalcTextSize(item.prefix.c_str()).x + icon_gap;
        }
        if (item.icon) {
            const auto width = ImGui::CalcTextSize(item.icon).x;
            if (draw) draw->AddText(glyph_origin(item.icon, {x + width * 0.5f, middle}), color, item.icon);
            x += width + (item.label.empty() ? 0.0f : icon_gap);
        }
        if (!item.label.empty()) {
            if (draw) draw->AddText({x, origin.y}, color, item.label.c_str());
            x += ImGui::CalcTextSize(item.label.c_str()).x;
        }
    }
    return x - origin.x;
}

/// Applies the theme's colors and line weights to ImGuizmo (viewport_tools.cpp).
void style_gizmo();

} // namespace maya::editor::detail
