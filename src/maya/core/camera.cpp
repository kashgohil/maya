#include "maya/core/camera.hpp"
#include "maya/platform/input.hpp"
#include "maya/math/math_utils.hpp"
#include <cmath>
#include <iostream>

namespace maya {

Camera::Camera(float fov, float aspect_ratio, float near_clip, float far_clip)
    : m_position(0.0f, 0.0f, 3.0f),
      m_front(0.0f, 0.0f, -1.0f),
      m_up(0.0f, 1.0f, 0.0f),
      m_world_up(0.0f, 1.0f, 0.0f),
      m_yaw(-90.0f),
      m_pitch(0.0f),
      m_movement_speed(2.5f),
      m_mouse_sensitivity(0.1f),
      m_fov(fov),
      m_aspect_ratio(aspect_ratio),
      m_near_clip(near_clip),
      m_far_clip(far_clip),
      m_first_mouse(true),
      m_last_x(0.0f),
      m_last_y(0.0f)
{
    // Initialize vectors
    m_right = math::Vec3::cross(m_front, m_world_up).normalized();
}

void Camera::update(float delta_time) {
    process_keyboard(delta_time);
    process_mouse();
}

math::Mat4 Camera::get_view_matrix() const {
    return math::Mat4::look_at(m_position, m_position + m_front, m_up);
}

math::Mat4 Camera::get_projection_matrix() const {
    // Convert FOV to radians for the matrix calculation
    return math::Mat4::perspective(math::to_radians(m_fov), m_aspect_ratio, m_near_clip, m_far_clip);
}

math::Mat4 Camera::get_view_projection_matrix() const {
    // Order: Projection * View
    return get_projection_matrix() * get_view_matrix();
}

void Camera::process_keyboard(float delta_time) {
    Input& input = Input::instance();
    float velocity = m_movement_speed * delta_time;

    if (input.is_key_down(KeyCode::W)) {
        m_position += m_front * velocity;
    }
    if (input.is_key_down(KeyCode::S)) {
        m_position -= m_front * velocity;
    }
    if (input.is_key_down(KeyCode::A)) {
        m_position -= m_right * velocity;
    }
    if (input.is_key_down(KeyCode::D)) {
        m_position += m_right * velocity;
    }
    if (input.is_key_down(KeyCode::Space)) {
        m_position += m_world_up * velocity; // Fly up
    }
    // Simple "crouch" or fly down
    if (input.is_key_down(KeyCode::Left)) { // Using Left arrow as temporary down if Shift isn't mapped
         // Or just strictly planar movement if we didn't want flying. 
         // Let's implement full free-cam.
    }
}

void Camera::process_mouse() {
    Input& input = Input::instance();
    math::Vec2 mouse_pos = input.get_mouse_position();

    if (m_first_mouse) {
        m_last_x = mouse_pos.x;
        m_last_y = mouse_pos.y;
        m_first_mouse = false;
        return; // Skip the first frame to prevent jumps
    }

    float x_offset = mouse_pos.x - m_last_x;
    float y_offset = m_last_y - mouse_pos.y; // Reversed: y-coordinates go from bottom to top
    
    m_last_x = mouse_pos.x;
    m_last_y = mouse_pos.y;

    x_offset *= m_mouse_sensitivity;
    y_offset *= m_mouse_sensitivity;

    m_yaw   += x_offset;
    m_pitch += y_offset;

    // Constrain pitch
    if (m_pitch > 89.0f) m_pitch = 89.0f;
    if (m_pitch < -89.0f) m_pitch = -89.0f;

    // Update vectors
    math::Vec3 front;
    front.x = std::cos(math::to_radians(m_yaw)) * std::cos(math::to_radians(m_pitch));
    front.y = std::sin(math::to_radians(m_pitch));
    front.z = std::sin(math::to_radians(m_yaw)) * std::cos(math::to_radians(m_pitch));
    m_front = front.normalized();
    
    m_right = math::Vec3::cross(m_front, m_world_up).normalized();
    m_up    = math::Vec3::cross(m_right, m_front).normalized();
}

} // namespace maya
