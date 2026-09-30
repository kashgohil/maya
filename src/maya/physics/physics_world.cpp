#include "maya/physics/physics.hpp"
#include "jolt_runtime.hpp"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace maya {
namespace {

/// Adds the wall time of its scope to a millisecond counter.
class PhaseTimer {
public:
    explicit PhaseTimer(double& milliseconds) : m_milliseconds(milliseconds), m_start(std::chrono::steady_clock::now()) {}
    ~PhaseTimer() {
        m_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
    }
    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;

private:
    double& m_milliseconds;
    std::chrono::steady_clock::time_point m_start;
};

std::string entity_text(const World& world, EntityHandle entity) {
    auto text = std::ostringstream{};
    text << "entity ";
    if (const auto id = world.persistent_id(entity)) text << std::hex << id->high << ' ' << id->low;
    else text << "(slot " << entity.slot << ')';
    return text.str();
}

bool finite(const math::Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const math::Quat& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

std::optional<math::Quat> normalized(math::Quat value) noexcept {
    if (!finite(value)) return std::nullopt;
    const auto length = std::sqrt(double(value.x) * value.x + double(value.y) * value.y + double(value.z) * value.z +
                                  double(value.w) * value.w);
    if (!(length > 1e-12)) return std::nullopt;
    return math::Quat(float(value.x / length), float(value.y / length), float(value.z / length), float(value.w / length));
}

bool near_identity(const math::Quat& value) noexcept {
    return std::abs(std::abs(value.w) - 1.0f) <= spatial_tolerance;
}

bool nearly_equal(float a, float b) noexcept {
    return std::abs(a - b) <= spatial_tolerance * std::max({1.0f, std::abs(a), std::abs(b)});
}

JPH::Vec3 jolt(const math::Vec3& value) noexcept { return {value.x, value.y, value.z}; }
JPH::Quat jolt(const math::Quat& value) noexcept { return {value.x, value.y, value.z, value.w}; }
math::Vec3 maya_vector(JPH::Vec3Arg value) noexcept { return {value.GetX(), value.GetY(), value.GetZ()}; }
math::Quat maya_quat(JPH::QuatArg value) noexcept { return {value.GetX(), value.GetY(), value.GetZ(), value.GetW()}; }

// Object layers: bits 0-3 the collision group, bit 4 set for kinematic and dynamic bodies, and
// bits 16-31 the mask. Static and moving bodies go to separate broad-phase trees.
constexpr uint32_t moving_bit = 1u << 4;

JPH::ObjectLayer object_layer(const BodyDesc& body) noexcept {
    return JPH::ObjectLayer(uint32_t(body.group) | (body.motion == MotionType::static_body ? 0u : moving_bit) |
                            (uint32_t(body.mask) << 16));
}
uint32_t group_of(JPH::ObjectLayer layer) noexcept { return uint32_t(layer) & 0xFu; }
uint32_t mask_of(JPH::ObjectLayer layer) noexcept { return uint32_t(layer) >> 16; }
bool moving(JPH::ObjectLayer layer) noexcept { return (uint32_t(layer) & moving_bit) != 0; }

const auto static_tree = JPH::BroadPhaseLayer(0);
const auto moving_tree = JPH::BroadPhaseLayer(1);

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return moving(layer) ? moving_tree : static_tree;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == moving_tree ? "moving" : "static";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer tree) const override {
        return moving(layer) || tree == moving_tree; // static bodies never test other static bodies
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return (moving(a) || moving(b)) && (mask_of(a) >> group_of(b) & 1u) != 0 && (mask_of(b) >> group_of(a) & 1u) != 0;
    }
};

JPH::EMotionType jolt_motion(MotionType type) noexcept {
    switch (type) {
    case MotionType::static_body: return JPH::EMotionType::Static;
    case MotionType::kinematic: return JPH::EMotionType::Kinematic;
    case MotionType::dynamic: break;
    }
    return JPH::EMotionType::Dynamic;
}

