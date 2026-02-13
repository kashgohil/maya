#pragma once

#include <string>

struct GLFWwindow;

namespace maya {

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    bool should_close() const;
    void poll_events();
    void* get_native_handle() const;
    GLFWwindow* get_glfw_window() const { return m_window; }

private:
    GLFWwindow* m_window;
};

} // namespace maya
