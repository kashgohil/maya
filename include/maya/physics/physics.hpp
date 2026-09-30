#pragma once

#include "maya/world/world.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Rigid-body physics for play sessions, on Jolt Physics behind a Maya-owned interface. Nothing here
// exposes Jolt; see docs/physics.md and docs/architecture/runtime-world-contracts.md#physics-boundary.
namespace maya {

/// Who writes a body's pose (the transform-authority table of the scheduling contract).
enum class MotionType : uint8_t {
    static_body, // world data: moved by ordinary transform writes, applied before the next step
    kinematic, // gameplay supplies a target pose each tick; physics moves the body to it
    dynamic, // physics; gameplay pushes it with forces, impulses, velocities, or a teleport
};
const char* motion_type_name(MotionType type) noexcept;

struct BoxShape {
    math::Vec3 half_extents{0.5f}; // metres
};
struct SphereShape {
    float radius = 0.5f;
};
/// A cylinder along local Y with hemispherical caps. Scaling stretches the cylinder (Y) and the
/// radius (X and Z, which must match); the caps stay round.
struct CapsuleShape {
    float radius = 0.5f;
    float half_height = 0.5f; // half the length of the straight section, excluding the caps
};
using ShapeGeometry = std::variant<BoxShape, SphereShape, CapsuleShape>;

/// One shape, placed in the body entity's local space.
struct ColliderDesc {
    ShapeGeometry shape = BoxShape{};
    math::Vec3 offset{0.0f};
    math::Quat rotation{};
};

inline constexpr uint32_t collision_group_count = 16;
inline constexpr uint16_t all_collision_groups = 0xFFFF;

/// A body for one entity. Two bodies collide only when each one's group is in the other's mask.
struct BodyDesc {
    MotionType motion = MotionType::dynamic;
    std::vector<ColliderDesc> colliders{ColliderDesc{}}; // one or more; several form a compound shape
    float mass = 0.0f; // kg; 0 derives it from the density
    float density = 1000.0f; // kg/m³
    float friction = 0.5f;
    float restitution = 0.0f; // 0 to 1
    float linear_damping = 0.05f; // 1/s
    float angular_damping = 0.05f;
    float gravity_factor = 1.0f;
    math::Vec3 linear_velocity{0.0f}; // initial, m/s (dynamic bodies)
    math::Vec3 angular_velocity{0.0f}; // initial, rad/s
    uint8_t group = 0; // 0 to 15
    uint16_t mask = all_collision_groups; // bit n: collides with group n
};
/// Why a description cannot make a body, or empty. Structural rules (root entity, scale) are
/// checked when the body is created.
std::string validate_body(const BodyDesc& body);

struct PhysicsSettings {
    math::Vec3 gravity{0.0f, -9.81f, 0.0f}; // m/s²
    uint32_t max_bodies = 16384;
    uint32_t max_body_pairs = 65536;
    uint32_t max_contact_constraints = 32768;
    uint32_t collision_steps = 1; // collision iterations per fixed interval
    size_t temp_allocator_bytes = size_t{4} << 20; // per-step scratch; larger steps fall back to malloc
};

/// A body's state after the last completed step.
struct BodyState {
    MotionType motion = MotionType::dynamic;
    math::Vec3 position{0.0f}; // of the entity, in world space
    math::Quat rotation{};
    math::Vec3 linear_velocity{0.0f};
    math::Vec3 angular_velocity{0.0f};
    float mass = 0.0f; // 0 for static and kinematic bodies
    bool sleeping = false;
};

/// Counts for diagnostics; the step flags count steps where Jolt reported the limit.
struct PhysicsStats {
    size_t bodies = 0, static_bodies = 0, kinematic_bodies = 0, dynamic_bodies = 0;
    size_t active_bodies = 0; // awake kinematic and dynamic bodies
    uint64_t steps = 0;
    uint64_t steps_with_errors = 0;
    uint64_t manifold_cache_full = 0, body_pair_cache_full = 0, contact_constraints_full = 0;
    uint64_t bodies_created = 0, bodies_removed = 0;
    // Wall time of the last tick's phases, in milliseconds.
    double prepare_ms = 0.0, step_ms = 0.0, synchronize_ms = 0.0, commit_ms = 0.0;
};

/// Jolt's process-wide allocations, counted by Maya's allocator hooks.
struct PhysicsMemory {
    size_t live_bytes = 0;
    size_t peak_bytes = 0;
    uint64_t allocations = 0;
};
PhysicsMemory physics_memory() noexcept;
void reset_physics_peak() noexcept;

/// Worker threads in the process's one physics job pool; the calling thread also runs jobs. -1
/// chooses the default (one less than the hardware threads, at most 7). Results are the same for any
/// count. Takes effect for the next step; call it on the thread that steps worlds.
void set_physics_worker_threads(int count);
int physics_worker_threads() noexcept;

class PhysicsWorld;

/// Body requests made during one fixed tick, in order. Each is checked when it is made, against the
/// World and physics state the tick started from, and throws std::invalid_argument with the reason
/// if it breaks the motion-authority rules. Forces, velocities, targets, and teleports apply before
/// this tick's step; created and removed bodies take effect when the tick's World batch commits.
class BodyCommands {
public:
    BodyCommands(const PhysicsWorld& physics, const World& world);