std::string validate_collider(const ColliderDesc& collider) {
    if (!finite(collider.offset)) return "a collider offset is not finite";
    if (!normalized(collider.rotation)) return "a collider rotation is not a finite, nonzero quaternion";
    return std::visit([](const auto& shape) -> std::string {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, BoxShape>) {
            if (!finite(shape.half_extents) || !(shape.half_extents.x > 0.0f && shape.half_extents.y > 0.0f && shape.half_extents.z > 0.0f))
                return "a box needs finite, positive half extents";
        } else if constexpr (std::is_same_v<T, SphereShape>) {
            if (!(std::isfinite(shape.radius) && shape.radius > 0.0f)) return "a sphere needs a finite, positive radius";
        } else {
            if (!(std::isfinite(shape.radius) && shape.radius > 0.0f && std::isfinite(shape.half_height) && shape.half_height > 0.0f))
                return "a capsule needs a finite, positive radius and half height";
        }
        return {};
    }, collider.shape);
}

/// Checks the scale rules of the physics contract for a shape scaled by `scale` (the entity's world
/// scale for a static body, 1 for a moving one).
std::string check_scale(const std::vector<ColliderDesc>& colliders, const math::Vec3& scale) {
    const auto uniform = nearly_equal(scale.x, scale.y) && nearly_equal(scale.y, scale.z);
    for (const auto& collider : colliders) {
        if (!uniform && !near_identity(*normalized(collider.rotation)))
            return "a rotated collider cannot take its entity's nonuniform scale";
        if (std::holds_alternative<SphereShape>(collider.shape) && !uniform)
            return "a sphere needs uniform scale";
        if (std::holds_alternative<CapsuleShape>(collider.shape) && !nearly_equal(scale.x, scale.z))
            return "a capsule needs the same scale on X and Z";
    }
    return {};
}

JPH::Ref<JPH::Shape> make_shape(const std::vector<ColliderDesc>& colliders, const math::Vec3& scale, float density) {
    const auto convex = [&](const ColliderDesc& collider) -> JPH::Ref<JPH::ConvexShape> {
        auto shape = std::visit([&](const auto& value) -> JPH::Ref<JPH::ConvexShape> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, BoxShape>) {
                const auto half = JPH::Vec3(value.half_extents.x * scale.x, value.half_extents.y * scale.y, value.half_extents.z * scale.z);
                return new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, 0.25f * half.ReduceMin()));
            } else if constexpr (std::is_same_v<T, SphereShape>) {
                return new JPH::SphereShape(value.radius * scale.x);
            } else {
                return new JPH::CapsuleShape(value.half_height * scale.y, value.radius * scale.x);
            }
        }, collider.shape);
        shape->SetDensity(density);
        return shape;
    };
    const auto offset = [&](const ColliderDesc& collider) {
        return JPH::Vec3(collider.offset.x * scale.x, collider.offset.y * scale.y, collider.offset.z * scale.z);
    };
    if (colliders.size() == 1) {
        const auto& collider = colliders.front();
        auto shape = JPH::Ref<JPH::Shape>(convex(collider).GetPtr());
        const auto rotation = *normalized(collider.rotation);
        if (collider.offset.length_squared() == 0.0f && near_identity(rotation)) return shape;
        return new JPH::RotatedTranslatedShape(offset(collider), jolt(rotation), shape);
    }
    auto compound = JPH::StaticCompoundShapeSettings();
    for (const auto& collider : colliders)
        compound.AddShape(offset(collider), jolt(*normalized(collider.rotation)), convex(collider).GetPtr());
    auto result = compound.Create();
    if (result.HasError()) throw std::runtime_error("the compound shape cannot be built: " + std::string(result.GetError().c_str()));
    return result.Get();
}

} // namespace

const char* motion_type_name(MotionType type) noexcept {
    switch (type) {
    case MotionType::static_body: return "static";
    case MotionType::kinematic: return "kinematic";
    case MotionType::dynamic: break;
    }
    return "dynamic";
}

std::string validate_body(const BodyDesc& body) {
    if (body.motion != MotionType::static_body && body.motion != MotionType::kinematic && body.motion != MotionType::dynamic)
        return "the motion type is unknown";
    if (body.colliders.empty()) return "a body needs at least one collider";
    for (const auto& collider : body.colliders)
        if (auto reason = validate_collider(collider); !reason.empty()) return reason;
    if (!(std::isfinite(body.mass) && body.mass >= 0.0f)) return "the mass must be finite and not negative";
    if (!(std::isfinite(body.density) && body.density > 0.0f)) return "the density must be finite and positive";
    if (!(std::isfinite(body.friction) && body.friction >= 0.0f)) return "the friction must be finite and not negative";
    if (!(std::isfinite(body.restitution) && body.restitution >= 0.0f && body.restitution <= 1.0f))
        return "the restitution must be between 0 and 1";
    if (!(std::isfinite(body.linear_damping) && body.linear_damping >= 0.0f && std::isfinite(body.angular_damping) &&
          body.angular_damping >= 0.0f))
        return "the damping must be finite and not negative";
    if (!std::isfinite(body.gravity_factor)) return "the gravity factor must be finite";
    if (!finite(body.linear_velocity) || !finite(body.angular_velocity)) return "the velocities must be finite";
    if (body.motion != MotionType::dynamic && (body.linear_velocity.length_squared() != 0.0f || body.angular_velocity.length_squared() != 0.0f))
        return std::string("a ") + motion_type_name(body.motion) + " body has no initial velocity";
    if (body.group >= collision_group_count) return "the collision group must be 0 to 15";
    return {};
}

