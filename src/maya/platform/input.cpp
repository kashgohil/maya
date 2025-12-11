#include "maya/platform/input.hpp"
#include <GLFW/glfw3.h>
#include <cstring>

namespace maya {

// Key state tracking
static bool s_keys_down[static_cast<size_t>(Key::Count)] = {};
static bool s_keys_pressed[static_cast<size_t>(Key::Count)] = {};
static bool s_keys_released[static_cast<size_t>(Key::Count)] = {};

// Mouse state tracking
static bool s_mouse_buttons_down[static_cast<size_t>(MouseButton::Count)] = {};
static bool s_mouse_buttons_pressed[static_cast<size_t>(MouseButton::Count)] =
    {};
static bool s_mouse_buttons_released[static_cast<size_t>(MouseButton::Count)] =
    {};

static Vec2 s_mouse_position{0.0f, 0.0f};
static Vec2 s_mouse_last_position{0.0f, 0.0f};
static Vec2 s_mouse_delta{0.0f, 0.0f};
static f32 s_scroll_delta = 0.0f;

// GLFW to Maya key mapping
static Key glfw_to_maya_key(int key) {
  switch (key) {
  case GLFW_KEY_SPACE:
    return Key::Space;
  case GLFW_KEY_APOSTROPHE:
    return Key::Apostrophe;
  case GLFW_KEY_COMMA:
    return Key::Comma;
  case GLFW_KEY_MINUS:
    return Key::Minus;
  case GLFW_KEY_PERIOD:
    return Key::Period;
  case GLFW_KEY_SLASH:
    return Key::Slash;

  case GLFW_KEY_0:
    return Key::Num0;
  case GLFW_KEY_1:
    return Key::Num1;
  case GLFW_KEY_2:
    return Key::Num2;
  case GLFW_KEY_3:
    return Key::Num3;
  case GLFW_KEY_4:
    return Key::Num4;
  case GLFW_KEY_5:
    return Key::Num5;
  case GLFW_KEY_6:
    return Key::Num6;
  case GLFW_KEY_7:
    return Key::Num7;
  case GLFW_KEY_8:
    return Key::Num8;
  case GLFW_KEY_9:
    return Key::Num9;

  case GLFW_KEY_SEMICOLON:
    return Key::Semicolon;
  case GLFW_KEY_EQUAL:
    return Key::Equal;

  case GLFW_KEY_A:
    return Key::A;
  case GLFW_KEY_B:
    return Key::B;
  case GLFW_KEY_C:
    return Key::C;
  case GLFW_KEY_D:
    return Key::D;
  case GLFW_KEY_E:
    return Key::E;
  case GLFW_KEY_F:
    return Key::F;
  case GLFW_KEY_G:
    return Key::G;
  case GLFW_KEY_H:
    return Key::H;
  case GLFW_KEY_I:
    return Key::I;
  case GLFW_KEY_J:
    return Key::J;
  case GLFW_KEY_K:
    return Key::K;
  case GLFW_KEY_L:
    return Key::L;
  case GLFW_KEY_M:
    return Key::M;
  case GLFW_KEY_N:
    return Key::N;
  case GLFW_KEY_O:
    return Key::O;
  case GLFW_KEY_P:
    return Key::P;
  case GLFW_KEY_Q:
    return Key::Q;
  case GLFW_KEY_R:
    return Key::R;
  case GLFW_KEY_S:
    return Key::S;
  case GLFW_KEY_T:
    return Key::T;
  case GLFW_KEY_U:
    return Key::U;
  case GLFW_KEY_V:
    return Key::V;
  case GLFW_KEY_W:
    return Key::W;
  case GLFW_KEY_X:
    return Key::X;
  case GLFW_KEY_Y:
    return Key::Y;
  case GLFW_KEY_Z:
    return Key::Z;

  case GLFW_KEY_ESCAPE:
    return Key::Escape;
  case GLFW_KEY_ENTER:
    return Key::Enter;
  case GLFW_KEY_TAB:
    return Key::Tab;
  case GLFW_KEY_BACKSPACE:
    return Key::Backspace;
  case GLFW_KEY_INSERT:
    return Key::Insert;
  case GLFW_KEY_DELETE:
    return Key::Delete;
  case GLFW_KEY_RIGHT:
    return Key::Right;
  case GLFW_KEY_LEFT:
    return Key::Left;
  case GLFW_KEY_DOWN:
    return Key::Down;
  case GLFW_KEY_UP:
    return Key::Up;

  case GLFW_KEY_F1:
    return Key::F1;
  case GLFW_KEY_F2:
    return Key::F2;
  case GLFW_KEY_F3:
    return Key::F3;
  case GLFW_KEY_F4:
    return Key::F4;
  case GLFW_KEY_F5:
    return Key::F5;
  case GLFW_KEY_F6:
    return Key::F6;
  case GLFW_KEY_F7:
    return Key::F7;
  case GLFW_KEY_F8:
    return Key::F8;
  case GLFW_KEY_F9:
    return Key::F9;
  case GLFW_KEY_F10:
    return Key::F10;
  case GLFW_KEY_F11:
    return Key::F11;
  case GLFW_KEY_F12:
    return Key::F12;

  case GLFW_KEY_LEFT_SHIFT:
    return Key::LeftShift;
  case GLFW_KEY_LEFT_CONTROL:
    return Key::LeftControl;
  case GLFW_KEY_LEFT_ALT:
    return Key::LeftAlt;
  case GLFW_KEY_RIGHT_SHIFT:
    return Key::RightShift;
  case GLFW_KEY_RIGHT_CONTROL:
    return Key::RightControl;
  case GLFW_KEY_RIGHT_ALT:
    return Key::RightAlt;

  default:
    return Key::Unknown;
  }
}

// GLFW callbacks
void glfw_key_callback(GLFWwindow * /*window*/, int key, int /*scancode*/,
                       int action, int /*mods*/) {
  Key maya_key = glfw_to_maya_key(key);
  if (maya_key == Key::Unknown)
    return;

  size_t key_index = static_cast<size_t>(maya_key);

  if (action == GLFW_PRESS) {
    s_keys_down[key_index] = true;
    s_keys_pressed[key_index] = true;
  } else if (action == GLFW_RELEASE) {
    s_keys_down[key_index] = false;
    s_keys_released[key_index] = true;
  }
}

void glfw_mouse_button_callback(GLFWwindow * /*window*/, int button, int action,
                                int /*mods*/) {
  MouseButton maya_button = MouseButton::Count;

  switch (button) {
  case GLFW_MOUSE_BUTTON_LEFT:
    maya_button = MouseButton::Left;
    break;
  case GLFW_MOUSE_BUTTON_RIGHT:
    maya_button = MouseButton::Right;
    break;
  case GLFW_MOUSE_BUTTON_MIDDLE:
    maya_button = MouseButton::Middle;
    break;
  default:
    return;
  }

  size_t button_index = static_cast<size_t>(maya_button);

  if (action == GLFW_PRESS) {
    s_mouse_buttons_down[button_index] = true;
    s_mouse_buttons_pressed[button_index] = true;
  } else if (action == GLFW_RELEASE) {
    s_mouse_buttons_down[button_index] = false;
    s_mouse_buttons_released[button_index] = true;
  }
}

void glfw_cursor_pos_callback(GLFWwindow * /*window*/, double xpos,
                              double ypos) {
  s_mouse_position.x = static_cast<f32>(xpos);
  s_mouse_position.y = static_cast<f32>(ypos);
}

void glfw_scroll_callback(GLFWwindow * /*window*/, double /*xoffset*/,
                          double yoffset) {
  s_scroll_delta += static_cast<f32>(yoffset);
}

Input &Input::instance() {
  static Input s_instance;
  return s_instance;
}

void Input::init(void *window_handle) {
  if (!window_handle)
    return;

  GLFWwindow *window = static_cast<GLFWwindow *>(window_handle);

  glfwSetKeyCallback(window, glfw_key_callback);
  glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
  glfwSetCursorPosCallback(window, glfw_cursor_pos_callback);
  glfwSetScrollCallback(window, glfw_scroll_callback);
}

void Input::update() {
  // Clear pressed/released states
  std::memset(s_keys_pressed, 0, sizeof(s_keys_pressed));
  std::memset(s_keys_released, 0, sizeof(s_keys_released));
  std::memset(s_mouse_buttons_pressed, 0, sizeof(s_mouse_buttons_pressed));
  std::memset(s_mouse_buttons_released, 0, sizeof(s_mouse_buttons_released));

  // Update mouse delta
  m_mouse_delta.x = s_mouse_position.x - s_mouse_last_position.x;
  m_mouse_delta.y = s_mouse_position.y - s_mouse_last_position.y;
  s_mouse_last_position = s_mouse_position;
  m_mouse_position = s_mouse_position;

  // Update scroll delta
  m_scroll_delta = s_scroll_delta;
  s_scroll_delta = 0.0f;
}

bool Input::is_key_down(Key key) const {
  if (key == Key::Unknown || key >= Key::Count)
    return false;
  return s_keys_down[static_cast<size_t>(key)];
}

bool Input::is_key_pressed(Key key) const {
  if (key == Key::Unknown || key >= Key::Count)
    return false;
  return s_keys_pressed[static_cast<size_t>(key)];
}

bool Input::is_key_released(Key key) const {
  if (key == Key::Unknown || key >= Key::Count)
    return false;
  return s_keys_released[static_cast<size_t>(key)];
}

bool Input::is_mouse_button_down(MouseButton button) const {
  if (button >= MouseButton::Count)
    return false;
  return s_mouse_buttons_down[static_cast<size_t>(button)];
}

bool Input::is_mouse_button_pressed(MouseButton button) const {
  if (button >= MouseButton::Count)
    return false;
  return s_mouse_buttons_pressed[static_cast<size_t>(button)];
}

bool Input::is_mouse_button_released(MouseButton button) const {
  if (button >= MouseButton::Count)
    return false;
  return s_mouse_buttons_released[static_cast<size_t>(button)];
}

} // namespace maya
