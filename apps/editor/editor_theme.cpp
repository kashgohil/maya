#include "editor_theme.hpp"
#include <imgui_internal.h>

namespace maya::editor::theme {
namespace {
ImVec4 v(ImU32 color, float alpha = 1.0f) {
    auto value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= alpha;
    return value;
}
} // namespace

void apply(ImGuiStyle& style) {
    style = ImGuiStyle{};
    style.WindowPadding = {14.0f, 12.0f};
    style.FramePadding = {10.0f, 5.0f};
    style.CellPadding = {8.0f, 6.0f};
    style.ItemSpacing = {10.0f, 8.0f};
    style.ItemInnerSpacing = {8.0f, 6.0f};
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 0.0f; // decorate_tabs draws the separator and the selected underline
    style.TabBarOverlineSize = 0.0f;
    style.DockingSeparatorSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowTitleAlign = {0.0f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.5f};

    auto* c = style.Colors;
    c[ImGuiCol_Text] = v(color::text);
    c[ImGuiCol_TextDisabled] = v(color::muted);
    c[ImGuiCol_WindowBg] = v(color::panel);
    c[ImGuiCol_ChildBg] = v(color::panel, 0.0f);
    c[ImGuiCol_PopupBg] = v(color::rgb(0x16171A));
    c[ImGuiCol_Border] = v(color::border);
    c[ImGuiCol_BorderShadow] = v(0, 0.0f);
    c[ImGuiCol_FrameBg] = v(color::surface);
    c[ImGuiCol_FrameBgHovered] = v(color::hover);
    c[ImGuiCol_FrameBgActive] = v(color::active);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = v(color::panel);
    c[ImGuiCol_MenuBarBg] = v(color::background);
    c[ImGuiCol_ScrollbarBg] = v(0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = v(color::rgb(0x2A2C32));
    c[ImGuiCol_ScrollbarGrabHovered] = v(color::rgb(0x35373E));
    c[ImGuiCol_ScrollbarGrabActive] = v(color::rgb(0x40424A));
    c[ImGuiCol_CheckMark] = v(color::accent);
    c[ImGuiCol_SliderGrab] = v(color::accent, 0.85f);
    c[ImGuiCol_SliderGrabActive] = v(color::accent);
    c[ImGuiCol_Button] = v(color::surface);
    c[ImGuiCol_ButtonHovered] = v(color::hover);
    c[ImGuiCol_ButtonActive] = v(color::active);
    c[ImGuiCol_Header] = v(color::accent, 0.16f);
    c[ImGuiCol_HeaderHovered] = v(color::rgb(0xFFFFFF), 0.05f);
    c[ImGuiCol_HeaderActive] = v(color::accent, 0.24f);
    c[ImGuiCol_Separator] = v(color::border);
    c[ImGuiCol_SeparatorHovered] = v(color::accent, 0.6f);
    c[ImGuiCol_SeparatorActive] = v(color::accent);
    c[ImGuiCol_ResizeGrip] = v(0, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = v(color::accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = v(color::accent);
    // Tabs are flat: no fill except a soft hover; the strip shares the panel color.
    c[ImGuiCol_Tab] = v(color::panel, 0.0f);
    c[ImGuiCol_TabHovered] = v(color::rgb(0xFFFFFF), 0.04f);
    c[ImGuiCol_TabSelected] = v(color::panel);
    c[ImGuiCol_TabSelectedOverline] = v(color::accent, 0.0f);
    c[ImGuiCol_TabDimmed] = v(color::panel, 0.0f);
    c[ImGuiCol_TabDimmedSelected] = v(color::panel);
    c[ImGuiCol_TabDimmedSelectedOverline] = v(color::accent, 0.0f);
    c[ImGuiCol_DockingPreview] = v(color::accent, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = v(color::background);
    c[ImGuiCol_TableHeaderBg] = v(color::panel);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = v(color::border);
    c[ImGuiCol_TableRowBg] = v(0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = v(color::rgb(0xFFFFFF), 0.02f);
    c[ImGuiCol_TextLink] = v(color::accent);
    c[ImGuiCol_TextSelectedBg] = v(color::accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = v(color::accent);
    c[ImGuiCol_NavCursor] = v(color::accent);
    c[ImGuiCol_ModalWindowDimBg] = v(color::background, 0.6f);
}

void decorate_tabs(const char* const* window_names, int count) {
    auto seen = ImVector<ImGuiDockNode*>{};
    for (int i = 0; i < count; ++i) {
        auto* window = ImGui::FindWindowByName(window_names[i]);
        if (!window || !window->DockIsActive || !window->DockNode) continue;
        auto* node = window->DockNode;
        auto* bar = node->TabBar;
        if (!bar || !node->HostWindow || node->IsHiddenTabBar() || node->IsNoTabBar()) continue;
        auto* draw = node->HostWindow->DrawList;
        const auto bottom = bar->BarRect.Max.y;
        if (!seen.contains(node)) {
            seen.push_back(node);
            draw->AddLine({node->Pos.x, bottom - 0.5f}, {node->Pos.x + node->Size.x, bottom - 0.5f}, color::border);
        }
        if (bar->VisibleTabId != window->TabId) continue;
        // ImGui draws every tab label in the muted text color; repaint the visible tab's label brightly.
        const auto rect = window->DC.DockTabItemRect;
        draw->AddRectFilled(rect.Min, {rect.Max.x, bottom - 1.0f}, color::panel);
        const char* end = ImGui::FindRenderedTextEnd(window->Name);
        draw->PushClipRect(rect.Min, {rect.Max.x - bar->FramePadding.x * 0.5f, rect.Max.y}, true);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {rect.Min.x + bar->FramePadding.x, rect.Min.y + bar->FramePadding.y},
                      color::text, window->Name, end);
        draw->PopClipRect();
        const auto focused = (bar->Flags & ImGuiTabBarFlags_IsFocused) != 0;
        draw->AddRectFilled({rect.Min.x + 8.0f, bottom - 2.0f}, {rect.Max.x - 8.0f, bottom},
                            focused ? color::accent : color::rgb(0x3A3C43), 1.0f);
    }
}

void caption(const Fonts& fonts, const char* text, const char* detail) {
    ImGui::PushFont(fonts.caption);
    ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
    ImGui::TextUnformatted(text);
    if (detail) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(detail).x);
        ImGui::PushStyleColor(ImGuiCol_Text, color::faint);
        ImGui::TextUnformatted(detail);
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void dot(ImU32 fill, float radius) {
    const auto position = ImGui::GetCursorScreenPos();
    const auto height = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled({position.x + radius, position.y + height * 0.5f}, radius, fill, 16);
    ImGui::Dummy({radius * 2.0f, height});
    ImGui::SameLine(0.0f, 8.0f);
}

void pill(const Fonts& fonts, const char* text, ImU32 foreground, ImU32 background) {
    ImGui::PushFont(fonts.mono);
    const auto size = ImGui::CalcTextSize(text);
    const auto padding = ImVec2{6.0f, 1.0f};
    const auto line = ImGui::GetTextLineHeight();
    const auto position = ImGui::GetCursorScreenPos();
    const auto top = position.y + (line - size.y) * 0.5f - padding.y;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({position.x, top}, {position.x + size.x + padding.x * 2.0f, top + size.y + padding.y * 2.0f},
                        background, 4.0f);
    draw->AddText({position.x + padding.x, top + padding.y}, foreground, text);
    ImGui::Dummy({size.x + padding.x * 2.0f, line});
    ImGui::PopFont();
}

bool begin_properties(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {10.0f, 2.0f});
    if (!ImGui::BeginTable(id, 2)) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed); // fits the widest label
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void property(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void end_properties() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void mono_text(const Fonts& fonts, const char* text, bool muted) {
    ImGui::PushFont(fonts.mono);
    if (muted) ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
    ImGui::TextUnformatted(text);
    if (muted) ImGui::PopStyleColor();
    ImGui::PopFont();
}

} // namespace maya::editor::theme
