#pragma once

#include "maya/platform/input.hpp"
#include <array>
#include <functional>
#include <string>
#include <utility>

struct GLFWwindow;
struct GLFWcursor;

namespace maya {

using FramebufferResizeCallback = std::function<void(int width, int height)>;

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool should_close() const;
    void poll_events();
    void* get_native_handle() const;
    GLFWwindow* get_glfw_window() const { return m_window; }

    void set_framebuffer_resize_callback(FramebufferResizeCallback callback);
    void set_title(const std::string& title);
    std::pair<int, int> framebuffer_size() const;
    /// Size in points (logical coordinates); smaller than the framebuffer on Retina displays.
    std::pair<int, int> window_size() const;
    std::string clipboard_text() const;
    void set_clipboard_text(const std::string& text);
    void set_cursor_captured(bool captured);
    void set_cursor_shape(CursorShape shape);

private:
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window = nullptr;
    std::array<GLFWcursor*, 5> m_cursors{}; // standard cursors, created on first use
    bool m_glfw_acquired = false;
    FramebufferResizeCallback m_framebuffer_resize_callback;
};

} // namespace maya