struct PhysicsWorld::Impl {
    struct Record {
        EntityHandle entity;
        JPH::BodyID body;
        MotionType motion = MotionType::static_body;
        bool alive = true;
        bool targeted = false; // kinematic: received a target this tick
        bool coasting = false; // kinematic: moved toward a target last tick
        math::Vec3 position{0.0f}; // moving bodies: the pose last written to the World
        math::Quat rotation{};
        math::Mat4 world{}; // static bodies: the world matrix the shape was built for
        math::Vec3 scale{1.0f};
        std::vector<ColliderDesc> colliders; // static bodies: to rebuild when the scale changes
        float density = 1000.0f;
    };

    explicit Impl(PhysicsSettings value) : settings(value), temp(uint32_t(value.temp_allocator_bytes)) {
        system.Init(settings.max_bodies, 0, settings.max_body_pairs, settings.max_contact_constraints, layers,
                    object_vs_broad_phase, object_pairs);
        system.SetGravity(jolt(settings.gravity));
    }

    JPH::BodyInterface& bodies() { return system.GetBodyInterfaceNoLock(); }
    const JPH::BodyInterface& bodies() const { return system.GetBodyInterfaceNoLock(); }

    const Record* find(EntityHandle entity) const noexcept {
        const auto it = index.find(entity.slot);
        if (it == index.end()) return nullptr;
        const auto& record = records[it->second];
        return record.alive && record.entity == entity ? &record : nullptr;
    }
    Record* find(EntityHandle entity) noexcept {
        return const_cast<Record*>(std::as_const(*this).find(entity));
    }
    bool moving_body(EntityHandle entity) const noexcept {
        const auto* record = find(entity);
        return record && record->motion != MotionType::static_body;
    }
    bool has_moving_bodies() const noexcept { return live_moving > 0; }

    void remove(Record& record) {
        bodies().RemoveBody(record.body);
        bodies().DestroyBody(record.body);
        index.erase(record.entity.slot);
        record.alive = false;
        (record.motion == MotionType::static_body ? live_static : live_moving) -= 1;
        ++stats.bodies_removed;
        ++dead;
    }

    /// Keeps creation order while dropping removed records, once they outnumber the live ones.
    void compact() {
        if (dead < 64 || dead < records.size() / 2) return;
        auto kept = std::vector<Record>{};
        kept.reserve(records.size() - dead);
        for (auto& record : records)
            if (record.alive) kept.push_back(std::move(record));
        records = std::move(kept);
        index.clear();
        for (uint32_t i = 0; i < records.size(); ++i) {
            index[records[i].entity.slot] = i;
            bodies().SetUserData(records[i].body, i);
        }
        dead = 0;
    }

    std::string refuse_create(const World& world, EntityHandle entity, const BodyDesc& body, math::Vec3& position,
                              math::Quat& rotation, math::Vec3& scale, math::Mat4& matrix) const;
    void create(const World& world, EntityHandle entity, const BodyDesc& body, const std::string& source,
                std::vector<JPH::BodyID>& activate, std::vector<JPH::BodyID>& resting);

    PhysicsSettings settings;
    BroadPhaseLayers layers;
    ObjectVsBroadPhase object_vs_broad_phase;
    ObjectPairs object_pairs;
    JPH::TempAllocatorImplWithMallocFallback temp;
    JPH::PhysicsSystem system;
    std::vector<Record> records; // creation order
    std::unordered_map<uint32_t, uint32_t> index; // entity slot -> record
    size_t live_static = 0, live_moving = 0, dead = 0;
    PhysicsStats stats;
};

