#pragma once

#include "maya/world/world.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

/// One shape, placed in the body entity's local space. `scale` stretches the shape along its own axes
/// before the rotation and offset place it (a collider on a scaled child entity); a sphere needs it
/// uniform and a capsule needs X and Z to match.
struct ColliderDesc {
    ShapeGeometry shape = BoxShape{};
    math::Vec3 offset{0.0f};
    math::Quat rotation{};
    math::Vec3 scale{1.0f};
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
    bool sensor = false; // detects overlaps without a contact response
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

/// What a query hits: colliders in these groups, sensors only when asked, and never `ignore`.
struct QueryFilter {
    uint16_t groups = all_collision_groups; // bit n: hit colliders in group n
    bool sensors = false;
    std::optional<EntityHandle> ignore;
};
/// A query result against the last completed step: one per body, the nearest point.
struct QueryHit {
    EntityHandle entity;
    EntityId id;
    math::Vec3 point{0.0f}; // world space; for an overlap, a point where they touch
    math::Vec3 normal{0.0f}; // the hit surface's outward normal
    float distance = 0.0f; // along the ray or cast; 0 for an overlap, or a cast that starts touching
};

enum class PhysicsEventKind : uint8_t { contact_begin, contact_end, trigger_enter, trigger_exit };
const char* physics_event_name(PhysicsEventKind kind) noexcept;
/// A contact or trigger event from a step, delivered in phase 7 in order of kind, then `first`, then
/// `second` (first < second). A trigger is a pair where either collider is a sensor. Events hold
/// identities, never component pointers.
struct PhysicsEvent {
    PhysicsEventKind kind = PhysicsEventKind::contact_begin;
    uint64_t tick = 0;
    EntityId first, second;
    /// The two entities, when they are still in the World and not being destroyed in this tick's
    /// batch; a recipient without one is skipped (and counted in PhysicsStats::event_recipients_skipped).
    std::optional<EntityHandle> first_entity, second_entity;
    math::Vec3 point{0.0f}; // begin and enter: where they touched
    math::Vec3 normal{0.0f}; // begin and enter: from first toward second
    float speed = 0.0f; // begin and enter: how fast they approached along the normal, m/s
    /// End and exit: because a body was removed (its entity destroyed, its body removed, or the
    /// session stopping), not because the two separated.
    bool removed = false;
};

/// Counts for diagnostics; the step flags count steps where Jolt reported the limit.
struct PhysicsStats {
    size_t bodies = 0, static_bodies = 0, kinematic_bodies = 0, dynamic_bodies = 0;
    size_t active_bodies = 0; // awake kinematic and dynamic bodies
    uint64_t steps = 0;
    uint64_t steps_with_errors = 0;
    uint64_t manifold_cache_full = 0, body_pair_cache_full = 0, contact_constraints_full = 0;
    uint64_t bodies_created = 0, bodies_removed = 0;
    uint64_t events = 0; // contact and trigger events produced
    size_t contacts = 0, overlaps = 0; // body pairs touching now: solid pairs, and pairs with a sensor
    uint64_t queries = 0; // raycasts, shape casts, and overlaps asked
    uint64_t event_recipients_skipped = 0; // recipients already gone, or being destroyed, at delivery
    uint64_t contact_records_dropped = 0; // contact changes Jolt reported that could not be stored (out of memory)
    size_t temp_high_water_bytes = 0; // the most the per-step scratch allocator held at once
    size_t temp_capacity_bytes = 0; // its preallocated size; more than this falls back to malloc
    // Wall time of the last tick's phases, in milliseconds.
    double prepare_ms = 0.0, step_ms = 0.0, synchronize_ms = 0.0, events_ms = 0.0, commit_ms = 0.0;
};

/// Jolt's process-wide allocations, counted by Maya's allocator hooks.
struct PhysicsMemory {
    size_t live_bytes = 0;
    size_t peak_bytes = 0;
    uint64_t allocations = 0;
};
PhysicsMemory physics_memory() noexcept;
/// The Jolt build this process simulates with: version, precision, layer bits, and the options that
/// change results. Play recordings name it; a replay under another configuration is refused.
std::string physics_configuration();
void reset_physics_peak() noexcept;

/// Worker threads in the process's one physics job pool; the calling thread also runs jobs. -1
/// chooses the default (one less than the hardware threads, at most 7). Results are the same for any
/// count. Takes effect for the next step; call it on the thread that steps worlds.
void set_physics_worker_threads(int count);
int physics_worker_threads() noexcept;

/// A body as debug views draw it (docs/physics.md#debug-views).
struct PhysicsDebugBody {
    EntityHandle entity;
    MotionType motion = MotionType::static_body;
    bool sleeping = false; // kinematic and dynamic bodies
    bool sensor = false;
    uint8_t group = 0;
    std::span<const ColliderDesc> colliders; // in the entity's space, before its scale
};
/// A contact point in the last step between two solid bodies (sensors report no points).
struct PhysicsDebugContact {
    math::Vec3 point{0.0f};
    math::Vec3 normal{0.0f}; // from the lower body ID toward the other
    uint16_t groups = 0; // bit n: one of the two bodies is in group n
};
enum class PhysicsQueryKind : uint8_t { raycast, shape_cast, overlap };
/// A query as it was asked, and where it hit.
struct PhysicsDebugQuery {
    PhysicsQueryKind kind = PhysicsQueryKind::raycast;
    math::Vec3 origin{0.0f}; // an overlap's position
    math::Quat rotation{}; // shape casts and overlaps
    math::Vec3 direction{0.0f}; // unit; raycasts and shape casts
    float distance = 0.0f;
    std::optional<ShapeGeometry> shape; // shape casts and overlaps
    std::vector<math::Vec3> hits; // each hit body's nearest point
};

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
    /// The entities these requests teleport, in request order: a teleport resets their pose history.
    std::vector<EntityHandle> teleports() const {
        auto entities = std::vector<EntityHandle>{};
        for (const auto& request : m_requests)
            if (request.kind == Kind::teleport) entities.push_back(std::get<EntityHandle>(request.entity));
        return entities;
    }
    /// Drops the requests made after the first `size`, as if they were never made. Script hosts use
    /// it to discard a failed call's requests.
    void truncate(size_t size) {
        if (size < m_requests.size()) m_requests.erase(m_requests.begin() + std::ptrdiff_t(size), m_requests.end());
    }

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

