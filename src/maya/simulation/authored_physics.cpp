#include "maya/simulation/authored_physics.hpp"
#include <cmath>
#include <sstream>

namespace maya {
namespace {

std::string entity_text(const World& world, EntityHandle entity) {
    auto text = std::ostringstream{};
    text << "entity ";
    if (const auto id = world.persistent_id(entity)) text << std::hex << id->high << ' ' << id->low;
    world.with<NameComponent>(entity, [&](const NameComponent& name) {
        if (!name.value.empty()) text << " \"" << name.value << '"';
    });
    return text.str();
}

template<class T> std::optional<T> component(const World& world, EntityHandle entity) {
    auto value = std::optional<T>{};
    world.with<T>(entity, [&](const T& stored) { value = stored; });
    return value;
}

bool near(float a, float b) noexcept {
    return std::abs(a - b) <= spatial_tolerance * std::max({1.0f, std::abs(a), std::abs(b)});
}

ShapeGeometry geometry(const ColliderComponent& collider) {
    switch (collider.shape) {
    case ColliderShape::sphere: return SphereShape{collider.radius};
    case ColliderShape::capsule: return CapsuleShape{collider.radius, collider.half_height};
    case ColliderShape::box: break;
    }
    return BoxShape{collider.half_extents};
}

class Builder {
public:
    explicit Builder(const World& world) : m_world(world) {}

    /// A body for `entity`, which has a rigid body: its own collider and those below it.
    std::optional<std::string> moving(EntityHandle entity, const RigidBodyComponent& rigid, BodyDesc& body) {
        body.motion = rigid.motion == BodyMotion::kinematic ? MotionType::kinematic : MotionType::dynamic;
        body.mass = rigid.mass;
        body.density = rigid.density;
        body.linear_damping = rigid.linear_damping;
        body.angular_damping = rigid.angular_damping;
        body.gravity_factor = rigid.gravity_factor;
        body.linear_velocity = rigid.linear_velocity;
        body.angular_velocity = rigid.angular_velocity;
        body.colliders.clear();
        m_first.reset();
        if (const auto own = component<ColliderComponent>(m_world, entity))
            if (auto error = add(entity, *own, ColliderDesc{geometry(*own), own->offset, own->rotation}, body)) return error;
        const auto inverse = m_world.world_matrix(entity) ? inverse_affine(*m_world.world_matrix(entity)) : std::nullopt;
        if (!inverse) return entity_text(m_world, entity) + ": its world transform cannot be inverted";
        if (auto error = below(entity, entity, *inverse, body)) return error;
        if (body.colliders.empty())
            return entity_text(m_world, entity) + ": a rigid body needs a collider on its entity or on an entity below it";
        return std::nullopt;
    }

    /// A static body for `entity`, which has a collider and no rigid body on it or above it.
    std::optional<std::string> fixed(EntityHandle entity, const ColliderComponent& collider, BodyDesc& body) {
        body.motion = MotionType::static_body;
        body.colliders.clear();
        m_first.reset();
        return add(entity, collider, ColliderDesc{geometry(collider), collider.offset, collider.rotation}, body);
    }

private:
    std::optional<std::string> add(EntityHandle entity, const ColliderComponent& collider, ColliderDesc desc, BodyDesc& body) {
        if (!m_first) {
            m_first = entity;
            body.friction = collider.friction;
            body.restitution = collider.restitution;
            body.group = uint8_t(collider.group);
            body.mask = uint16_t(collider.mask);
            body.sensor = collider.sensor;
        } else if (body.group != uint8_t(collider.group) || body.mask != uint16_t(collider.mask) || body.sensor != collider.sensor) {
            return entity_text(m_world, entity) + ": its collider must have the same collision group, mask, and sensor " +
                   "setting as the other colliders of its body (" + entity_text(m_world, *m_first) + ")";
        }
        body.colliders.push_back(desc);
        return std::nullopt;
    }

    /// Adds the colliders on entities below `parent` that have no rigid body, placed in `body_entity`'s space.
    std::optional<std::string> below(EntityHandle body_entity, EntityHandle parent, const math::Mat4& to_body, BodyDesc& body) {
        for (const auto child : m_world.children(parent)) {
            if (m_world.has<RigidBodyComponent>(child)) continue; // its own body; creating it reports the conflict
            if (const auto collider = component<ColliderComponent>(m_world, child)) {
                const auto world = m_world.world_matrix(child);
                const auto relative = world ? decompose_transform(to_body * *world) : std::nullopt;
                if (!relative)
                    return entity_text(m_world, child) + ": its collider cannot be placed in the body of " +
                           entity_text(m_world, body_entity) + ": its transform relative to the body has shear";
                const auto& s = relative->scale;
                const auto uniform = near(s.x, s.y) && near(s.y, s.z);
                const auto& r = collider->rotation;
                if (!uniform && std::abs(std::abs(r.w) - 1.0f) > spatial_tolerance)
                    return entity_text(m_world, child) + ": a rotated collider cannot take its entity's nonuniform scale";
                auto desc = ColliderDesc{geometry(*collider)};
                desc.offset = relative->translation +
                              relative->rotation.rotate({collider->offset.x * s.x, collider->offset.y * s.y, collider->offset.z * s.z});
                desc.rotation = relative->rotation * collider->rotation;
                desc.scale = s;
                if (auto error = add(child, *collider, desc, body)) return error;
            }
            if (auto error = below(body_entity, child, to_body, body)) return error;
        }
        return std::nullopt;
    }

    const World& m_world;
    std::optional<EntityHandle> m_first; // the entity of the body's first collider
};

bool under_rigid_body(const World& world, EntityHandle entity) {
    for (auto parent = world.parent(entity); parent; parent = world.parent(*parent))
        if (world.has<RigidBodyComponent>(*parent)) return true;
    return false;
}

} // namespace

AuthoredPhysics authored_physics(const World& world, std::span<const EntityId> order, PhysicsSettings base) {
    auto result = AuthoredPhysics{{}, base, {}};
    if (world.component_count<ColliderComponent>() == 0 && world.component_count<RigidBodyComponent>() == 0 &&
        world.component_count<PhysicsSettingsComponent>() == 0)
        return result; // a scene without physics costs nothing to scan
    auto builder = Builder(world);
    auto settings_entity = std::optional<EntityHandle>{};
    for (const auto id : order) {
        const auto entity = world.find(id);
        if (!entity) continue;
        if (const auto settings = component<PhysicsSettingsComponent>(world, *entity)) {
            if (settings_entity) {
                result.error = entity_text(world, *entity) + ": a scene has at most one physics settings component; " +
                               entity_text(world, *settings_entity) + " has one too";
                return result;
            }
            settings_entity = entity;
            result.settings.gravity = settings->gravity;
        }
        auto body = BodyDesc{};
        auto error = std::optional<std::string>{};
        if (const auto rigid = component<RigidBodyComponent>(world, *entity)) {
            error = builder.moving(*entity, *rigid, body);
        } else if (const auto collider = component<ColliderComponent>(world, *entity)) {
            if (under_rigid_body(world, *entity)) continue; // part of that body's shape
            error = builder.fixed(*entity, *collider, body);
        } else {
            continue;
        }
        if (error) {
            result.error = *error;
            result.bodies.clear();
            return result;
        }
        result.bodies.emplace_back(*entity, std::move(body));
    }
    return result;
}

} // namespace maya
