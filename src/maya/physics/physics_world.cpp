#include "maya/physics/physics.hpp"
#include "jolt_runtime.hpp"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
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

/// "entity 70 2", followed by the entity's name in quotes when it has one.
std::string entity_text(const World& world, EntityHandle entity) {
    auto text = std::ostringstream{};
    text << "entity ";
    if (const auto id = world.persistent_id(entity)) text << std::hex << id->high << ' ' << id->low;
    else text << "(slot " << entity.slot << ')';
    world.with<NameComponent>(entity, [&](const NameComponent& name) {
        if (!name.value.empty()) text << " \"" << name.value << '"';
    });
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
// Positions cross as doubles: Jolt is built with JPH_DOUBLE_PRECISION (#1065), so RVec3 is double.
JPH::RVec3 jolt_position(const math::DVec3& value) noexcept { return {value.x, value.y, value.z}; }
math::DVec3 maya_position(JPH::RVec3Arg value) noexcept { return {value.GetX(), value.GetY(), value.GetZ()}; }

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
    if (!finite(collider.scale) || !(collider.scale.x > 0.0f && collider.scale.y > 0.0f && collider.scale.z > 0.0f))
        return "a collider scale must be finite and positive";
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

math::Vec3 times(const math::Vec3& a, const math::Vec3& b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
bool uniform(const math::Vec3& scale) noexcept {
    return nearly_equal(scale.x, scale.y) && nearly_equal(scale.y, scale.z);
}

/// Checks the scale rules of the physics contract for shapes placed in an entity scaled by `scale`
/// (the entity's world scale for a static body, 1 for a moving one).
std::string check_scale(const std::vector<ColliderDesc>& colliders, const math::Vec3& scale) {
    for (const auto& collider : colliders) {
        if (!uniform(scale) && !near_identity(*normalized(collider.rotation)))
            return "a rotated collider cannot take its entity's nonuniform scale";
        const auto shape = times(collider.scale, scale);
        if (std::holds_alternative<SphereShape>(collider.shape) && !uniform(shape))
            return "a sphere needs uniform scale";
        if (std::holds_alternative<CapsuleShape>(collider.shape) && !nearly_equal(shape.x, shape.z))
            return "a capsule needs the same scale on X and Z";
    }
    return {};
}

JPH::Ref<JPH::Shape> make_shape(const std::vector<ColliderDesc>& colliders, const math::Vec3& scale, float density) {
    const auto convex = [&](const ColliderDesc& collider) -> JPH::Ref<JPH::ConvexShape> {
        const auto size = times(collider.scale, scale);
        auto shape = std::visit([&](const auto& value) -> JPH::Ref<JPH::ConvexShape> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, BoxShape>) {
                const auto half = JPH::Vec3(value.half_extents.x * size.x, value.half_extents.y * size.y, value.half_extents.z * size.z);
                return new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, 0.25f * half.ReduceMin()));
            } else if constexpr (std::is_same_v<T, SphereShape>) {
                return new JPH::SphereShape(value.radius * size.x);
            } else {
                return new JPH::CapsuleShape(value.half_height * size.y, value.radius * size.x);
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
        if (collider.offset.length_squared() == 0.0f && near_identity(rotation)) return shape; // centred, unrotated
        return new JPH::RotatedTranslatedShape(offset(collider), jolt(rotation), shape);
    }
    auto compound = JPH::StaticCompoundShapeSettings();
    for (const auto& collider : colliders)
        compound.AddShape(offset(collider), jolt(*normalized(collider.rotation)), convex(collider).GetPtr());
    auto result = compound.Create();
    if (result.HasError()) throw std::runtime_error("the compound shape cannot be built: " + std::string(result.GetError().c_str()));
    return result.Get();
}

/// Jolt's per-step scratch allocator, counting the most it held at once. Jolt uses it as the wrapped
/// allocator requires, so the counts need no more synchronization than the allocator itself.
class CountedTempAllocator final : public JPH::TempAllocator {
public:
    explicit CountedTempAllocator(JPH::uint size) : m_inner(size), m_capacity(size) {}
    void* Allocate(JPH::uint size) override {
        m_used += JPH::AlignUp(size, JPH_RVECTOR_ALIGNMENT);
        m_high = std::max(m_high, m_used);
        return m_inner.Allocate(size);
    }
    void Free(void* address, JPH::uint size) override {
        m_used -= JPH::AlignUp(size, JPH_RVECTOR_ALIGNMENT);
        m_inner.Free(address, size);
    }
    size_t high_water() const noexcept { return m_high; }
    size_t capacity() const noexcept { return m_capacity; }

private:
    JPH::TempAllocatorImplWithMallocFallback m_inner;
    size_t m_capacity, m_used = 0, m_high = 0;
};

/// A contact that began or ended in a step, as Jolt reported it from a worker thread.
struct ContactRecord {
    bool added = false;
    JPH::BodyID a, b; // a < b
    JPH::SubShapeID sub_a, sub_b;
    math::DVec3 point{};
    math::Vec3 normal{0.0f}; // added: normal from a toward b
    float speed = 0.0f; // added: approach speed along the normal
    bool sensor = false; // added: either body is a sensor
};

/// Jolt's contact callbacks run on worker threads while every body is locked: they only append plain
/// records, and never throw. The owner thread takes them after the step (PhysicsWorld::take_events).
class ContactRecorder final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        auto record = ContactRecord{true, a.GetID(), b.GetID(), manifold.mSubShapeID1, manifold.mSubShapeID2};
        const auto point = manifold.GetWorldSpaceContactPointOn1(0);
        const auto normal = manifold.mWorldSpaceNormal;
        record.point = maya_position(point);
        record.normal = maya_vector(normal);
        record.speed = (a.GetPointVelocity(point) - b.GetPointVelocity(point)).Dot(normal);
        record.sensor = a.IsSensor() || b.IsSensor();
        append(record);
        if (m_capture.load(std::memory_order_relaxed) && !record.sensor) capture(a, b, manifold);
    }
    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        if (m_capture.load(std::memory_order_relaxed) && !a.IsSensor() && !b.IsSensor()) capture(a, b, manifold);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        append({false, pair.GetBody1ID(), pair.GetBody2ID(), pair.GetSubShapeID1(), pair.GetSubShapeID2()});
    }
    /// Debug views: every contact point of the step's solid contacts, while capture is on.
    struct Point {
        JPH::BodyID a, b;
        math::DVec3 point{};
        math::Vec3 normal{0.0f};
    };
    void set_capture(bool on) noexcept { m_capture = on; }
    bool capturing() const noexcept { return m_capture; }
    void take_points(std::vector<Point>& into) {
        into.clear();
        const auto lock = std::scoped_lock(m_mutex);
        std::swap(into, m_points);
    }
    /// Swaps the recorded contacts into `into` (cleared first), keeping both buffers' capacity.
    void take(std::vector<ContactRecord>& into) {
        into.clear();
        const auto lock = std::scoped_lock(m_mutex);
        std::swap(into, m_records);
    }
    uint64_t dropped() const noexcept { return m_dropped; }