std::string PhysicsWorld::Impl::refuse_create(const World& world, EntityHandle entity, const BodyDesc& body,
                                              math::Vec3& position, math::Quat& rotation, math::Vec3& scale,
                                              math::Mat4& matrix) const {
    if (!world.alive(entity)) return "the entity no longer exists";
    auto transform = std::optional<TransformComponent>{};
    world.with<TransformComponent>(entity, [&](const TransformComponent& value) { transform = value; });
    if (!transform) return "a body needs a transform";
    if (find(entity)) return "the entity already has a body";
    const auto any_body_below = [&](auto&& self, EntityHandle parent) -> bool {
        for (const auto child : world.children(parent))
            if (find(child) || self(self, child)) return true;
        return false;
    };
    if (body.motion != MotionType::static_body) {
        if (world.parent(entity)) return std::string("a ") + motion_type_name(body.motion) + " body must be a root entity";
        if (!unit_scale(transform->scale)) return std::string("a ") + motion_type_name(body.motion) + " body needs unit scale";
        if (any_body_below(any_body_below, entity))
            return "an entity below it has a body; colliders under a moving body are part of its shape";
        position = transform->translation;
        rotation = *normalized(transform->rotation);
        scale = math::Vec3(1.0f);
    } else {
        for (auto parent = world.parent(entity); parent; parent = world.parent(*parent))
            if (moving_body(*parent)) return "it is below a moving body; colliders under a moving body are part of its shape";
        const auto world_matrix = world.world_matrix(entity);
        const auto pose = world_matrix ? decompose_transform(*world_matrix) : std::nullopt;
        if (!pose) return "its world transform has shear or cannot be represented";
        matrix = *world_matrix;
        position = pose->translation;
        rotation = pose->rotation;
        scale = pose->scale;
    }
    return check_scale(body.colliders, scale);
}

void PhysicsWorld::Impl::create(const World& world, EntityHandle entity, const BodyDesc& body, const std::string& source,
                                std::vector<JPH::BodyID>& activate, std::vector<JPH::BodyID>& resting) {
    auto position = math::Vec3(0.0f);
    auto rotation = math::Quat{};
    auto scale = math::Vec3(1.0f);
    auto matrix = math::Mat4{};
    const auto fail = [&](const std::string& reason) {
        throw std::runtime_error((source.empty() ? std::string() : source + ": ") + "cannot create a " +
                                 motion_type_name(body.motion) + " body for " + entity_text(world, entity) + ": " + reason);
    };
    if (auto reason = refuse_create(world, entity, body, position, rotation, scale, matrix); !reason.empty()) fail(reason);
    auto creation = JPH::BodyCreationSettings(make_shape(body.colliders, scale, body.density), JPH::RVec3(jolt(position)),
                                              jolt(rotation), jolt_motion(body.motion), object_layer(body));
    creation.mUserData = records.size();
    creation.mFriction = body.friction;
    creation.mRestitution = body.restitution;
    creation.mLinearDamping = body.linear_damping;
    creation.mAngularDamping = body.angular_damping;
    creation.mGravityFactor = body.gravity_factor;
    creation.mLinearVelocity = jolt(body.linear_velocity);
    creation.mAngularVelocity = jolt(body.angular_velocity);
    if (body.motion == MotionType::dynamic && body.mass > 0.0f) {
        creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        creation.mMassPropertiesOverride.mMass = body.mass;
    }
    auto* created = bodies().CreateBody(creation);
    if (!created) fail("the physics world is full (" + std::to_string(settings.max_bodies) + " bodies)");
    auto record = Record{};
    record.entity = entity;
    record.body = created->GetID();
    record.motion = body.motion;
    record.position = position;
    record.rotation = rotation;
    if (body.motion == MotionType::static_body) {
        record.world = matrix;
        record.scale = scale;
        record.colliders = body.colliders;
        record.density = body.density;
        resting.push_back(record.body);
        ++live_static;
    } else {
        (body.motion == MotionType::dynamic ? activate : resting).push_back(record.body);
        ++live_moving;
    }
    index[entity.slot] = uint32_t(records.size());
    records.push_back(std::move(record));
    ++stats.bodies_created;
}

// --- BodyCommands ---------------------------------------------------------------------------------

BodyCommands::BodyCommands(const PhysicsWorld& physics, const World& world) : m_physics(physics), m_world(world) {}