    /// Debug views (docs/physics.md#debug-views). While capture is on, each step keeps its contact
    /// points, and each query is kept until clear_debug_queries; while it is off, neither costs anything.
    void set_debug_capture(bool on);
    bool debug_capture() const noexcept;
    /// Every body in creation order, with its shape as it was described.
    void for_each_debug_body(const std::function<void(const PhysicsDebugBody&)>& visit) const;
    /// The last step's contact points, sorted by position; empty while capture is off.
    const std::vector<PhysicsDebugContact>& debug_contacts() const noexcept;
    /// The queries asked since clear_debug_queries, in order; empty while capture is off.
    const std::vector<PhysicsDebugQuery>& debug_queries() const noexcept;
    /// Play sessions call it at the start of each tick, so the list holds the last tick's queries.
    void clear_debug_queries() noexcept;

    /// Throws std::invalid_argument naming the entity if `commands` (from index `first`) sets the
    /// transform of, removes the transform of, or reparents an entity with a kinematic or dynamic
    /// body, or puts any body under one.
    void check_world_commands(const World& world, const WorldCommands& commands, size_t first = 0) const;
    /// Phase 4: moves static bodies whose committed transforms changed, then applies the requests of
    /// each list in turn, in the order they were made. Kinematic bodies reach their targets over
    /// `interval`; those without a target stop.
    void prepare(const World& world, std::span<const BodyCommands* const> requests, float interval);
    void prepare(const World& world, const BodyCommands& requests, float interval) {
        const BodyCommands* lists[] = {&requests};
        prepare(world, lists, interval);
    }
    /// Phase 5: one fixed interval.
    void step(float interval);
    /// Phase 6: appends the moved kinematic and dynamic poses to the tick's batch, in creation order.
    void synchronize(const World& world, WorldCommands& commands);
    /// Phase 7: the contacts and triggers that began or ended in the last step, and the ends caused by
    /// bodies removed since, sorted. Recipients destroyed in `commands` are left out of the events.
    std::vector<PhysicsEvent> take_events(const World& world, const WorldCommands& commands, uint64_t tick);
    /// When the session stops: ends every contact and trigger still in progress (`removed` set), sorted.
    std::vector<PhysicsEvent> end_contacts(const World& world, uint64_t tick);

    /// Queries against the last completed step, on the owner thread between steps. Results are one per
    /// body, sorted by distance and then EntityId, and repeat exactly for the same state. They throw
    /// std::invalid_argument for a direction that is zero or not finite, a negative distance, or a
    /// shape a collider could not have.
    std::vector<QueryHit> raycast(math::Vec3 origin, math::Vec3 direction, float distance, const QueryFilter& filter = {}) const;
    /// The nearest hit of the same ray, exactly raycast(...).front(), without collecting the others.
    std::optional<QueryHit> raycast_nearest(math::Vec3 origin, math::Vec3 direction, float distance, const QueryFilter& filter = {}) const;
    /// Sweeps a shape from `origin` along `direction` for `distance`.
    std::vector<QueryHit> shape_cast(const ShapeGeometry& shape, math::Vec3 origin, math::Quat rotation, math::Vec3 direction,
                                     float distance, const QueryFilter& filter = {}) const;
    /// The bodies a shape at `position` touches, sorted by EntityId.
    std::vector<QueryHit> overlap(const ShapeGeometry& shape, math::Vec3 position, math::Quat rotation,
                                  const QueryFilter& filter = {}) const;
    /// Creates every body in order, or none: returns why the first refused one cannot be created,
    /// naming its entity, or empty. Play sessions use it for the bodies authored in a scene.
    std::string create_bodies(const World& world, std::span<const std::pair<EntityHandle, BodyDesc>> bodies);
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
