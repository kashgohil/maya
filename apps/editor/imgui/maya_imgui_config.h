#pragma once
// Dear ImGui build configuration for the Maya editor (IMGUI_USER_CONFIG).

// 32-bit indices keep every index offset 4-byte aligned, as the Maya RHI requires, and let one draw
// list address all of its vertices without per-command vertex offsets.
#define ImDrawIdx unsigned int
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
// Maya supplies clipboard access through its platform services instead of OS-specific defaults.
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
