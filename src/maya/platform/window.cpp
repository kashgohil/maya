#include "maya/platform/window.hpp"
#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

namespace maya {

Window::Window(int width, int height, const std::string& title) {
    if (!glfwInit()) return;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
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
