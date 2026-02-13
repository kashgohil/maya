#pragma once

#include "maya/math/vector.hpp"
#include "maya/math/matrix.hpp"

namespace maya {

class Camera {
public:
    Camera(float fov, float aspect_ratio, float near_clip, float far_clip);

    void update(float delta_time);

    void set_position(const math::Vec3& position) { m_position = position; }
    const math::Vec3& get_position() const { return m_position; }

    math::Mat4 get_view_matrix() const;
    math::Mat4 get_projection_matrix() const;
    math::Mat4 get_view_projection_matrix() const;

private:
    void process_keyboard(float delta_time);
    void process_mouse();

    // Camera Attributes
    math::Vec3 m_position;
    math::Vec3 m_front;
    math::Vec3 m_up;
    math::Vec3 m_right;
    math::Vec3 m_world_up;

    // Euler Angles
    float m_yaw;
    float m_pitch;

    // Options
    float m_movement_speed;
    float m_mouse_sensitivity;

    // Projection
    float m_fov;
    float m_aspect_ratio;
    float m_near_clip;
    float m_far_clip;

    // Mouse state
    bool m_first_mouse;
    float m_last_x;
    float m_last_y;
};

} // namespace maya
