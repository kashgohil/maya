#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include "maya/math/vector.hpp"

namespace maya {

/// Key codes use GLFW's values, so any GLFW key converts directly; the named keys are the ones the
/// engine and editor refer to.
enum class KeyCode {
    Unknown = -1,
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Semicolon = 59, Equal = 61,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
    Right = 262, Left = 263, Down = 264, Up = 265, PageUp = 266, PageDown = 267, Home = 268, End = 269,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342, LeftSuper = 343,
    RightShift = 344, RightControl = 345, RightAlt = 346, RightSuper = 347
};

enum class KeyModifiers : uint8_t { none = 0, shift = 1, control = 2, alt = 4, super = 8 };
constexpr bool has_modifier(KeyModifiers set, KeyModifiers flag) noexcept {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(flag)) != 0;
}
enum class MouseButton : uint8_t { left, right, middle, other };

// Window input in delivery order. Positions are in window points (logical coordinates), origin at
// the top left; multiply by WindowMetrics::scale() for framebuffer pixels.
struct KeyEvent { KeyCode key; bool down; KeyModifiers modifiers; };
struct TextEvent { uint32_t codepoint; };
struct MouseMoveEvent { float x; float y; };
struct MouseButtonEvent { MouseButton button; bool down; KeyModifiers modifiers; };
struct ScrollEvent { float x; float y; };
struct FocusEvent { bool focused; };
using InputEvent = std::variant<KeyEvent, TextEvent, MouseMoveEvent, MouseButtonEvent, ScrollEvent, FocusEvent>;

/// Window size in points and framebuffer pixels. They differ on high-density (Retina) displays.
struct WindowMetrics {
    float width = 0.0f; // points
    float height = 0.0f;
    uint32_t framebuffer_width = 0; // pixels
    uint32_t framebuffer_height = 0;
    /// Framebuffer pixels per point, or 1 for an empty window.
    float scale() const noexcept { return width > 0.0f ? float(framebuffer_width) / width : 1.0f; }
};

enum class CursorShape : uint8_t { arrow, text, hand, resize_horizontal, resize_vertical };

/// Host services an application may use without depending on the window library.
struct PlatformServices {
    std::function<std::string()> get_clipboard;
    std::function<void(const std::string&)> set_clipboard;
    std::function<void(CursorShape)> set_cursor;
};

/// Per-window input shared by the desktop host and applications (main thread only). The host
/// records callbacks, publishes metrics before each tick, and calls update() after it. Held-key
/// state serves simple controllers; events() carries this frame's ordered events for UIs.
class Input {
public:
    static Input& instance() {
        static Input s_instance;
        return s_instance;
    }

    // Key states
    bool is_key_down(KeyCode key) const {
        auto it = m_keys.find(key);
        return it != m_keys.end() && it->second;
    }

    bool is_key_pressed(KeyCode key) const {
        auto it = m_keys.find(key);
        auto it_prev = m_prev_keys.find(key);
        bool currently_down = (it != m_keys.end() && it->second);
        bool previously_down = (it_prev != m_prev_keys.end() && it_prev->second);
        return currently_down && !previously_down;
    }

    // Internal setters (called by Window callbacks)
    void set_key_state(KeyCode key, bool down) {
        m_keys[key] = down;
    }

    void set_mouse_position(float x, float y) {
        m_mouse_pos = {x, y};
    }

    const math::Vec2& get_mouse_position() const { return m_mouse_pos; }

    /// Events received since the last update(), in order.
    const std::vector<InputEvent>& events() const noexcept { return m_events; }
    void push_event(const InputEvent& event) { m_events.push_back(event); }

    const WindowMetrics& window_metrics() const noexcept { return m_metrics; }
    void set_window_metrics(const WindowMetrics& metrics) noexcept { m_metrics = metrics; }

    /// Asks the host to capture (hide and lock) or release the cursor after this tick.
    void request_cursor_capture(bool captured) noexcept { m_capture_request = captured; }
    std::optional<bool> take_cursor_capture_request() noexcept { return std::exchange(m_capture_request, std::nullopt); }

    const PlatformServices& services() const noexcept { return m_services; }
    void set_services(PlatformServices services) { m_services = std::move(services); }

    // Lifecycle
    void reset() {
        m_keys.clear();
        m_prev_keys.clear();
        m_mouse_pos = {};
        m_events.clear();
        m_metrics = {};
        m_capture_request.reset();
        m_services = {};
    }

    void update() {
        m_prev_keys = m_keys;
        m_events.clear();
    }

private:
    Input() = default;

    std::unordered_map<KeyCode, bool> m_keys;
    std::unordered_map<KeyCode, bool> m_prev_keys;
    math::Vec2 m_mouse_pos;
    std::vector<InputEvent> m_events;
    WindowMetrics m_metrics;
    std::optional<bool> m_capture_request;
    PlatformServices m_services;
};

} // namespace maya
