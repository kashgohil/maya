#pragma once

#include "maya/physics/physics.hpp"
#include "maya/world/debug_draw.hpp"
#include "maya/world/presentation.hpp"

// Physics debug views (docs/physics.md#debug-views): what the editor, and the player when asked, draw
// over a view. Play views draw from the play session's physics world; authoring views, which have no
// physics world, draw from the collider components.
namespace maya {

enum class PhysicsDebugCategory : uint8_t {
    colliders = 1 << 0, // solid collider outlines
    contacts = 1 << 1, // contact points and normals (play only)
    body_state = 1 << 2, // outlines colored by motion type, and dynamic bodies by active or sleeping
    triggers = 1 << 3, // sensor outlines
    queries = 1 << 4, // raycasts, shape casts, and overlaps, and where they hit (play only)
};
inline constexpr uint8_t all_physics_debug = 0x1F;

struct PhysicsDebugOptions {
    uint8_t categories = 0; // PhysicsDebugCategory bits; none draws nothing
    uint16_t groups = all_collision_groups; // bit n: draw bodies (and contacts) in collision group n
    bool has(PhysicsDebugCategory category) const noexcept { return (categories & uint8_t(category)) != 0; }
    bool any() const noexcept { return categories != 0 && groups != 0; }
    /// Contacts and queries need the physics world to capture them (PhysicsWorld::set_debug_capture).
    bool needs_capture() const noexcept {
        return groups != 0 && (has(PhysicsDebugCategory::contacts) || has(PhysicsDebugCategory::queries));
    }
    bool operator==(const PhysicsDebugOptions&) const = default;
};

namespace physics_debug_color {
inline constexpr DebugColor collider{0.40f, 0.85f, 0.95f, 0.9f};
inline constexpr DebugColor trigger{1.00f, 0.78f, 0.30f, 0.9f};
inline constexpr DebugColor static_body{0.58f, 0.64f, 0.76f, 0.9f};
inline constexpr DebugColor kinematic{0.70f, 0.56f, 1.00f, 0.9f};
inline constexpr DebugColor kinematic_sleeping{0.70f, 0.56f, 1.00f, 0.5f};
inline constexpr DebugColor active{0.36f, 0.90f, 0.47f, 0.9f};
inline constexpr DebugColor sleeping{0.36f, 0.42f, 0.52f, 0.8f}; // dynamic bodies
inline constexpr DebugColor contact{1.00f, 0.36f, 0.32f, 1.0f};
inline constexpr DebugColor normal{1.00f, 0.60f, 0.36f, 1.0f};
inline constexpr DebugColor query{0.52f, 0.62f, 1.00f, 0.9f};
inline constexpr DebugColor query_hit{1.00f, 0.42f, 0.80f, 1.0f};
} // namespace physics_debug_color

/// Outlines one collider at `body_world` (its body entity's world matrix, scale included).
void debug_collider(DebugDraw& out, const math::Affine& body_world, const ColliderDesc& collider, const DebugColor& color);

/// An authoring World: each collider component at its entity's world pose. Body state is the authored
/// motion (a rigid body on the entity or above it, else static); contacts and queries have none.
void authored_physics_debug(const World& world, const PhysicsDebugOptions& options, DebugDraw& out);

/// A play World: every body of `physics` at its entity's shown pose (`poses`, else the World's), and
/// the contacts and queries it captured.
void play_physics_debug(const World& world, const PhysicsWorld& physics, const PhysicsDebugOptions& options,
                        const PresentationPoses* poses, DebugDraw& out);

} // namespace maya