EntityHandle BodyCommands::require_body(EntityHandle entity, const char* action, bool kinematic, bool dynamic) const {
    const auto* record = m_physics.m_impl->find(entity);
    const auto describe = [&](const std::string& reason) {
        return std::invalid_argument(std::string("cannot ") + action + " " + entity_text(m_world, entity) + ": " + reason);
    };
    if (!m_world.alive(entity)) throw describe("the entity does not exist");
    if (!record) throw describe("it has no body");
    const auto allowed = (record->motion == MotionType::kinematic && kinematic) || (record->motion == MotionType::dynamic && dynamic);
    if (!allowed) {
        auto reason = std::string("its body is ") + motion_type_name(record->motion);
        if (record->motion == MotionType::static_body) reason += "; move a static collider by writing its transform";
        else if (record->motion == MotionType::kinematic) reason += "; move it with a kinematic target";
        throw describe(reason);
    }
    return entity;
}

void BodyCommands::push(Kind kind, EntityHandle entity, math::Vec3 vector, math::Quat rotation) {
    if (!finite(vector)) throw std::invalid_argument("A physics request for " + entity_text(m_world, entity) + " is not finite");
    m_requests.push_back({kind, entity, vector, rotation, nullptr, {}});
}

void BodyCommands::create(EntityTarget entity, BodyDesc body) {
    if (auto reason = validate_body(body); !reason.empty()) {
        const auto who = std::holds_alternative<EntityHandle>(entity) ? entity_text(m_world, std::get<EntityHandle>(entity))
                                                                       : std::string("a new entity");
        throw std::invalid_argument("cannot create a body for " + who + ": " + reason);
    }
    if (const auto* handle = std::get_if<EntityHandle>(&entity)) {
        if (!m_world.alive(*handle)) throw std::invalid_argument("cannot create a body for an entity that does not exist");
        if (m_physics.has_body(*handle))
            throw std::invalid_argument("cannot create a body for " + entity_text(m_world, *handle) + ": it already has one");
    }
    for (const auto& request : m_requests)
        if (request.kind == Kind::create && request.entity == entity)
            throw std::invalid_argument("a body for this entity was already requested in this tick");
    for (auto& collider : body.colliders) collider.rotation = *normalized(collider.rotation);
    m_requests.push_back({Kind::create, entity, {}, {}, std::make_unique<BodyDesc>(std::move(body)), m_source});
}

void BodyCommands::remove(EntityHandle entity) {
    if (!m_physics.has_body(entity)) throw std::invalid_argument("cannot remove the body of " + entity_text(m_world, entity) + ": it has none");
    m_requests.push_back({Kind::remove, entity, {}, {}, nullptr, m_source});
}

void BodyCommands::add_force(EntityHandle entity, math::Vec3 force) {
    push(Kind::force, require_body(entity, "push", false, true), force);
}
void BodyCommands::add_torque(EntityHandle entity, math::Vec3 torque) {
    push(Kind::torque, require_body(entity, "turn", false, true), torque);
}
void BodyCommands::add_impulse(EntityHandle entity, math::Vec3 impulse) {
    push(Kind::impulse, require_body(entity, "push", false, true), impulse);
}
void BodyCommands::add_angular_impulse(EntityHandle entity, math::Vec3 impulse) {
    push(Kind::angular_impulse, require_body(entity, "turn", false, true), impulse);
}
void BodyCommands::set_linear_velocity(EntityHandle entity, math::Vec3 velocity) {
    push(Kind::linear_velocity, require_body(entity, "set the velocity of", false, true), velocity);
}
void BodyCommands::set_angular_velocity(EntityHandle entity, math::Vec3 velocity) {
    push(Kind::angular_velocity, require_body(entity, "set the velocity of", false, true), velocity);
}
void BodyCommands::set_kinematic_target(EntityHandle entity, math::Vec3 position, math::Quat rotation) {
    require_body(entity, "set a kinematic target for", true, false);
    const auto unit = normalized(rotation);
    if (!unit) throw std::invalid_argument("A kinematic target rotation must be a finite, nonzero quaternion");
    for (const auto& request : m_requests)
        if (request.kind == Kind::kinematic_target && request.entity == EntityTarget(entity))
            throw std::invalid_argument("A kinematic target for " + entity_text(m_world, entity) + " was already set in this tick");
    push(Kind::kinematic_target, entity, position, *unit);
}
void BodyCommands::teleport(EntityHandle entity, math::Vec3 position, math::Quat rotation) {
    require_body(entity, "teleport", true, true);
    const auto unit = normalized(rotation);
    if (!unit) throw std::invalid_argument("A teleport rotation must be a finite, nonzero quaternion");
    push(Kind::teleport, entity, position, *unit);
}
void BodyCommands::wake(EntityHandle entity) {
    push(Kind::wake, require_body(entity, "wake", true, true), {});
}

