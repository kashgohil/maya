#pragma once
// A minimal Jolt setup shared by the world-scale prototypes (#1060): two broad-phase layers (static and
// moving), as MayaPhysics uses, and a helper that owns the factory and type registration.

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <memory>

namespace prototype {
inline constexpr JPH::ObjectLayer static_layer = 0;
inline constexpr JPH::ObjectLayer moving_layer = 1;

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer(JPH::uint8(layer)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return layer.GetValue() == 0 ? "static" : "moving"; }
#endif
};
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == moving_layer || broad.GetValue() == moving_layer;
    }
};
class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == moving_layer || b == moving_layer; }
};

/// Registers Jolt's types for the life of the object.
struct JoltRuntime {
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltRuntime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;
};

/// A PhysicsSystem with its layer tables, which must outlive it.
struct PhysicsScene {
    BroadPhaseLayers layers;
    ObjectVsBroadPhase object_vs_broad;
    ObjectPairs pairs;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl temp{64u << 20};

    explicit PhysicsScene(JPH::uint max_bodies = 65536) {
        system.Init(max_bodies, 0, 65536, 65536, layers, object_vs_broad, pairs);
    }
};
} // namespace prototype