    /// A body for an entity, which may be created in this tick's batch. Kinematic and dynamic bodies
    /// must be root entities with unit scale; a static body's world scale is baked into its shapes.
    void create(EntityTarget entity, BodyDesc body);
    void remove(EntityHandle entity);
    void add_force(EntityHandle entity, math::Vec3 force); // N at the centre of mass, this step only
    void add_torque(EntityHandle entity, math::Vec3 torque); // N·m, this step only
    void add_impulse(EntityHandle entity, math::Vec3 impulse); // N·s at the centre of mass
    void add_angular_impulse(EntityHandle entity, math::Vec3 impulse);
    void set_linear_velocity(EntityHandle entity, math::Vec3 velocity);
    void set_angular_velocity(EntityHandle entity, math::Vec3 velocity);
    /// Where a kinematic body should be at the end of this step. Without a target, it stops.
    void set_kinematic_target(EntityHandle entity, math::Vec3 position, math::Quat rotation);
    /// Moves a kinematic or dynamic body directly; velocities are kept.
    void teleport(EntityHandle entity, math::Vec3 position, math::Quat rotation);
    void wake(EntityHandle entity);

    /// Names the system making the following requests, for errors reported after the step.
    void set_source(std::string_view name) { m_source = name; }
    size_t size() const noexcept { return m_requests.size(); }

private:
    friend class PhysicsWorld;
    enum class Kind {
        create, remove, force, torque, impulse, angular_impulse, linear_velocity, angular_velocity,
        kinematic_target, teleport, wake
    };
    struct Request {
        Kind kind;
        EntityTarget entity;
        math::Vec3 vector{0.0f};
        math::Quat rotation{};
        std::unique_ptr<BodyDesc> body; // create only
        std::string source; // create only
    };
    EntityHandle require_body(EntityHandle entity, const char* action, bool kinematic, bool dynamic) const;
    void push(Kind kind, EntityHandle entity, math::Vec3 vector, math::Quat rotation = {});

    const PhysicsWorld& m_physics;
    const World& m_world;
    std::string m_source;
    std::vector<Request> m_requests;
};

/// The physics of one play session. Single owner thread. Systems get a const reference (state and
/// counts); the play session drives the fixed-tick phases through the non-const members.
class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsSettings settings = {}); // throws std::invalid_argument for bad settings
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    const PhysicsSettings& settings() const noexcept;
    bool has_body(EntityHandle entity) const noexcept;
    std::optional<MotionType> motion_type(EntityHandle entity) const noexcept;
    std::optional<BodyState> state(EntityHandle entity) const;
    PhysicsStats stats() const;

    /// Throws std::invalid_argument naming the entity if `commands` (from index `first`) sets the
    /// transform of, removes the transform of, or reparents an entity with a kinematic or dynamic
    /// body, or puts any body under one.
    void check_world_commands(const World& world, const WorldCommands& commands, size_t first = 0) const;
    /// Phase 4: moves static bodies whose committed transforms changed, then applies the requests.
    /// Kinematic bodies reach their targets over `interval`; those without a target stop.
    void prepare(const World& world, const BodyCommands& requests, float interval);
    /// Phase 5: one fixed interval.
    void step(float interval);
    /// Phase 6: appends the moved kinematic and dynamic poses to the tick's batch, in creation order.
    void synchronize(const World& world, WorldCommands& commands);
    /// After the batch commits: removes the bodies of destroyed entities and applies created and
    /// removed bodies in request order. Throws std::runtime_error naming the source and the entity if
    /// a body cannot be created; bodies created before it stay.
    void commit(const World& world, const BodyCommands& requests, const WorldCommitResult& result);

private:
    friend class BodyCommands;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace maya