private:
    void append(const ContactRecord& record) noexcept {
        try {
            const auto lock = std::scoped_lock(m_mutex);
            m_records.push_back(record);
        } catch (...) {
            ++m_dropped; // out of memory: the event is lost, never the step
        }
    }
    void capture(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold) noexcept {
        try {
            const auto lock = std::scoped_lock(m_mutex);
            for (JPH::uint i = 0; i < manifold.mRelativeContactPointsOn1.size(); ++i)
                m_points.push_back({a.GetID(), b.GetID(), maya_position(manifold.GetWorldSpaceContactPointOn1(i)),
                                    maya_vector(manifold.mWorldSpaceNormal)});
        } catch (...) {
            // out of memory: a debug view misses a point
        }
    }
    std::mutex m_mutex;
    std::vector<ContactRecord> m_records;
    std::vector<Point> m_points;
    std::atomic<uint64_t> m_dropped = 0;
    std::atomic<bool> m_capture = false;
};

/// Queries hit the groups in the filter's mask.
class QueryGroups final : public JPH::ObjectLayerFilter {
public:
    explicit QueryGroups(uint16_t groups) : m_groups(groups) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return (uint32_t(m_groups) >> group_of(layer) & 1u) != 0; }

private:
    uint16_t m_groups;
};

/// Queries skip sensors unless asked, and the ignored body.
class QueryBodies final : public JPH::BodyFilter {
public:
    QueryBodies(bool sensors, JPH::BodyID ignore) : m_sensors(sensors), m_ignore(ignore) {}
    bool ShouldCollide(const JPH::BodyID& body) const override { return body != m_ignore; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return m_sensors || !body.IsSensor(); }

private:
    bool m_sensors;
    JPH::BodyID m_ignore;
};

std::optional<math::Vec3> unit(const math::Vec3& value) noexcept {
    if (!finite(value)) return std::nullopt;
    const auto length = value.length();
    if (!(length > 1e-12f)) return std::nullopt;
    return value * (1.0f / length);
}

} // namespace

