#pragma once

#include <functional>
#include <string>

struct GLFWwindow;

namespace maya {

using FramebufferResizeCallback = std::function<void(int width, int height)>;

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    bool should_close() const;
    void poll_events();
    void* get_native_handle() const;
    GLFWwindow* get_glfw_window() const { return m_window; }

    void set_framebuffer_resize_callback(FramebufferResizeCallback callback);
    void set_title(const std::string& title);

private:
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window;
    FramebufferResizeCallback m_framebuffer_resize_callback;
};

} // namespace maya
