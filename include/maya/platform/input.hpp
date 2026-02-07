#pragma once

#include <unordered_map>
#include "maya/math/vector.hpp"

namespace maya {

enum class KeyCode {
    Unknown = 0,
    Space = 32,
    Escape = 256,
    Enter = 257,
    Right = 262,
    Left = 263,
    Down = 264,
    Up = 265,
    A = 65,
    D = 68,
    S = 83,
    W = 87
};

class Input {
public:
    static Input& instance() {
        static Input s_instance;
        return s_instance;
    }

    // Key states
    bool is_key_down(KeyCode key) const {
        auto it = m_keys.find(key);
        return it != m_keys.end() && it->second;
    }

    bool is_key_pressed(KeyCode key) const {
        auto it = m_keys.find(key);
        auto it_prev = m_prev_keys.find(key);
        bool currently_down = (it != m_keys.end() && it->second);
        bool previously_down = (it_prev != m_prev_keys.end() && it_prev->second);
        return currently_down && !previously_down;
    }

    // Internal setters (called by Window callbacks)
    void set_key_state(KeyCode key, bool down) {
        m_keys[key] = down;
    }

    void set_mouse_position(float x, float y) {
        m_mouse_pos = {x, y};
    }

    const math::Vec2& get_mouse_position() const { return m_mouse_pos; }

    // Lifecycle
    void update() {
        m_prev_keys = m_keys;
    }

private:
    Input() = default;
    
    std::unordered_map<KeyCode, bool> m_keys;
    std::unordered_map<KeyCode, bool> m_prev_keys;
    math::Vec2 m_mouse_pos;
};

} // namespace maya
