#pragma once

#include "maya/math/types.hpp"
#include "maya/math/vector.hpp"

namespace maya {

enum class Key : u32 {
  Unknown = 0,
  Space,
  Apostrophe,
  Comma,
  Minus,
  Period,
  Slash,

  Num0,
  Num1,
  Num2,
  Num3,
  Num4,
  Num5,
  Num6,
  Num7,
  Num8,
  Num9,

  Semicolon,
  Equal,

  A,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,

  LeftBracket,
  Backslash,
  RightBracket,
  GraveAccent,

  Escape,
  Enter,
  Tab,
  Backspace,
  Insert,
  Delete,
  Right,
  Left,
  Down,
  Up,

  PageUp,
  PageDown,
  Home,
  End,

  CapsLock,
  ScrollLock,
  NumLock,
  PrintScreen,
  Pause,

  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,

  LeftShift,
  LeftControl,
  LeftAlt,
  LeftSuper,
  RightShift,
  RightControl,
  RightAlt,
  RightSuper,

  Count
};

enum class MouseButton : u32 { Left = 0, Right, Middle, Count };

class Input {
public:
  static Input &instance();

  void init(void *window_handle);
  void update();

  bool is_key_down(Key key) const;
  bool is_key_pressed(Key key) const;
  bool is_key_released(Key key) const;

  bool is_mouse_button_down(MouseButton button) const;
  bool is_mouse_button_pressed(MouseButton button) const;
  bool is_mouse_button_released(MouseButton button) const;

  Vec2 get_mouse_position() const { return m_mouse_position; }
  Vec2 get_mouse_delta() const { return m_mouse_delta; }
  f32 get_scroll_delta() const { return m_scroll_delta; }

private:
  Input() = default;
  ~Input() = default;

  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;

  Vec2 m_mouse_position{0.0f, 0.0f};
  Vec2 m_mouse_delta{0.0f, 0.0f};
  f32 m_scroll_delta = 0.0f;
};

} // namespace maya
