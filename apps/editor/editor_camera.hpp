#pragma once

#include "maya/world/components.hpp"
#include "maya/math/matrix.hpp"

namespace maya::editor {

/// Viewport navigation input for one frame, already routed away from the UI.
struct NavigationInput {
    bool forward = false, back = false, left = false, right = false, up = false, down = false;
    bool fast = false;
    math::Vec2 look{0.0f, 0.0f}; // pointer movement in points while navigating
    float dolly = 0.0f; // scroll steps over the viewport; positive moves forward
};

/// Free-flight editor camera. It is tool state: it views a World without being part of it.
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
};

} // namespace maya::editor
