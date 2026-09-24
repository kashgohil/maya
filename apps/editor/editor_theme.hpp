#pragma once

#include <imgui.h>

namespace maya::editor::theme {

// A neutral near-black palette with one cool accent. Surfaces step up in lightness; borders are
// hairlines; color is reserved for state (accent, success, warning, danger).
namespace color {
inline constexpr ImU32 rgb(unsigned value, unsigned alpha = 255) {
    return IM_COL32((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, alpha);
}
inline constexpr ImU32 background = rgb(0x0B0C0E); // app chrome, tab strips
inline constexpr ImU32 panel = rgb(0x111215); // panel bodies
inline constexpr ImU32 surface = rgb(0x18191D); // fields, buttons
inline constexpr ImU32 hover = rgb(0x1F2025);
inline constexpr ImU32 active = rgb(0x26282E);
inline constexpr ImU32 border = rgb(0x222328);
inline constexpr ImU32 text = rgb(0xE6E7EA);
inline constexpr ImU32 muted = rgb(0x8A8D95);
inline constexpr ImU32 faint = rgb(0x5B5E66);
inline constexpr ImU32 accent = rgb(0x7B8CFF);
inline constexpr ImU32 success = rgb(0x4ADE80);
inline constexpr ImU32 warning = rgb(0xF5B454);
inline constexpr ImU32 danger = rgb(0xF2616B);
} // namespace color

/// Fonts built for the current display scale; any may fall back to ImGui's built-in font.
struct Fonts {
    ImFont* body = nullptr; // Inter Regular, 14 pt
    ImFont* strong = nullptr; // Inter SemiBold, 14 pt
    ImFont* caption = nullptr; // Inter SemiBold, 11 pt: section captions
    ImFont* mono = nullptr; // Geist Mono, 12.5 pt: numbers, IDs, and paths
};
inline constexpr float body_size = 14.0f;
inline constexpr float caption_size = 11.0f;
inline constexpr float mono_size = 12.5f;

void apply(ImGuiStyle& style);

/// Small uppercase heading in the caption font, with optional right-aligned muted detail.
void caption(const Fonts& fonts, const char* text, const char* detail = nullptr);
/// A filled circle on the current line, vertically centered on text; advances the cursor.
void dot(ImU32 color, float radius = 3.5f);
/// Rounded label chip in the mono font.
void pill(const Fonts& fonts, const char* text, ImU32 foreground, ImU32 background);
/// Two-column property grid: muted labels on the left, full-width controls on the right.
bool begin_properties(const char* id);
void property(const char* label); // starts a row and moves to the control column
void end_properties();
/// Text in the mono font, optionally muted.
void mono_text(const Fonts& fonts, const char* text, bool muted = false);

} // namespace maya::editor::theme
