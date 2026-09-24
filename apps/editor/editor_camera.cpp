#include "editor_camera.hpp"
#include <algorithm>
#include <cmath>

namespace maya::editor {
namespace {
constexpr float pitch_limit = 1.55f; // just under 90 degrees, so the pose stays well defined
}

EditorCamera::EditorCamera(const math::Vec3& position, float yaw, float pitch)
    : position(position), yaw(yaw), pitch(std::clamp(pitch, -pitch_limit, pitch_limit)) {}

EditorCamera EditorCamera::looking_at(const math::Vec3& eye, const math::Vec3& target) {
    const auto direction = (target - eye).normalized();
    return EditorCamera(eye, std::atan2(-direction.x, -direction.z), std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
}

math::Vec3 EditorCamera::forward() const {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

void EditorCamera::update(const NavigationInput& input, float delta_time) {
    if (!std::isfinite(delta_time) || delta_time < 0.0f) return;
    yaw -= input.look.x * look_sensitivity;
    pitch = std::clamp(pitch - input.look.y * look_sensitivity, -pitch_limit, pitch_limit);
    yaw = std::remainder(yaw, 2.0f * math::PI);
    const auto front = forward();
    const auto right = math::Vec3(std::cos(yaw), 0.0f, -std::sin(yaw));
    const auto up = math::Vec3(0.0f, 1.0f, 0.0f);
    auto direction = math::Vec3(0.0f);
    if (input.forward) direction += front;
    if (input.back) direction -= front;
    if (input.right) direction += right;
    if (input.left) direction -= right;
    if (input.up) direction += up;
    if (input.down) direction -= up;
    const auto rate = speed * (input.fast ? 4.0f : 1.0f);
    if (direction.length_squared() > 0.0f) position += direction.normalized() * (rate * delta_time);
    position += front * (input.dolly * rate * 0.25f);
}

math::Mat4 EditorCamera::pose() const {
    const auto back = forward() * -1.0f;
    const auto right = math::Vec3(std::cos(yaw), 0.0f, -std::sin(yaw));
    const auto up = math::Vec3::cross(back, right);
    auto pose = math::Mat4::identity();
    const math::Vec3 columns[] = {right, up, back, position};
    for (int c = 0; c < 4; ++c) {
        pose.at(0, c) = columns[c].x;
        pose.at(1, c) = columns[c].y;
        pose.at(2, c) = columns[c].z;
    }
    return pose;
}

} // namespace maya::editor