const char* physics_event_name(PhysicsEventKind kind) noexcept {
    switch (kind) {
    case PhysicsEventKind::contact_begin: return "contact_begin";
    case PhysicsEventKind::contact_end: return "contact_end";
    case PhysicsEventKind::trigger_enter: return "trigger_enter";
    case PhysicsEventKind::trigger_exit: break;
    }
    return "trigger_exit";
}

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
        EntityId id; // for sorting query results and events
        JPH::BodyID body;
        MotionType motion = MotionType::static_body;
        bool alive = true;
        bool targeted = false; // kinematic: received a target this tick
        bool coasting = false; // kinematic: moved toward a target last tick
        math::DVec3 position{}; // moving bodies: the pose last written to the World
        math::Quat rotation{};
        math::Affine world{}; // static bodies: the world pose the shape was built for
        math::Vec3 scale{1.0f};
        std::vector<ColliderDesc> colliders; // to rebuild static bodies when their scale changes, and for debug views
        float density = 1000.0f;
        bool sensor = false;
        uint8_t group = 0;
    };

    explicit Impl(PhysicsSettings value) : settings(value), temp(uint32_t(value.temp_allocator_bytes)) {
        system.Init(settings.max_bodies, 0, settings.max_body_pairs, settings.max_contact_constraints, layers,
                    object_vs_broad_phase, object_pairs);
        system.SetGravity(jolt(settings.gravity));
        system.SetContactListener(&contacts);
    }

    /// Two bodies in contact: how many of their sub-shape pairs touch, and whether the contact is
    /// held while a body sleeps (Jolt removes a sleeping body's contacts; Maya keeps the pair).
    struct Pair {
        int touching = 0;
        bool dormant = false;
        bool sensor = false;
    };
    using PairKey = std::pair<uint32_t, uint32_t>; // the bodies' IDs, lower first
    static PairKey key(JPH::BodyID a, JPH::BodyID b) noexcept {
        const auto x = a.GetIndexAndSequenceNumber(), y = b.GetIndexAndSequenceNumber();
        return x < y ? PairKey{x, y} : PairKey{y, x};
    }
    struct PairHash {
        size_t operator()(const PairKey& key) const noexcept { return std::hash<uint64_t>{}(uint64_t(key.first) << 32 | key.second); }
    };
    void add_pair(const PairKey& key, Pair pair) {
        if (pairs.emplace(key, pair).second) ++(pair.sensor ? overlapping : touching);
        pairs_of[key.first].push_back(key);
        pairs_of[key.second].push_back(key);
    }
    void erase_pair(const PairKey& key) {
        if (const auto found = pairs.find(key); found != pairs.end()) {
            --(found->second.sensor ? overlapping : touching);
            pairs.erase(found);
        }
        apart.erase(key);
        for (const auto body : {key.first, key.second}) {
            auto found = pairs_of.find(body);
            if (found == pairs_of.end()) continue;
            std::erase(found->second, key);
            if (found->second.empty()) pairs_of.erase(found);
        }
    }
    const Record* by_id(uint32_t body) const noexcept {
        const auto it = by_body.find(body);
        return it == by_body.end() ? nullptr : &records[it->second];
    }
    /// An event between two records, `first` the lower EntityId; the normal turns with the order.
    PhysicsEvent event(bool begin, const Pair& pair, const Record& a, const Record& b, const ContactRecord* touch) const {
        auto result = PhysicsEvent{};
        result.kind = pair.sensor ? (begin ? PhysicsEventKind::trigger_enter : PhysicsEventKind::trigger_exit)
                                  : (begin ? PhysicsEventKind::contact_begin : PhysicsEventKind::contact_end);
        const auto swapped = b.id < a.id;
        const auto& first = swapped ? b : a;
        const auto& second = swapped ? a : b;
        result.first = first.id;
        result.second = second.id;
        result.first_entity = first.entity;
        result.second_entity = second.entity;
        if (touch) {
            result.point = touch->point;
            result.normal = swapped ? touch->normal * -1.0f : touch->normal; // records run from the lower body ID
            if (touch->a != a.body) result.normal = result.normal * -1.0f;
            result.speed = touch->speed;
        }
        return result;
    }
    /// Ends every pair a body is part of, as `removed` events for the next delivery.
    void end_pairs_of(const Record& record) {
        const auto body = record.body.GetIndexAndSequenceNumber();
        const auto found = pairs_of.find(body);
        if (found == pairs_of.end()) return;
        const auto keys = found->second; // erase_pair changes the list
        for (const auto& key : keys) {
            const auto other = key.first == body ? key.second : key.first;
            if (const auto* partner = by_id(other)) {
                auto ended = event(false, pairs.at(key), record, *partner, nullptr);
                ended.removed = true;
                pending.push_back(ended);
            }
            erase_pair(key);
        }
    }
    /// Keeps a recipient only if its entity is still in the World and not destroyed in `destroyed`.
    void resolve(std::optional<EntityHandle>& recipient, const World& world, const std::unordered_set<uint32_t>& destroyed) {
        if (recipient && world.alive(*recipient) && !destroyed.contains(recipient->slot)) return;
        recipient.reset();
        ++stats.event_recipients_skipped;
    }
    std::vector<PhysicsEvent> finish(std::vector<PhysicsEvent> events, uint64_t tick, const World& world,
                                     const std::unordered_set<uint32_t>& destroyed) {
        for (auto& e : events) {
            e.tick = tick;
            resolve(e.first_entity, world, destroyed);
            resolve(e.second_entity, world, destroyed);
        }
        std::ranges::sort(events, {}, [](const PhysicsEvent& e) { return std::tuple(e.kind, e.first, e.second); });
        stats.events += events.size();
        return events;
    }

    /// One result per body, nearest first: `fraction(hit)` picks each body's nearest raw hit, and only
    /// those are turned into results by `place`, which may be costly (a ray's normal locks the body).
    template<class Collector>
    std::vector<QueryHit> hits(const Collector& collector, auto&& fraction, auto&& place) const {
        using Hit = std::decay_t<decltype(collector.mHits[0])>;
        auto nearest = std::map<uint32_t, const Hit*>{}; // by body, in body ID order
        for (const auto& hit : collector.mHits) {
            auto [it, added] = nearest.try_emplace(hit.mBodyID2.GetIndexAndSequenceNumber(), &hit);
            if (!added && fraction(hit) < fraction(*it->second)) it->second = &hit;
        }
        auto results = std::vector<QueryHit>{};
        results.reserve(nearest.size());
        for (const auto& [body, hit] : nearest) {
            const auto* record = by_id(body);
            if (!record) continue;
            auto result = place(*hit);
            result.entity = record->entity;
            result.id = record->id;
            results.push_back(result);
        }
        std::ranges::sort(results, {}, [](const QueryHit& h) { return std::tuple(h.distance, h.id); });
        return results;
    }
    const JPH::NarrowPhaseQuery& query() const { return system.GetNarrowPhaseQueryNoLock(); }
    JPH::BodyID ignored(const QueryFilter& filter) const {
        const auto* record = filter.ignore ? find(*filter.ignore) : nullptr;
        return record ? record->body : JPH::BodyID();
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
        end_pairs_of(record);
        index.erase(record.entity.slot);
        by_body.erase(record.body.GetIndexAndSequenceNumber());
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
        by_body.clear();
        for (uint32_t i = 0; i < records.size(); ++i) {
            index[records[i].entity.slot] = i;
            by_body[records[i].body.GetIndexAndSequenceNumber()] = i;
            bodies().SetUserData(records[i].body, i);
        }
        dead = 0;
    }

    std::string refuse_create(const World& world, EntityHandle entity, const BodyDesc& body, math::DVec3& position,
                              math::Quat& rotation, math::Vec3& scale, math::Affine& matrix) const;
    void create(const World& world, EntityHandle entity, const BodyDesc& body, const std::string& source,
                std::vector<JPH::BodyID>& activate, std::vector<JPH::BodyID>& resting);

    PhysicsSettings settings;
    BroadPhaseLayers layers;
    ObjectVsBroadPhase object_vs_broad_phase;
    ObjectPairs object_pairs;
    CountedTempAllocator temp;
    JPH::PhysicsSystem system;
    ContactRecorder contacts;
    std::vector<Record> records; // creation order
    std::unordered_map<uint32_t, uint32_t> index; // entity slot -> record
    std::unordered_map<uint32_t, uint32_t> by_body; // Jolt body ID (index and sequence) -> record
    std::unordered_map<PairKey, Pair, PairHash> pairs; // bodies in contact; events are sorted, so their order does not matter
    std::unordered_map<uint32_t, std::vector<PairKey>> pairs_of; // each body's pairs
    std::unordered_set<PairKey, PairHash> apart; // pairs touching nowhere now: held while asleep, or about to end
    // take_events' scratch, kept between steps: the step's records, and each pair's change.
    std::vector<ContactRecord> taken;
    std::unordered_map<PairKey, std::pair<int, const ContactRecord*>, PairHash> changes;
    std::vector<PhysicsEvent> pending; // ends from removed bodies, for the next delivery
    size_t live_static = 0, live_moving = 0, dead = 0;
    size_t touching = 0, overlapping = 0; // pairs: solid, and with a sensor
    PhysicsStats stats;
    mutable uint64_t queries = 0;
    // Debug views: the last step's contact points, and the queries since the last clear.
    std::vector<ContactRecorder::Point> points;
    std::vector<PhysicsDebugContact> debug_contacts;
    mutable std::vector<PhysicsDebugQuery> debug_queries;
    void log_query(PhysicsDebugQuery query, const std::vector<QueryHit>& hits) const {
        ++queries;
        if (!contacts.capturing()) return;
        for (const auto& hit : hits) query.hits.push_back(hit.point);
        debug_queries.push_back(std::move(query));
    }
    void take_debug_contacts() {
        debug_contacts.clear();
        contacts.take_points(points);
        for (const auto& point : points) {
            const auto* a = by_id(point.a.GetIndexAndSequenceNumber());
            const auto* b = by_id(point.b.GetIndexAndSequenceNumber());
            if (!a || !b) continue;
            const auto lower = point.a.GetIndexAndSequenceNumber() < point.b.GetIndexAndSequenceNumber();
            debug_contacts.push_back({point.point, lower ? point.normal : point.normal * -1.0f,
                                      uint16_t(1u << a->group | 1u << b->group)});
        }
        // Worker threads report in any order.
        std::ranges::sort(debug_contacts, {}, [](const PhysicsDebugContact& c) {
            return std::tuple(c.point.x, c.point.y, c.point.z, c.normal.x, c.normal.y, c.normal.z);
        });
    }
};

