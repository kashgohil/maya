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
inline constexpr const char* plus = "\xEE\x8F\x94"; // U+E3D4 create
inline constexpr const char* copy = "\xEE\x87\x8A"; // U+E1CA duplicate
inline constexpr const char* trash = "\xEE\x92\xA6"; // U+E4A6 delete
inline constexpr const char* pencil = "\xEE\x8E\xB4"; // U+E3B4 rename
inline constexpr const char* undo = "\xEE\x82\x8A"; // U+E08A undo
inline constexpr const char* redo = "\xEE\x82\x8C"; // U+E08C redo
inline constexpr const char* unparent = "\xEE\x81\x8C"; // U+E04C move to root
inline constexpr const char* rotate = "\xEE\x80\xB6"; // U+E036 rotate tool
inline constexpr const char* scale = "\xEE\x82\xA6"; // U+E0A6 scale tool
inline constexpr const char* globe = "\xEE\x8A\x8E"; // U+E28E world space
inline constexpr const char* frame = "\xEE\x98\xA6"; // U+E626 frame selection
inline constexpr const char* floppy_disk = "\xEE\x89\x88"; // U+E248 save
inline constexpr const char* file_plus = "\xEE\x88\xB6"; // U+E236 new scene
inline constexpr const char* caret_down = "\xEE\x84\xB6"; // U+E136 menu
inline constexpr const char* arrows_clockwise = "\xEE\x82\x94"; // U+E094 reload or refresh
inline constexpr const char* magnifying_glass = "\xEE\x8C\x8C"; // U+E30C filter
inline constexpr const char* folder_open = "\xEE\x89\x96"; // U+E256 open
inline constexpr const char* check = "\xEE\x86\x82"; // U+E182 current choice
inline constexpr const char* play = "\xEE\x8F\x90"; // U+E3D0 play
inline constexpr const char* pause = "\xEE\x8E\x9E"; // U+E39E pause
inline constexpr const char* stop = "\xEE\x91\xAC"; // U+E46C stop
inline constexpr const char* skip_forward = "\xEE\x96\xA6"; // U+E5A6 step one tick
inline constexpr const char* game_controller = "\xEE\x89\xAE"; // U+E26E fly control; game input
inline constexpr const char* bounding_box = "\xEE\x9B\x8E"; // U+E6CE collider
inline constexpr const char* atom = "\xEE\x97\xA4"; // U+E5E4 rigid body
inline constexpr const char* planet = "\xEE\x99\x92"; // U+E652 physics settings
inline constexpr const char* stack = "\xEE\x91\xA6"; // U+E466 collision groups

/// Glyph ranges for the atlas: each icon codepoint, then a terminating zero.
inline constexpr ImWchar ranges[] = {0xE000, 0xE000, 0xE036, 0xE036, 0xE04C, 0xE04C, 0xE08A, 0xE08A, 0xE08C, 0xE08C, 0xE094, 0xE094, 0xE0A4, 0xE0A4, 0xE0A6, 0xE0A6, 0xE136, 0xE136, 0xE182, 0xE182, 0xE184, 0xE184, 0xE18C, 0xE18C, 0xE1CA, 0xE1CA, 0xE1DA, 0xE1DA, 0xE230, 0xE230, 0xE236, 0xE236, 0xE248, 0xE248, 0xE256, 0xE256, 0xE25A, 0xE25A, 0xE26E, 0xE26E, 0xE28E, 0xE28E, 0xE2CE, 0xE2CE, 0xE30C, 0xE30C, 0xE332, 0xE332, 0xE336, 0xE336, 0xE39E, 0xE39E, 0xE3B4, 0xE3B4, 0xE3D0, 0xE3D0, 0xE3D4, 0xE3D4, 0xE434, 0xE434, 0xE466, 0xE466, 0xE46C, 0xE46C, 0xE472, 0xE472, 0xE4A6, 0xE4A6, 0xE4DA, 0xE4DA, 0xE4E0, 0xE4E0, 0xE4F8, 0xE4F8, 0xE5A6, 0xE5A6, 0xE5E4, 0xE5E4, 0xE602, 0xE602, 0xE626, 0xE626, 0xE652, 0xE652, 0xE67C, 0xE67C, 0xE6CE, 0xE6CE, 0xED0A, 0xED0A, 0};
} // namespace maya::editor::icon
