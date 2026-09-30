#pragma once

#include "maya/world/components.hpp"
#include "maya/math/matrix.hpp"

namespace maya::editor {

/// How the pointer moves the camera while navigating: fly turns it in place (and WASD moves it);
/// orbit turns it around the pivot; pan slides it and the pivot sideways; zoom moves it toward the pivot.
enum class NavigationMode { none, fly, orbit, pan, zoom };

/// Viewport navigation input for one frame, already routed away from the UI.
struct NavigationInput {
    NavigationMode mode = NavigationMode::fly;
    bool forward = false, back = false, left = false, right = false, up = false, down = false;
    bool fast = false;
    math::Vec2 look{0.0f, 0.0f}; // pointer movement in points while navigating
    float dolly = 0.0f; // scroll steps over the viewport; positive moves forward
};

/// Editor camera: free flight, and orbit, pan, and zoom around a pivot. It is tool state: it views a World without being part of it.
class EditorCamera {
public:
    /// Radians: yaw 0 looks along -Z; positive yaw turns left, positive pitch looks up.
    EditorCamera(const math::Vec3& position = {0.0f, 0.0f, 0.0f}, float yaw = 0.0f, float pitch = 0.0f);
    static EditorCamera looking_at(const math::Vec3& eye, const math::Vec3& target);

    void update(const NavigationInput& input, float delta_time);
    /// Rigid world pose: camera looks along its local -Z.
    math::Mat4 pose() const;
    math::Vec3 forward() const;

    math::Vec3 position;
    float yaw;
    float pitch;
    CameraComponent camera{};
    float speed = 3.0f; // metres per second; fast movement is four times this
    float look_sensitivity = 0.003f; // radians per point
    /// The point orbit and zoom turn around and move toward, set when that navigation starts. Pan
    /// moves it with the camera.
    math::Vec3 pivot{0.0f, 0.0f, 0.0f};
    float pan_scale = 0.01f; // metres the camera pans per point of pointer movement
    float zoom_sensitivity = 0.005f; // zoom per point: drag right or up to move closer
};

} // namespace maya::editor