std::string PhysicsWorld::Impl::refuse_create(const World& world, EntityHandle entity, const BodyDesc& body,
                                              math::DVec3& position, math::Quat& rotation, math::Vec3& scale,
                                              math::Affine& matrix) const {
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
    auto position = math::DVec3{};
    auto rotation = math::Quat{};
    auto scale = math::Vec3(1.0f);
    auto matrix = math::Affine{};
    const auto fail = [&](const std::string& reason) {
        throw std::runtime_error((source.empty() ? std::string() : source + ": ") + "cannot create a " +
                                 motion_type_name(body.motion) + " body for " + entity_text(world, entity) + ": " + reason);
    };
    if (auto reason = refuse_create(world, entity, body, position, rotation, scale, matrix); !reason.empty()) fail(reason);
    auto creation = JPH::BodyCreationSettings(make_shape(body.colliders, scale, body.density), jolt_position(position),
                                              jolt(rotation), jolt_motion(body.motion), object_layer(body));
    creation.mUserData = records.size();
    creation.mFriction = body.friction;
    creation.mRestitution = body.restitution;
    creation.mLinearDamping = body.linear_damping;
    creation.mAngularDamping = body.angular_damping;
    creation.mGravityFactor = body.gravity_factor;
    creation.mIsSensor = body.sensor;
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
    record.id = world.persistent_id(entity).value_or(EntityId{});
    record.body = created->GetID();
    record.motion = body.motion;
    record.position = position;
    record.rotation = rotation;
    record.colliders = body.colliders;
    record.sensor = body.sensor;
    record.group = body.group;
    if (body.motion == MotionType::static_body) {
        record.world = matrix;
        record.scale = scale;
        record.density = body.density;
        resting.push_back(record.body);
        ++live_static;
    } else {
        (body.motion == MotionType::dynamic ? activate : resting).push_back(record.body);
        ++live_moving;
    }
    index[entity.slot] = uint32_t(records.size());
    by_body[record.body.GetIndexAndSequenceNumber()] = uint32_t(records.size());
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

void BodyCommands::push(Kind kind, EntityHandle entity, math::DVec3 vector, math::Quat rotation) {
    if (!std::isfinite(vector.x) || !std::isfinite(vector.y) || !std::isfinite(vector.z)) throw std::invalid_argument("A physics request for " + entity_text(m_world, entity) + " is not finite");
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
void BodyCommands::set_kinematic_target(EntityHandle entity, math::DVec3 position, math::Quat rotation) {
    require_body(entity, "set a kinematic target for", true, false);
    const auto unit = normalized(rotation);
    if (!unit) throw std::invalid_argument("A kinematic target rotation must be a finite, nonzero quaternion");
    for (const auto& request : m_requests)
        if (request.kind == Kind::kinematic_target && request.entity == EntityTarget(entity))
            throw std::invalid_argument("A kinematic target for " + entity_text(m_world, entity) + " was already set in this tick");
    push(Kind::kinematic_target, entity, position, *unit);
}
void BodyCommands::teleport(EntityHandle entity, math::DVec3 position, math::Quat rotation) {
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
    auto state = BodyState{record->motion, maya_position(position), maya_quat(rotation)};
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

void PhysicsWorld::set_debug_capture(bool on) {
    auto& impl = *m_impl;
    if (on == impl.contacts.capturing()) return;
    impl.contacts.set_capture(on);
    // Nothing captured is kept past turning it off, so a view that turns it on again starts clean.
    impl.take_debug_contacts();
    impl.debug_contacts.clear();
    impl.debug_queries.clear();
}

bool PhysicsWorld::debug_capture() const noexcept { return m_impl->contacts.capturing(); }

void PhysicsWorld::for_each_debug_body(const std::function<void(const PhysicsDebugBody&)>& visit) const {
    const auto& impl = *m_impl;
    for (const auto& record : impl.records) {
        if (!record.alive) continue;
        const auto sleeping = record.motion != MotionType::static_body && !impl.bodies().IsActive(record.body);
        visit({record.entity, record.motion, sleeping, record.sensor, record.group, record.colliders});
    }
}

const std::vector<PhysicsDebugContact>& PhysicsWorld::debug_contacts() const noexcept { return m_impl->debug_contacts; }
const std::vector<PhysicsDebugQuery>& PhysicsWorld::debug_queries() const noexcept { return m_impl->debug_queries; }
void PhysicsWorld::clear_debug_queries() noexcept { m_impl->debug_queries.clear(); }

PhysicsStats PhysicsWorld::stats() const {
    auto stats = m_impl->stats;
    stats.contact_records_dropped = m_impl->contacts.dropped();
    stats.contacts = m_impl->touching;
    stats.temp_high_water_bytes = m_impl->temp.high_water();
    stats.temp_capacity_bytes = m_impl->temp.capacity();
    stats.overlaps = m_impl->overlapping;
    stats.queries = m_impl->queries;
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

void PhysicsWorld::prepare(const World& world, std::span<const BodyCommands* const> lists, float interval) {
    auto& impl = *m_impl;
    const auto timer = PhaseTimer(impl.stats.prepare_ms);
    auto& bodies = impl.bodies();
    // Static colliders whose committed transforms changed.
    if (impl.live_static > 0) {
        for (auto& record : impl.records) {
            if (!record.alive || record.motion != MotionType::static_body) continue;
            const auto matrix = world.world_matrix(record.entity);
            if (matrix && *matrix == record.world) continue;
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
            bodies.SetPositionAndRotation(record.body, jolt_position(pose->translation), jolt(pose->rotation),
                                          JPH::EActivation::DontActivate);
            record.world = *matrix;
        }
    }
    // The requests, list by list, in the order they were made.
    using Kind = BodyCommands::Kind;
    for (const auto* requests : lists)
    for (const auto& request : requests->m_requests) {
        if (request.kind == Kind::create || request.kind == Kind::remove) continue;
        auto* record = impl.find(std::get<EntityHandle>(request.entity));
        if (!record) continue; // checked when requested; nothing removes bodies before the commit
        const auto id = record->body;
        const auto vector = jolt(request.vector.to_float()); // forces, impulses, and velocities
        switch (request.kind) {
        case Kind::force: bodies.AddForce(id, vector); break;
        case Kind::torque: bodies.AddTorque(id, vector); break;
        case Kind::impulse: bodies.AddImpulse(id, vector); break;
        case Kind::angular_impulse: bodies.AddAngularImpulse(id, vector); break;
        case Kind::linear_velocity: bodies.SetLinearVelocity(id, vector); break;
        case Kind::angular_velocity: bodies.SetAngularVelocity(id, vector); break;
        case Kind::kinematic_target:
            bodies.MoveKinematic(id, jolt_position(request.vector), jolt(request.rotation), interval);
            record->targeted = true;
            break;
        case Kind::teleport:
            bodies.SetPositionAndRotation(id, jolt_position(request.vector), jolt(request.rotation), JPH::EActivation::Activate);
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
    if (impl.contacts.capturing()) impl.take_debug_contacts();
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
        const auto moved = maya_position(position);
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

std::vector<PhysicsEvent> PhysicsWorld::take_events(const World& world, const WorldCommands& commands, uint64_t tick) {
    auto& impl = *m_impl;
    const auto timer = PhaseTimer(impl.stats.events_ms);
    auto events = std::exchange(impl.pending, {});
    // Jolt's records, in whatever order its threads made them, become per-pair changes: how many
    // sub-shape contacts each pair gained or lost, and the added record with the lowest sub-shape IDs.
    auto& records = impl.taken;
    impl.contacts.take(records);
    auto& changes = impl.changes;
    changes.clear();
    for (const auto& record : records) {
        auto& [delta, touch] = changes[Impl::key(record.a, record.b)];
        delta += record.added ? 1 : -1;
        if (record.added && (!touch || std::pair(record.sub_a.GetValue(), record.sub_b.GetValue()) <
                                           std::pair(touch->sub_a.GetValue(), touch->sub_b.GetValue())))
            touch = &record;
    }
    for (const auto& [key, change] : changes) {
        const auto [delta, touch] = change;
        const auto* a = impl.by_id(key.first);
        const auto* b = impl.by_id(key.second);
        if (!a || !b) continue; // a removed body's contact: its pair already ended
        auto found = impl.pairs.find(key);
        if (found == impl.pairs.end()) {
            if (delta <= 0 || !touch) continue;
            impl.add_pair(key, Impl::Pair{0, false, touch->sensor});
            found = impl.pairs.find(key);
        }
        auto& pair = found->second;
        const auto before = pair.touching;
        pair.touching += delta;
        if (pair.touching > 0 && before <= 0 && !pair.dormant) events.push_back(impl.event(true, pair, *a, *b, touch));
        if (pair.touching > 0) {
            pair.dormant = false;
            impl.apart.erase(key);
        } else {
            impl.apart.insert(key);
        }
    }
    // Pairs no longer touching: held while a moving body sleeps, ended once both are awake and apart.
    const auto asleep = [&](const Impl::Record& record) {
        return record.motion != MotionType::static_body && !impl.bodies().IsActive(record.body);
    };
    for (const auto key : std::vector(impl.apart.begin(), impl.apart.end())) {
        auto& pair = impl.pairs.at(key);
        const auto* a = impl.by_id(key.first);
        const auto* b = impl.by_id(key.second);
        if (!a || !b) continue;
        if (asleep(*a) || asleep(*b)) {
            pair.dormant = true;
            continue;
        }
        events.push_back(impl.event(false, pair, *a, *b, nullptr));
        impl.erase_pair(key);
    }
    // Recipients destroyed in this tick's batch: looked up only when there is something to deliver.
    auto destroyed = std::unordered_set<uint32_t>{};
    if (!events.empty())
        for (const auto& staged : commands.staged())
            if (staged.kind == WorldCommands::Kind::destroy)
                if (const auto* entity = std::get_if<EntityHandle>(&staged.target)) destroyed.insert(entity->slot);
    return impl.finish(std::move(events), tick, world, destroyed);
}

std::vector<PhysicsEvent> PhysicsWorld::end_contacts(const World& world, uint64_t tick) {
    auto& impl = *m_impl;
    auto events = std::exchange(impl.pending, {});
    for (const auto& [key, pair] : impl.pairs) {
        const auto* a = impl.by_id(key.first);
        const auto* b = impl.by_id(key.second);
        if (!a || !b) continue;
        auto ended = impl.event(false, pair, *a, *b, nullptr);
        ended.removed = true;
        events.push_back(ended);
    }
    impl.pairs.clear();
    impl.pairs_of.clear();
    impl.apart.clear();
    impl.contacts.take(impl.taken);
    return impl.finish(std::move(events), tick, world, {});
}

std::optional<QueryHit> PhysicsWorld::raycast_nearest(math::DVec3 origin, math::Vec3 direction, float distance,
                                                      const QueryFilter& filter) const {
    const auto& impl = *m_impl;
    const auto along = unit(direction);
    if (!along || !finite(origin.to_float())) throw std::invalid_argument("A raycast needs a finite origin and a finite, nonzero direction");
    if (!(std::isfinite(distance) && distance >= 0.0f)) throw std::invalid_argument("A raycast's distance must be finite and not negative");
    auto logged = PhysicsDebugQuery{PhysicsQueryKind::raycast, origin, {}, *along, distance, std::nullopt, {}};
    if (distance == 0.0f) {
        impl.log_query(std::move(logged), {});
        return std::nullopt;
    }
    // The nearest hit, and on a tie the lower EntityId, as raycast orders them. Hits as near as the
    // best so far must still arrive, so the early out sits just past it.
    class Nearest final : public JPH::CastRayCollector {
    public:
        explicit Nearest(const Impl& impl) : m_impl(impl) {}
        void AddHit(const JPH::RayCastResult& hit) override {
            const auto* record = m_impl.by_id(hit.mBodyID.GetIndexAndSequenceNumber());
            if (!record) return;
            if (best && (hit.mFraction > best->mFraction || (hit.mFraction == best->mFraction && !(record->id < best_id)))) return;
            best = hit;
            best_id = record->id;
            UpdateEarlyOutFraction(std::nextafter(hit.mFraction, 2.0f));
        }
        std::optional<JPH::RayCastResult> best;
        EntityId best_id;

    private:
        const Impl& m_impl;
    } collector(impl);
    const auto ray = JPH::RRayCast(jolt_position(origin), jolt(*along * distance));
    const auto groups = QueryGroups(filter.groups);
    const auto bodies = QueryBodies(filter.sensors, impl.ignored(filter));
    impl.query().CastRay(ray, JPH::RayCastSettings(), collector, JPH::BroadPhaseLayerFilter(), groups, bodies);
    auto result = std::optional<QueryHit>{};
    if (collector.best) {
        const auto& hit = *collector.best;
        const auto* record = impl.by_id(hit.mBodyID.GetIndexAndSequenceNumber());
        result = QueryHit{record->entity, record->id};
        result->distance = hit.mFraction * distance;
        result->point = origin + *along * result->distance;
        const auto lock = JPH::BodyLockRead(impl.system.GetBodyLockInterfaceNoLock(), hit.mBodyID);
        if (lock.Succeeded()) result->normal = maya_vector(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, jolt_position(result->point)));
    }
    impl.log_query(std::move(logged), result ? std::vector<QueryHit>{*result} : std::vector<QueryHit>{});
    return result;
}

std::vector<QueryHit> PhysicsWorld::raycast(math::DVec3 origin, math::Vec3 direction, float distance, const QueryFilter& filter) const {
    const auto& impl = *m_impl;
    const auto along = unit(direction);
    if (!along || !finite(origin.to_float())) throw std::invalid_argument("A raycast needs a finite origin and a finite, nonzero direction");
    if (!(std::isfinite(distance) && distance >= 0.0f)) throw std::invalid_argument("A raycast's distance must be finite and not negative");
    auto logged = PhysicsDebugQuery{PhysicsQueryKind::raycast, origin, {}, *along, distance, std::nullopt, {}};
    if (distance == 0.0f) {
        impl.log_query(std::move(logged), {});
        return {};
    }
    const auto ray = JPH::RRayCast(jolt_position(origin), jolt(*along * distance));
    auto collector = JPH::AllHitCollisionCollector<JPH::CastRayCollector>{};
    const auto groups = QueryGroups(filter.groups);
    const auto bodies = QueryBodies(filter.sensors, impl.ignored(filter));
    impl.query().CastRay(ray, JPH::RayCastSettings(), collector, JPH::BroadPhaseLayerFilter(), groups, bodies);
    // A ray hit has no body ID named mBodyID2: adapt it to the shared collector reader.
    struct Hit {
        JPH::BodyID mBodyID2;
        JPH::SubShapeID sub;
        float fraction;
    };
    struct Hits {
        std::vector<Hit> mHits;
    } found;
    for (const auto& hit : collector.mHits) found.mHits.push_back({hit.mBodyID, hit.mSubShapeID2, hit.mFraction});
    auto results = impl.hits(found, [](const Hit& hit) { return hit.fraction; }, [&](const Hit& hit) {
        auto result = QueryHit{};
        result.distance = hit.fraction * distance;
        result.point = origin + *along * result.distance;
        const auto lock = JPH::BodyLockRead(impl.system.GetBodyLockInterfaceNoLock(), hit.mBodyID2);
        if (lock.Succeeded()) result.normal = maya_vector(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.sub, jolt_position(result.point)));
        return result;
    });
    impl.log_query(std::move(logged), results);
    return results;
}

std::vector<QueryHit> PhysicsWorld::shape_cast(const ShapeGeometry& shape, math::DVec3 origin, math::Quat rotation, math::Vec3 direction,
                                               float distance, const QueryFilter& filter) const {
    const auto& impl = *m_impl;
    const auto along = unit(direction);
    const auto turned = normalized(rotation);
    if (!along || !finite(origin.to_float()) || !turned)
        throw std::invalid_argument("A shape cast needs a finite origin and rotation, and a finite, nonzero direction");
    if (!(std::isfinite(distance) && distance >= 0.0f)) throw std::invalid_argument("A shape cast's distance must be finite and not negative");
    const auto collider = ColliderDesc{shape};
    if (auto reason = validate_collider(collider); !reason.empty()) throw std::invalid_argument("A shape cast's shape is invalid: " + reason);
    if (distance == 0.0f) return overlap(shape, origin, rotation, filter);
    const auto swept = make_shape({collider}, math::Vec3(1.0f), 1000.0f);
    const auto cast = JPH::RShapeCast::sFromWorldTransform(swept, JPH::Vec3::sReplicate(1.0f),
                                                           JPH::RMat44::sRotationTranslation(jolt(*turned), jolt_position(origin)),
                                                           jolt(*along * distance));
    auto settings = JPH::ShapeCastSettings();
    settings.mReturnDeepestPoint = true;
    auto collector = JPH::AllHitCollisionCollector<JPH::CastShapeCollector>{};
    const auto groups = QueryGroups(filter.groups);
    const auto bodies = QueryBodies(filter.sensors, impl.ignored(filter));
    // Contact points come back relative to the base offset, in float: the cast's own origin keeps them precise.
    impl.query().CastShape(cast, settings, jolt_position(origin), collector, JPH::BroadPhaseLayerFilter(), groups, bodies);
    auto results = impl.hits(collector, [](const JPH::ShapeCastResult& hit) { return hit.mFraction; }, [&](const JPH::ShapeCastResult& hit) {
        auto result = QueryHit{};
        result.distance = hit.mFraction * distance;
        result.point = origin + maya_vector(hit.mContactPointOn2);
        if (const auto axis = maya_vector(hit.mPenetrationAxis); const auto normal = unit(axis)) result.normal = *normal * -1.0f;
        return result;
    });
    impl.log_query({PhysicsQueryKind::shape_cast, origin, *turned, *along, distance, shape, {}}, results);
    return results;
}

std::vector<QueryHit> PhysicsWorld::overlap(const ShapeGeometry& shape, math::DVec3 position, math::Quat rotation,
                                            const QueryFilter& filter) const {
    const auto& impl = *m_impl;
    const auto turned = normalized(rotation);
    if (!finite(position.to_float()) || !turned) throw std::invalid_argument("An overlap needs a finite position and rotation");
    const auto collider = ColliderDesc{shape};
    if (auto reason = validate_collider(collider); !reason.empty()) throw std::invalid_argument("An overlap's shape is invalid: " + reason);
    const auto probe = make_shape({collider}, math::Vec3(1.0f), 1000.0f);
    auto collector = JPH::AllHitCollisionCollector<JPH::CollideShapeCollector>{};
    const auto groups = QueryGroups(filter.groups);
    const auto bodies = QueryBodies(filter.sensors, impl.ignored(filter));
    impl.query().CollideShape(probe, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(jolt(*turned), jolt_position(position)),
                              JPH::CollideShapeSettings(), jolt_position(position), collector, JPH::BroadPhaseLayerFilter(), groups, bodies);
    auto results = impl.hits(collector, [](const JPH::CollideShapeResult&) { return 0.0f; }, [&](const JPH::CollideShapeResult& hit) {
        auto result = QueryHit{};
        result.point = position + maya_vector(hit.mContactPointOn2); // relative to the base offset
        if (const auto normal = unit(maya_vector(hit.mPenetrationAxis))) result.normal = *normal * -1.0f;
        return result;
    });
    impl.log_query({PhysicsQueryKind::overlap, position, *turned, {}, 0.0f, shape, {}}, results);
    return results;
}

std::string PhysicsWorld::create_bodies(const World& world, std::span<const std::pair<EntityHandle, BodyDesc>> bodies) {
    auto& impl = *m_impl;
    const auto first = impl.records.size();
    auto activate = std::vector<JPH::BodyID>{};
    auto resting = std::vector<JPH::BodyID>{};
    try {
        for (const auto& [entity, body] : bodies) {
            if (auto reason = validate_body(body); !reason.empty())
                throw std::runtime_error("cannot create a " + std::string(motion_type_name(body.motion)) + " body for " +
                                         entity_text(world, entity) + ": " + reason);
            impl.create(world, entity, body, {}, activate, resting);
        }
    } catch (const std::exception& error) {
        // None of these bodies was added to the physics system yet: destroy them and forget them.
        for (auto i = impl.records.size(); i-- > first;) {
            const auto& record = impl.records[i];
            impl.bodies().DestroyBody(record.body);
            impl.index.erase(record.entity.slot);
            impl.by_body.erase(record.body.GetIndexAndSequenceNumber());
            (record.motion == MotionType::static_body ? impl.live_static : impl.live_moving) -= 1;
            --impl.stats.bodies_created;
        }
        impl.records.resize(first);
        return error.what();
    }
    for (auto* ids : {&resting, &activate}) {
        if (ids->empty()) continue;
        const auto mode = ids == &activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
        auto state = impl.bodies().AddBodiesPrepare(ids->data(), int(ids->size()));
        impl.bodies().AddBodiesFinalize(ids->data(), int(ids->size()), state, mode);
    }
    if (activate.size() + resting.size() >= 256) impl.system.OptimizeBroadPhase();
    return {};
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
