#pragma once

#include "maya/assets/asset_ref.hpp"
#include "maya/math/quaternion.hpp"
#include <string>
#include <variant>
#include <vector>

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
/// How a camera maps scene light to the display (docs/renderer.md#exposure-and-tone-mapping).
enum class ToneMapping : uint8_t { agx, pbr_neutral };
struct CameraComponent {
    float vertical_fov = math::PI / 3.0f; // radians
    float near_clip = 0.1f; // metres
    float far_clip = 1000.0f;
    float exposure = 0.0f; // EV100; scene values are scaled by 1 / (1.2 × 2^EV100) before tone mapping
    ToneMapping tone_mapping = ToneMapping::agx;
};

enum class LightKind { directional, point, spot };
struct LightComponent {
    LightKind kind = LightKind::directional;
    math::Vec3 color{1.0f}; // linear RGB
    // Lux for directional lights; candela for point and spot lights, as glTF's KHR_lights_punctual (since
    // version 2, #1034; version 1 stored lumens, 4 pi x candela). A white diffuse surface facing a
    // directional light reflects intensity / pi, so the default shows it at scene light 1
    // (docs/renderer.md#lights).
    float intensity = math::PI;
    float range = 10.0f; // metres, local lights only: their light fades to nothing there
    float inner_cone = math::PI / 6.0f; // full angle in radians, spot only
    float outer_cone = math::PI / 4.0f;
    bool enabled = true;
    // Shadows (since version 2, #1034; docs/renderer.md#shadows). Point lights cast none yet.
    bool cast_shadows = true;
    float shadow_bias = 1.0f; // shadow-map texels: the receiver's depth moves toward the light
    float shadow_normal_bias = 1.0f; // shadow-map texels: the receiver moves along its normal
    float shadow_distance = 60.0f; // metres from the camera that a directional light's cascades cover
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

// Physics (docs/physics.md). Authored data only: play sessions turn these into physics bodies.

enum class ColliderShape { box, sphere, capsule };
/// A collision shape in the entity's local space. Without a rigid body on the entity or an ancestor,
/// it is a static collider; otherwise it is part of that body's shape.
struct ColliderComponent {
    ColliderShape shape = ColliderShape::box;
    math::Vec3 half_extents{0.5f}; // box, metres
    float radius = 0.5f; // sphere and capsule
    float half_height = 0.5f; // capsule: half the straight section along local Y, excluding the caps
    math::Vec3 offset{0.0f};
    math::Quat rotation{};
    float friction = 0.5f;
    float restitution = 0.0f; // 0 to 1
    bool sensor = false; // reports overlaps (#1021) without a contact response
    int32_t group = 0; // collision group 0 to 15, named in the project
    uint32_t mask = 0xFFFF; // bit n set: collides with group n
};

enum class BodyMotion { dynamic, kinematic };
/// Makes the entity a moving body. Its shape is its own collider and the colliders below it.
struct RigidBodyComponent {
    BodyMotion motion = BodyMotion::dynamic;
    float mass = 0.0f; // kg; 0 derives it from the density
    float density = 1000.0f; // kg/m³
    float linear_damping = 0.05f; // 1/s
    float angular_damping = 0.05f;
    float gravity_factor = 1.0f;
    math::Vec3 linear_velocity{0.0f}; // initial, m/s; dynamic bodies only
    math::Vec3 angular_velocity{0.0f}; // initial, rad/s
};

/// The scene's physics settings; at most one per scene. Without one, the defaults apply.
struct PhysicsSettingsComponent {
    math::Vec3 gravity{0.0f, -9.81f, 0.0f}; // m/s²
};

/// The scene's environment for image-based lighting (docs/renderer.md#environments); at most one is
/// used. Without one, surfaces are lit by the uniform ambient light the view supplies.
struct EnvironmentComponent {
    AssetRef<EnvironmentAsset> environment{};
    float intensity = 1.0f; // multiplies the environment's radiance
    float rotation = 0.0f; // radians about +Y, counter-clockwise seen from above
    bool background = true; // drawn where no surface is, in place of the view's clear color
};

// Scripting (docs/scripting.md).

enum class ScriptValueType { number, integer, boolean, string, vector, color, entity };
/// number: float; integer: int32_t; boolean: bool; string; vector and color: Vec3; entity: EntityId
/// (zero for none).
using ScriptValueData = std::variant<float, int32_t, bool, std::string, math::Vec3, EntityId>;
/// One value for a property a script declares, stored by the property's name.
struct ScriptValue {
    std::string name;
    ScriptValueType type = ScriptValueType::number;
    ScriptValueData data = 0.0f;
    bool operator==(const ScriptValue& other) const;
};
/// Attaches a script asset to the entity, with values for the properties the script declares.
/// Properties without a value use the script's defaults.
struct ScriptComponent {
    AssetRef<ScriptAsset> script{};
    std::vector<ScriptValue> values;
};
} // namespace maya
