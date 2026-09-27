#pragma once

#include "maya/assets/asset_ref.hpp"
#include "maya/math/quaternion.hpp"
#include <string>

namespace maya {
struct NameComponent {
    std::string value;
};

/// Authoritative local TRS. World queries expose const values; edit via set_transform.
struct TransformComponent {
    math::Vec3 translation{0.0f}; // metres
    math::Quat rotation{};
    math::Vec3 scale{1.0f};
};

struct MeshRendererComponent {
    AssetRef<MeshAsset> mesh{};
    AssetRef<MaterialAsset> material{};
    bool visible = true;
};

/// Window-independent data; aspect ratio belongs to the view, not the scene camera.
struct CameraComponent {
    float vertical_fov = math::PI / 3.0f; // radians
    float near_clip = 0.1f; // metres
    float far_clip = 1000.0f;
};

enum class LightKind { directional, point, spot };
struct LightComponent {
    LightKind kind = LightKind::directional;
    math::Vec3 color{1.0f}; // linear RGB
    float intensity = 1.0f; // lux for directional; lumens for point/spot
    float range = 10.0f; // metres, local lights only
    float inner_cone = math::PI / 6.0f; // full angle in radians, spot only
    float outer_cone = math::PI / 4.0f;
    bool enabled = true;
};

// Built-in behaviors, until scripting exists. They are authored like any other component and run
// only in play sessions (the player and editor play), through the shared simulation systems.

/// Turns the entity about a local axis while playing. A zero axis does not turn.
struct SpinComponent {
    math::Vec3 axis{0.0f, 1.0f, 0.0f};
    float speed = math::PI / 4.0f; // radians per second; negative turns the other way
};

/// Gameplay input flies the entity while playing (normally a camera): WASD to move, Q/E down and up,
/// Shift faster, and the mouse to look.
struct FlyControlComponent {
    float speed = 3.0f; // metres per second
    float look_sensitivity = 0.0025f; // radians per point of mouse movement
};
} // namespace maya
