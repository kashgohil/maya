#include "maya/platform/window.hpp"
#include <GLFW/glfw3.h>
#include <iostream>

namespace maya {

static bool s_glfw_initialized = false;
static u32 s_window_count = 0;

static void glfw_error_callback(int error, const char *description) {
  std::cerr << "[GLFW Error " << error << "] " << description << "\n";
}

static void glfw_framebuffer_size_callback(GLFWwindow *window, int width,
                                           int height) {
  Window *win = static_cast<Window *>(glfwGetWindowUserPointer(window));
  if (win) {
    win->m_width = static_cast<u32>(width);
    win->m_height = static_cast<u32>(height);
    std::cout << "[Window] Resized to " << width << "x" << height << "\n";
  }
}

Window::~Window() { destroy(); }

bool Window::create(const WindowConfig &config) {
  std::cout << "[Window] Creating window: " << config.title << "\n";
  std::cout << "[Window] Size: " << config.width << "x" << config.height
            << "\n";

  if (!s_glfw_initialized) {
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
      std::cerr << "[Window] Failed to initialize GLFW!\n";
      return false;
    }

    s_glfw_initialized = true;
    std::cout << "[Window] GLFW initialized\n";
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

  GLFWmonitor *monitor = config.fullscreen ? glfwGetPrimaryMonitor() : nullptr;

  GLFWwindow *window = glfwCreateWindow(static_cast<int>(config.width),
                                        static_cast<int>(config.height),
                                        config.title, monitor, nullptr);

  if (!window) {
    std::cerr << "[Window] Failed to create GLFW window!\n";
    return false;
  }

  m_native_handle = window;
  m_width = config.width;
  m_height = config.height;
  s_window_count++;

  glfwSetWindowUserPointer(window, this);
  glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);

  std::cout << "[Window] Window created successfully\n";
  return true;
}

void Window::destroy() {
  if (m_native_handle) {
    std::cout << "[Window] Destroying window\n";

    GLFWwindow *window = static_cast<GLFWwindow *>(m_native_handle);
    glfwDestroyWindow(window);
    m_native_handle = nullptr;

    s_window_count--;
    if (s_window_count == 0 && s_glfw_initialized) {
      glfwTerminate();
      s_glfw_initialized = false;
      std::cout << "[Window] GLFW terminated\n";
    }
  }
}

void Window::poll_events() { glfwPollEvents(); }

bool Window::should_close() const {
  if (!m_native_handle) {
    return true;
  }

  GLFWwindow *window = static_cast<GLFWwindow *>(m_native_handle);
  return glfwWindowShouldClose(window);
}

} // namespace maya
