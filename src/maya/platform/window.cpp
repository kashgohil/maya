#include "maya/platform/window.hpp"
#include "maya/platform/app_icon.hpp"
#include "maya/platform/input.hpp"
#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#define GLFW_NATIVE_INCLUDE_NONE
#include <CoreGraphics/CGDirectDisplay.h>
#include <objc/objc.h>
#include <GLFW/glfw3native.h>

namespace maya {

namespace {
// GLFW windows and this reference count are confined to the main thread.
unsigned int window_count = 0;

KeyModifiers modifiers(int mods) {
    return static_cast<KeyModifiers>(mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER));
}
MouseButton mouse_button(int button) {
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::left;
    case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::right;
    case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::middle;
    default: return MouseButton::other;
    }
}
}

Window::Window(int width, int height, const std::string& title) {
    if (window_count == 0 && !glfwInit()) return;
    ++window_count;
    m_glfw_acquired = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) return;

    set_application_icon(glfwGetCocoaWindow(m_window));

    // Show and focus window on macOS
    glfwShowWindow(m_window);
    glfwFocusWindow(m_window);

    // Set this window instance as user pointer for callbacks
    glfwSetWindowUserPointer(m_window, this);

    // Input callbacks update held state and record ordered events. Repeats are left to consumers.
    glfwSetKeyCallback(m_window, [](GLFWwindow*, int key, int, int action, int mods) {
        if (action == GLFW_REPEAT) return;
        const auto code = static_cast<KeyCode>(key);
        Input::instance().set_key_state(code, action == GLFW_PRESS);
        Input::instance().push_event(KeyEvent{code, action == GLFW_PRESS, modifiers(mods)});
    });
    glfwSetCharCallback(m_window, [](GLFWwindow*, unsigned int codepoint) {
        Input::instance().push_event(TextEvent{codepoint});
    });
    glfwSetCursorPosCallback(m_window, [](GLFWwindow*, double xpos, double ypos) {
        Input::instance().set_mouse_position(static_cast<float>(xpos), static_cast<float>(ypos));
        Input::instance().push_event(MouseMoveEvent{static_cast<float>(xpos), static_cast<float>(ypos)});
    });
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow*, int button, int action, int mods) {
        Input::instance().push_event(MouseButtonEvent{mouse_button(button), action == GLFW_PRESS, modifiers(mods)});
    });
    glfwSetScrollCallback(m_window, [](GLFWwindow*, double x, double y) {
        Input::instance().push_event(ScrollEvent{static_cast<float>(x), static_cast<float>(y)});
    });
    glfwSetWindowFocusCallback(m_window, [](GLFWwindow*, int focused) {
        Input::instance().push_event(FocusEvent{focused == GLFW_TRUE});
    });

    glfwSetFramebufferSizeCallback(m_window, framebuffer_size_callback);
}

void Window::framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    void* user = glfwGetWindowUserPointer(window);
    if (!user) return;
    auto* self = static_cast<Window*>(user);
    if (self->m_framebuffer_resize_callback) {
        self->m_framebuffer_resize_callback(width, height);
    }
}

void Window::set_framebuffer_resize_callback(FramebufferResizeCallback callback) {
    m_framebuffer_resize_callback = std::move(callback);
}

void Window::set_title(const std::string& title) {
    if (m_window) {
        glfwSetWindowTitle(m_window, title.c_str());
    }
}

Window::~Window() {
    if (m_window) {
        // Drop callbacks that may capture an already-stopped engine before native teardown.
        glfwSetFramebufferSizeCallback(m_window, nullptr);
        m_framebuffer_resize_callback = {};
        glfwDestroyWindow(m_window);
    }
    for (auto* cursor : m_cursors) if (cursor) glfwDestroyCursor(cursor);
    if (m_glfw_acquired && --window_count == 0) glfwTerminate();
}

bool Window::should_close() const {
    return !m_window || glfwWindowShouldClose(m_window);
}

void Window::poll_events() {
    if (m_glfw_acquired) glfwPollEvents();
}

void* Window::get_native_handle() const {
    return m_window ? glfwGetCocoaWindow(m_window) : nullptr;
}

std::pair<int, int> Window::window_size() const {
    int width = 0;
    int height = 0;
    if (m_window) glfwGetWindowSize(m_window, &width, &height);
    return {width, height};
}

std::string Window::clipboard_text() const {
    const char* text = m_window ? glfwGetClipboardString(m_window) : nullptr;
    return text ? text : "";
}

void Window::set_clipboard_text(const std::string& text) {
    if (m_window) glfwSetClipboardString(m_window, text.c_str());
}

std::pair<int, int> Window::framebuffer_size() const {
    int width = 0;
    int height = 0;
    if (m_window) glfwGetFramebufferSize(m_window, &width, &height);
    return {width, height};
}

void Window::set_cursor_shape(CursorShape shape) {
    if (!m_window) return;
    static constexpr int shapes[] = {GLFW_ARROW_CURSOR, GLFW_IBEAM_CURSOR, GLFW_HAND_CURSOR,
                                     GLFW_HRESIZE_CURSOR, GLFW_VRESIZE_CURSOR};
    const auto index = static_cast<size_t>(shape);
    if (index >= m_cursors.size()) return;
    if (!m_cursors[index]) m_cursors[index] = glfwCreateStandardCursor(shapes[index]);
    glfwSetCursor(m_window, m_cursors[index]);
}

void Window::set_cursor_captured(bool captured) {
    if (m_window) glfwSetInputMode(m_window, GLFW_CURSOR,
        captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

} // namespace maya
