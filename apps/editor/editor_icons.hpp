#pragma once

#include <imgui.h>

namespace maya::editor::icon {
// Phosphor Icons (Light weight, MIT license): resources/fonts/Phosphor-Light.ttf.
// UTF-8 strings for use in ImGui text; only these glyphs are added to the font atlas.
inline constexpr const char* tree_structure = "\xEE\x99\xBC"; // U+E67C Hierarchy panel
inline constexpr const char* cube_focus = "\xEE\xB4\x8A"; // U+ED0A Viewport panel
inline constexpr const char* sliders = "\xEE\x90\xB4"; // U+E434 Inspector panel
inline constexpr const char* folder = "\xEE\x89\x9A"; // U+E25A Assets panel
inline constexpr const char* pulse = "\xEE\x80\x80"; // U+E000 Diagnostics panel
inline constexpr const char* video_camera = "\xEE\x93\x9A"; // U+E4DA camera entity
inline constexpr const char* sun = "\xEE\x91\xB2"; // U+E472 light entity
inline constexpr const char* cube = "\xEE\x87\x9A"; // U+E1DA mesh entity or asset
inline constexpr const char* circle_dashed = "\xEE\x98\x82"; // U+E602 other entity
inline constexpr const char* circle_half = "\xEE\x86\x8C"; // U+E18C material asset
inline constexpr const char* file = "\xEE\x88\xB0"; // U+E230 scene file
inline constexpr const char* info = "\xEE\x8B\x8E"; // U+E2CE informational log entry
inline constexpr const char* warning = "\xEE\x93\xA0"; // U+E4E0 warning
inline constexpr const char* x_circle = "\xEE\x93\xB8"; // U+E4F8 error
inline constexpr const char* check_circle = "\xEE\x86\x84"; // U+E184 ready
inline constexpr const char* arrows_move = "\xEE\x82\xA4"; // U+E0A4 flying
inline constexpr const char* mouse_right = "\xEE\x8C\xB6"; // U+E336 right mouse button
inline constexpr const char* mouse_scroll = "\xEE\x8C\xB2"; // U+E332 mouse wheel

/// Glyph ranges for the atlas: each icon codepoint, then a terminating zero.
inline constexpr ImWchar ranges[] = {0xE000, 0xE000, 0xE0A4, 0xE0A4, 0xE184, 0xE184, 0xE18C, 0xE18C, 0xE1DA, 0xE1DA, 0xE230, 0xE230, 0xE25A, 0xE25A, 0xE2CE, 0xE2CE, 0xE332, 0xE332, 0xE336, 0xE336, 0xE434, 0xE434, 0xE472, 0xE472, 0xE4DA, 0xE4DA, 0xE4E0, 0xE4E0, 0xE4F8, 0xE4F8, 0xE602, 0xE602, 0xE67C, 0xE67C, 0xED0A, 0xED0A, 0};
} // namespace maya::editor::icon
