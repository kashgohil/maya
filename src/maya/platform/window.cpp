#include "maya/platform/window.hpp"
#include "maya/platform/input.hpp"
#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

namespace maya {

Window::Window(int width, int height, const std::string& title) {
    if (!glfwInit()) return;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) return;

    // Set this window instance as user pointer for callbacks
    glfwSetWindowUserPointer(m_window, this);

    // Keyboard callback
    glfwSetKeyCallback(m_window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
        if (action == GLFW_REPEAT) return;
        Input::instance().set_key_state(static_cast<KeyCode>(key), action == GLFW_PRESS);
    });

    // Mouse position callback
    glfwSetCursorPosCallback(m_window, [](GLFWwindow* window, double xpos, double ypos) {
        Input::instance().set_mouse_position(static_cast<float>(xpos), static_cast<float>(ypos));
    });
}

Window::~Window() {
    if (m_window) {
        glfwDestroyWindow(m_window);
    }
    glfwTerminate();
}

bool Window::should_close() const {
    return glfwWindowShouldClose(m_window);
}

void Window::poll_events() {
    glfwPollEvents();
}

void* Window::get_native_handle() const {
    return glfwGetCocoaWindow(m_window);
}

} // namespace maya
