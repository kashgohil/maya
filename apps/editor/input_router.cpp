#include "input_router.hpp"
#include <type_traits>

namespace maya::editor {

std::optional<bool> InputRouter::cancel() {
    if (!m_navigating) return std::nullopt;
    auto output = RoutedInput{};
    end_navigation(output);
    return output.capture;
}

void InputRouter::end_navigation(RoutedInput& output) {
    m_navigating = false;
    m_forward = m_back = m_left = m_right = m_up = m_down = m_fast = false;
    output.capture = false;
    // The UI saw no pointer motion while the cursor was captured; give it the current position.
    output.ui.push_back(MouseMoveEvent{m_pointer.x, m_pointer.y});
}

NavigationInput InputRouter::held() const {
    auto input = NavigationInput{};
    input.forward = m_forward;
    input.back = m_back;
    input.left = m_left;
    input.right = m_right;
    input.up = m_up;
    input.down = m_down;
    input.fast = m_fast;
    return input;
}

RoutedInput InputRouter::route(const std::vector<InputEvent>& events, const RouterContext& context) {
    auto output = RoutedInput{};
    auto look = math::Vec2{0.0f, 0.0f};
    auto dolly = 0.0f;
    for (const auto& event : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, MouseMoveEvent>) {
                if (m_navigating) look += math::Vec2{e.x - m_pointer.x, e.y - m_pointer.y};
                else output.ui.push_back(e);
                m_pointer = {e.x, e.y};
            } else if constexpr (std::is_same_v<T, MouseButtonEvent>) {
                if (m_navigating) {
                    if (e.button == MouseButton::right && !e.down) end_navigation(output);
                } else if (e.button == MouseButton::right && e.down && context.viewport_hovered) {
                    m_navigating = true;
                    output.capture = true;
                    output.navigation_started = true;
                } else {
                    output.ui.push_back(e);
                }
            } else if constexpr (std::is_same_v<T, KeyEvent>) {
                if (!m_navigating) {
                    output.ui.push_back(e);
                    return;
                }
                switch (e.key) {
                case KeyCode::W: m_forward = e.down; break;
                case KeyCode::S: m_back = e.down; break;
                case KeyCode::A: m_left = e.down; break;
                case KeyCode::D: m_right = e.down; break;
                case KeyCode::E: m_up = e.down; break;
                case KeyCode::Q: m_down = e.down; break;
                case KeyCode::LeftShift: case KeyCode::RightShift: m_fast = e.down; break;
                case KeyCode::Escape: if (e.down) end_navigation(output); break;
                default: break; // other keys are neither UI shortcuts nor text while navigating
                }
            } else if constexpr (std::is_same_v<T, TextEvent>) {
                if (!m_navigating) output.ui.push_back(e);
            } else if constexpr (std::is_same_v<T, ScrollEvent>) {
                if (m_navigating || context.viewport_hovered) dolly += e.y;
                else output.ui.push_back(e);
            } else if constexpr (std::is_same_v<T, FocusEvent>) {
                if (!e.focused && m_navigating) end_navigation(output);
                output.ui.push_back(e);
            }
        }, event);
    }
    output.navigation = m_navigating ? held() : NavigationInput{};
    output.navigation.look = look;
    output.navigation.dolly = dolly;
    return output;
}

} // namespace maya::editor
