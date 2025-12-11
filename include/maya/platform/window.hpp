#pragma once

#include "maya/math/types.hpp"

namespace maya {

struct WindowConfig {
  const char *title = "Maya Window";
  u32 width = 1280;
  u32 height = 720;
  bool fullscreen = false;
  bool resizable = true;
};

class Window {
public:
  Window() = default;
  ~Window();

  bool create(const WindowConfig &config);
  void destroy();

  void poll_events();
  bool should_close() const;

  u32 get_width() const { return m_width; }
  u32 get_height() const { return m_height; }

  void *get_native_handle() const { return m_native_handle; }

  // Allow callbacks to access width/height
  u32 m_width = 0;
  u32 m_height = 0;

private:
  void *m_native_handle = nullptr; // GLFWwindow*
};

} // namespace maya