// --- PhysicsWorld ---------------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld(PhysicsSettings settings) {
    if (settings.max_bodies == 0 || settings.max_bodies > JPH::BodyID::cMaxBodyIndex || settings.max_body_pairs == 0 ||
        settings.max_contact_constraints == 0 || settings.collision_steps == 0 || settings.collision_steps > 16 ||
        settings.temp_allocator_bytes < 64 * 1024 || settings.temp_allocator_bytes > (size_t{1} << 31) || !finite(settings.gravity))
        throw std::invalid_argument("Physics settings are out of range");
    detail::initialize_jolt();
    m_impl = std::make_unique<Impl>(settings);
}

PhysicsWorld::~PhysicsWorld() = default; // the PhysicsSystem destroys its bodies

const PhysicsSettings& PhysicsWorld::settings() const noexcept { return m_impl->settings; }

bool PhysicsWorld::has_body(EntityHandle entity) const noexcept { return m_impl->find(entity) != nullptr; }

std::optional<MotionType> PhysicsWorld::motion_type(EntityHandle entity) const noexcept {
    const auto* record = m_impl->find(entity);
    return record ? std::optional(record->motion) : std::nullopt;
}

std::optional<BodyState> PhysicsWorld::state(EntityHandle entity) const {
    const auto* record = m_impl->find(entity);
    if (!record) return std::nullopt;
    const auto& bodies = m_impl->bodies();
    auto position = JPH::RVec3();
    auto rotation = JPH::Quat();
    bodies.GetPositionAndRotation(record->body, position, rotation);
    auto state = BodyState{record->motion, maya_vector(JPH::Vec3(position)), maya_quat(rotation)};
    if (record->motion != MotionType::static_body) {
        state.linear_velocity = maya_vector(bodies.GetLinearVelocity(record->body));
        state.angular_velocity = maya_vector(bodies.GetAngularVelocity(record->body));
        state.sleeping = !bodies.IsActive(record->body);
    }
    if (record->motion == MotionType::dynamic) {
        const auto lock = JPH::BodyLockRead(m_impl->system.GetBodyLockInterfaceNoLock(), record->body);
        if (lock.Succeeded()) state.mass = 1.0f / lock.GetBody().GetMotionProperties()->GetInverseMass();
    }
    return state;
}

PhysicsStats PhysicsWorld::stats() const {
    auto stats = m_impl->stats;
    for (const auto& record : m_impl->records) {
        if (!record.alive) continue;
        ++stats.bodies;
        switch (record.motion) {
        case MotionType::static_body: ++stats.static_bodies; break;
        case MotionType::kinematic: ++stats.kinematic_bodies; break;
        case MotionType::dynamic: ++stats.dynamic_bodies; break;
        }
        if (record.motion != MotionType::static_body && m_impl->bodies().IsActive(record.body)) ++stats.active_bodies;
    }
    return stats;
}

void PhysicsWorld::check_world_commands(const World& world, const WorldCommands& commands, size_t first) const {
    if (m_impl->records.size() == m_impl->dead) return; // no bodies
    const auto moving_above = [&](EntityHandle entity) {
        for (auto parent = std::optional(entity); parent; parent = world.parent(*parent))
            if (m_impl->moving_body(*parent)) return true;
        return false;
    };
    const auto body_at_or_below = [&](auto&& self, EntityHandle entity) -> bool {
        if (m_impl->find(entity)) return true;
        for (const auto child : world.children(entity))
            if (self(self, child)) return true;
        return false;
    };
    for (const auto& staged : commands.staged(first)) {
        const auto* entity = std::get_if<EntityHandle>(&staged.target);
        if (!entity || !world.alive(*entity)) continue;
        const auto transform = staged.component == typeid(TransformComponent);
        using Kind = WorldCommands::Kind;
        if (staged.kind == Kind::set_transform && m_impl->moving_body(*entity))
            throw std::invalid_argument("cannot set the transform of " + entity_text(world, *entity) + ": its " +
                                        motion_type_name(*motion_type(*entity)) + " body's pose is written by physics; " +
                                        "use a force, a velocity, a kinematic target, or a teleport");
        if (staged.kind == Kind::remove && transform && has_body(*entity))
            throw std::invalid_argument("cannot remove the transform of " + entity_text(world, *entity) + ": it has a body");
        if (staged.kind == Kind::reparent) {
            if (m_impl->moving_body(*entity))
                throw std::invalid_argument("cannot reparent " + entity_text(world, *entity) + ": its " +
                                            motion_type_name(*motion_type(*entity)) + " body must stay a root entity");
            const auto* parent = staged.parent ? std::get_if<EntityHandle>(&*staged.parent) : nullptr;
            if (parent && world.alive(*parent) && moving_above(*parent) && body_at_or_below(body_at_or_below, *entity))
                throw std::invalid_argument("cannot reparent " + entity_text(world, *entity) + " under a moving body: " +
                                            "it has a body at or below it, and colliders under a moving body are part of its shape");
        }
    }
}

