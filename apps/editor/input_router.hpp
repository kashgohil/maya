#pragma once

#include "editor_camera.hpp"
#include "maya/platform/input.hpp"
#include <optional>
#include <vector>

namespace maya::editor {

/// What the UI reported about the pointer at the end of the previous frame.
struct RouterContext {
    bool viewport_hovered = false; // the viewport image is under the pointer and not covered
    bool game_view = false; // the scene is playing and the viewport shows its camera
};

struct RoutedInput {
    std::vector<InputEvent> ui; // events for the UI, in order
    NavigationInput navigation; // camera input for this frame
    std::vector<InputEvent> game; // events for the playing scene, in order
    std::optional<bool> capture; // cursor capture to request from the host, when it changes
    bool navigation_started = false; // the UI should drop keyboard focus (e.g. an active text field)
    bool game_started = false; // the game took the input; the UI should drop keyboard focus too
    bool game_ended = false; // the game gave the input back; release what it held
};

/// Sends each window event to exactly one owner. Normally everything goes to the UI, so typing
/// into a field never reaches the camera. Holding the right mouse button over the viewport starts
/// navigation: the cursor is captured and movement keys, pointer motion, and scrolling drive the
/// camera until the button is released, Escape is pressed, or the window loses focus. Scrolling
/// over the viewport dollies the camera without starting navigation.
///
/// While the scene plays in the game view, a left click on the viewport gives the game the input:
/// the cursor is captured and every event goes to the game until Escape is pressed or the window
/// loses focus. The editor camera is not flown or dollied over the game view.
class InputRouter {
public:
    RoutedInput route(const std::vector<InputEvent>& events, const RouterContext& context);
    bool navigating() const noexcept { return m_navigating; }
    bool game_has_input() const noexcept { return m_game; }
    /// Ends navigation or the game's input (e.g. when the viewport disappears or play stops);
    /// returns the release request.
    std::optional<bool> cancel();

private:
    void end_navigation(RoutedInput& output);
    void end_game(RoutedInput& output);
    NavigationInput held() const;

    bool m_navigating = false;
    bool m_game = false;
    bool m_forward = false, m_back = false, m_left = false, m_right = false, m_up = false, m_down = false;
    bool m_fast = false;
    math::Vec2 m_pointer{0.0f, 0.0f};
};

} // namespace maya::editor
