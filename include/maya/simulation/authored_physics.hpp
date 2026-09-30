#pragma once

#include "maya/physics/physics.hpp"
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace maya {

/// The physics a scene's components author: its bodies in document order, and its settings.
struct AuthoredPhysics {
    std::vector<std::pair<EntityHandle, BodyDesc>> bodies;
    PhysicsSettings settings;
    std::string error; // why the scene's physics cannot be built, naming the entity; empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};

/// Turns maya.collider, maya.rigid_body, and maya.physics_settings components into body descriptions
/// (docs/physics.md#authored-bodies). `order` lists the World's entities in document order.
///  - An entity with a rigid body is a kinematic or dynamic body. Its shape is its own collider plus
///    the colliders on entities below it that have no rigid body, placed relative to it.
///  - An entity with a collider and no rigid body on it or an ancestor is a static body.
///  - A body takes its friction and restitution from its first collider; all its colliders must share
///    one collision group, mask, and sensor setting.
///  - At most one entity may have physics settings; they replace the defaults in `base`.
/// Rules that depend on where a body sits (roots, unit scale, scale per shape) are checked when the
/// bodies are created (PhysicsWorld::create_bodies).
AuthoredPhysics authored_physics(const World& world, std::span<const EntityId> order, PhysicsSettings base = {});

} // namespace maya