void PhysicsWorld::prepare(const World& world, const BodyCommands& requests, float interval) {
    auto& impl = *m_impl;
    const auto timer = PhaseTimer(impl.stats.prepare_ms);
    auto& bodies = impl.bodies();
    // Static colliders whose committed transforms changed.
    if (impl.live_static > 0) {
        for (auto& record : impl.records) {
            if (!record.alive || record.motion != MotionType::static_body) continue;
            const auto matrix = world.world_matrix(record.entity);
            if (matrix && std::equal(std::begin(matrix->elements), std::end(matrix->elements), std::begin(record.world.elements))) continue;
            const auto pose = matrix ? decompose_transform(*matrix) : std::nullopt;
            if (!pose) throw std::runtime_error("the static collider of " + entity_text(world, record.entity) +
                                                " can no longer be placed: its world transform has shear or cannot be represented");
            const auto same_scale = nearly_equal(pose->scale.x, record.scale.x) && nearly_equal(pose->scale.y, record.scale.y) &&
                                    nearly_equal(pose->scale.z, record.scale.z);
            if (!same_scale) {
                if (auto reason = check_scale(record.colliders, pose->scale); !reason.empty())
                    throw std::runtime_error("the static collider of " + entity_text(world, record.entity) + " cannot take its new scale: " + reason);
                bodies.SetShape(record.body, make_shape(record.colliders, pose->scale, record.density), false, JPH::EActivation::DontActivate);
                record.scale = pose->scale;
            }
            bodies.SetPositionAndRotation(record.body, JPH::RVec3(jolt(pose->translation)), jolt(pose->rotation),
                                          JPH::EActivation::DontActivate);
            record.world = *matrix;
        }
    }
    // This tick's requests, in the order they were made.
    using Kind = BodyCommands::Kind;
    for (const auto& request : requests.m_requests) {
        if (request.kind == Kind::create || request.kind == Kind::remove) continue;
        auto* record = impl.find(std::get<EntityHandle>(request.entity));
        if (!record) continue; // checked when requested; nothing removes bodies before the commit
        const auto id = record->body;
        const auto vector = jolt(request.vector);
        switch (request.kind) {
        case Kind::force: bodies.AddForce(id, vector); break;
        case Kind::torque: bodies.AddTorque(id, vector); break;
        case Kind::impulse: bodies.AddImpulse(id, vector); break;
        case Kind::angular_impulse: bodies.AddAngularImpulse(id, vector); break;
        case Kind::linear_velocity: bodies.SetLinearVelocity(id, vector); break;
        case Kind::angular_velocity: bodies.SetAngularVelocity(id, vector); break;
        case Kind::kinematic_target:
            bodies.MoveKinematic(id, JPH::RVec3(vector), jolt(request.rotation), interval);
            record->targeted = true;
            break;
        case Kind::teleport:
            bodies.SetPositionAndRotation(id, JPH::RVec3(vector), jolt(request.rotation), JPH::EActivation::Activate);
            break;
        case Kind::wake: bodies.ActivateBody(id); break;
        case Kind::create:
        case Kind::remove: break;
        }
    }
    // A kinematic body without a target this tick stops where it is.
    for (auto& record : impl.records) {
        if (!record.alive || record.motion != MotionType::kinematic) continue;
        if (!record.targeted && record.coasting) bodies.SetLinearAndAngularVelocity(record.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        record.coasting = record.targeted;
        record.targeted = false;
    }
}

void PhysicsWorld::step(float interval) {
    auto& impl = *m_impl;
    const auto timer = PhaseTimer(impl.stats.step_ms);
    const auto errors = impl.system.Update(interval, int(impl.settings.collision_steps), &impl.temp, &detail::physics_jobs());
    ++impl.stats.steps;
    if (errors == JPH::EPhysicsUpdateError::None) return;
    ++impl.stats.steps_with_errors;
    const auto has = [&](JPH::EPhysicsUpdateError flag) { return (errors & flag) != JPH::EPhysicsUpdateError::None; };
    impl.stats.manifold_cache_full += has(JPH::EPhysicsUpdateError::ManifoldCacheFull) ? 1 : 0;
    impl.stats.body_pair_cache_full += has(JPH::EPhysicsUpdateError::BodyPairCacheFull) ? 1 : 0;
    impl.stats.contact_constraints_full += has(JPH::EPhysicsUpdateError::ContactConstraintsFull) ? 1 : 0;
}

void PhysicsWorld::synchronize(const World& world, WorldCommands& commands) {
    auto& impl = *m_impl;
    const auto timer = PhaseTimer(impl.stats.synchronize_ms);
    if (!impl.has_moving_bodies()) return;
    // Entities destroyed in this batch get no pose.
    auto destroyed = std::unordered_set<uint32_t>{};
    for (const auto& staged : commands.staged())
        if (staged.kind == WorldCommands::Kind::destroy)
            if (const auto* entity = std::get_if<EntityHandle>(&staged.target)) destroyed.insert(entity->slot);
    const auto& bodies = impl.bodies();
    for (auto& record : impl.records) {
        if (!record.alive || record.motion == MotionType::static_body || destroyed.contains(record.entity.slot)) continue;
        auto position = JPH::RVec3();
        auto rotation = JPH::Quat();
        bodies.GetPositionAndRotation(record.body, position, rotation);
        const auto moved = maya_vector(JPH::Vec3(position));
        const auto turned = maya_quat(rotation);
        if (moved.x == record.position.x && moved.y == record.position.y && moved.z == record.position.z &&
            turned.x == record.rotation.x && turned.y == record.rotation.y && turned.z == record.rotation.z &&
            turned.w == record.rotation.w)
            continue;
        if (!world.alive(record.entity)) continue;
        commands.set_transform(record.entity, TransformComponent{moved, turned, math::Vec3(1.0f)});
        record.position = moved;
        record.rotation = turned;
    }
}

void PhysicsWorld::commit(const World& world, const BodyCommands& requests, const WorldCommitResult& result) {
    auto& impl = *m_impl;
    const auto timer = PhaseTimer(impl.stats.commit_ms);
    for (auto& record : impl.records)
        if (record.alive && !world.alive(record.entity)) impl.remove(record);
    auto activate = std::vector<JPH::BodyID>{};
    auto resting = std::vector<JPH::BodyID>{};
    const auto add_created = [&] {
        // Added in creation order, as one batch per activation state.
        for (auto* ids : {&resting, &activate}) {
            if (ids->empty()) continue;
            const auto mode = ids == &activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
            auto state = impl.bodies().AddBodiesPrepare(ids->data(), int(ids->size()));
            impl.bodies().AddBodiesFinalize(ids->data(), int(ids->size()), state, mode);
        }
        if (activate.size() + resting.size() >= 256) impl.system.OptimizeBroadPhase();
        activate.clear();
        resting.clear();
    };
    using Kind = BodyCommands::Kind;
    try {
        for (const auto& request : requests.m_requests) {
            if (request.kind == Kind::remove) {
                if (auto* record = impl.find(std::get<EntityHandle>(request.entity))) {
                    add_created(); // keep the request order: bodies created before it exist first
                    impl.remove(*record);
                }
            } else if (request.kind == Kind::create) {
                auto entity = EntityHandle{};
                if (const auto* handle = std::get_if<EntityHandle>(&request.entity)) entity = *handle;
                else {
                    const auto pending = std::get<PendingEntity>(request.entity);
                    if (pending.index >= result.created.size())
                        throw std::runtime_error(request.source + ": a body was requested for an entity from another batch");
                    entity = result.created[pending.index];
                }
                impl.create(world, entity, *request.body, request.source, activate, resting);
            }
        }
    } catch (...) {
        add_created(); // bodies created before the failure are added, never leaked
        throw;
    }
    add_created();
    impl.compact();
}

} // namespace maya
