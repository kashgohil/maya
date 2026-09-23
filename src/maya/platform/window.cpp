#include "maya/platform/window.hpp"
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
}

Window::Window(int width, int height, const std::string& title) {
    if (window_count == 0 && !glfwInit()) return;
    ++window_count;
    m_glfw_acquired = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) return;

    // Show and focus window on macOS
    glfwShowWindow(m_window);
    glfwFocusWindow(m_window);

    // Set this window instance as user pointer for callbacks
    glfwSetWindowUserPointer(m_window, this);

    // Keyboard callback
    glfwSetKeyCallback(m_window, [](GLFWwindow*, int key, int, int action, int) {
        if (action == GLFW_REPEAT) return;
        Input::instance().set_key_state(static_cast<KeyCode>(key), action == GLFW_PRESS);
    });

    // Mouse position callback
    glfwSetCursorPosCallback(m_window, [](GLFWwindow*, double xpos, double ypos) {
        Input::instance().set_mouse_position(static_cast<float>(xpos), static_cast<float>(ypos));
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

std::pair<int, int> Window::framebuffer_size() const {
    int width = 0;
    int height = 0;
    if (m_window) glfwGetFramebufferSize(m_window, &width, &height);
    return {width, height};
}

void Window::set_cursor_captured(bool captured) {
    if (m_window) glfwSetInputMode(m_window, GLFW_CURSOR,
        captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

} // namespace maya
