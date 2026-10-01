#include "maya/simulation/physics_debug.hpp"
#include "maya/world/spatial.hpp"

namespace maya {
namespace {

constexpr float contact_size = 0.08f; // metres across a contact point's cross
constexpr float normal_length = 0.3f;
constexpr float hit_size = 0.1f;

/// Motion type first: static, kinematic (dimmer while asleep), or dynamic as active or sleeping.
const DebugColor& state_color(MotionType motion, bool sleeping) noexcept {
    if (motion == MotionType::static_body) return physics_debug_color::static_body;
    if (motion == MotionType::kinematic) return sleeping ? physics_debug_color::kinematic_sleeping : physics_debug_color::kinematic;
    return sleeping ? physics_debug_color::sleeping : physics_debug_color::active;
}

/// The color a body's outlines take, or none when the options leave them out.
const DebugColor* body_color(const PhysicsDebugOptions& options, bool sensor, uint8_t group, MotionType motion, bool sleeping) {
    if ((options.groups >> group & 1u) == 0) return nullptr;
    if (sensor) return options.has(PhysicsDebugCategory::triggers) ? &physics_debug_color::trigger : nullptr;
    if (options.has(PhysicsDebugCategory::body_state)) return &state_color(motion, sleeping);
    return options.has(PhysicsDebugCategory::colliders) ? &physics_debug_color::collider : nullptr;
}

void debug_shape(DebugDraw& out, const math::Mat4& world, const ShapeGeometry& shape, const DebugColor& color) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BoxShape>) {
            out.box(world, value.half_extents, color);
        } else if constexpr (std::is_same_v<T, SphereShape>) {
            out.sphere(world, value.radius, color);
        } else {
            // The caps stay round: the scale goes into the radius (X) and the straight section (Y).
            const auto pose = decompose_transform(world);
            if (!pose) return;
            auto rigid = *pose;
            rigid.scale = math::Vec3(1.0f);
            out.capsule(local_matrix(rigid), value.radius * pose->scale.x, value.half_height * pose->scale.y, color);
        }
    }, shape);
}

math::Mat4 placement(const math::Vec3& offset, const math::Quat& rotation, const math::Vec3& scale) {
    auto local = TransformComponent{};
    local.translation = offset;
    local.rotation = rotation;
    local.scale = scale;
    return local_matrix(local);
}

} // namespace

void debug_collider(DebugDraw& out, const math::Mat4& body_world, const ColliderDesc& collider, const DebugColor& color) {
    debug_shape(out, body_world * placement(collider.offset, collider.rotation, collider.scale), collider.shape, color);
}

void authored_physics_debug(const World& world, const PhysicsDebugOptions& options, DebugDraw& out) {
    if (!options.any()) return;
    world.for_each<ColliderComponent>([&](EntityHandle entity, const ColliderComponent& collider) {
        if (collider.group < 0 || collider.group >= int32_t(collision_group_count)) return;
        // Authored motion: the nearest rigid body at or above the entity, else static.
        auto motion = MotionType::static_body;
        for (auto at = std::optional(entity); at; at = world.parent(*at)) {
            auto found = false;
            world.with<RigidBodyComponent>(*at, [&](const RigidBodyComponent& body) {
                motion = body.motion == BodyMotion::kinematic ? MotionType::kinematic : MotionType::dynamic;
                found = true;
            });
            if (found) break;
        }
        const auto* color = body_color(options, collider.sensor, uint8_t(collider.group), motion, false);
        const auto matrix = color ? world.world_matrix(entity) : std::nullopt;
        if (!matrix) return;
        auto shape = ShapeGeometry{BoxShape{collider.half_extents}};
        if (collider.shape == ColliderShape::sphere) shape = SphereShape{collider.radius};
        if (collider.shape == ColliderShape::capsule) shape = CapsuleShape{collider.radius, collider.half_height};
        debug_collider(out, *matrix, ColliderDesc{shape, collider.offset, collider.rotation}, *color);
    });
}

void play_physics_debug(const World& world, const PhysicsWorld& physics, const PhysicsDebugOptions& options,
                        const PresentationPoses* poses, DebugDraw& out) {
    if (!options.any()) return;
    const auto outlines = options.has(PhysicsDebugCategory::colliders) || options.has(PhysicsDebugCategory::body_state) ||
                          options.has(PhysicsDebugCategory::triggers);
    if (outlines)
        physics.for_each_debug_body([&](const PhysicsDebugBody& body) {
            const auto* color = body_color(options, body.sensor, body.group, body.motion, body.sleeping);
            if (!color || !world.alive(body.entity)) return;
            // Where the entity is shown, so outlines stay on meshes between ticks.
            const auto matrix = poses ? poses->world_matrix(world, body.entity) : world.world_matrix(body.entity);
            if (!matrix) return;
            for (const auto& collider : body.colliders) debug_collider(out, *matrix, collider, *color);
        });
    if (options.has(PhysicsDebugCategory::contacts))
        for (const auto& contact : physics.debug_contacts()) {
            if ((contact.groups & options.groups) == 0) continue;
            out.cross(contact.point, contact_size, physics_debug_color::contact);
            out.arrow(contact.point, contact.point + contact.normal * normal_length, physics_debug_color::normal);
        }
    if (options.has(PhysicsDebugCategory::queries))
        for (const auto& query : physics.debug_queries()) {
            const auto end = query.origin + query.direction * query.distance;
            if (query.kind == PhysicsQueryKind::raycast) {
                out.line(query.origin, end, physics_debug_color::query);
            } else if (query.shape) {
                debug_shape(out, placement(query.origin, query.rotation, math::Vec3(1.0f)), *query.shape, physics_debug_color::query);
                if (query.kind == PhysicsQueryKind::shape_cast) {
                    out.line(query.origin, end, physics_debug_color::query);
                    debug_shape(out, placement(end, query.rotation, math::Vec3(1.0f)), *query.shape, physics_debug_color::query);
                }
            }
            for (const auto& hit : query.hits) out.cross(hit, hit_size, physics_debug_color::query_hit);
        }
}

} // namespace maya
