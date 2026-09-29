#include "jolt_prototype.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterMask.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc/malloc.h>
#include <memory>
#include <mutex>
#include <tuple>

namespace maya::prototype {
namespace {

// Jolt's allocator hooks are process-wide function pointers, installed before any Jolt object
// exists. They count bytes with malloc_size, since Jolt's Free does not pass a size.
std::atomic<size_t> g_live{0};
std::atomic<size_t> g_peak{0};
std::atomic<uint64_t> g_allocations{0};

void count_allocation(void* block) {
    if (!block)
        return;
    const auto live = g_live.fetch_add(malloc_size(block), std::memory_order_relaxed) + malloc_size(block);
    auto peak = g_peak.load(std::memory_order_relaxed);
    while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    g_allocations.fetch_add(1, std::memory_order_relaxed);
}

void count_free(void* block) {
    if (block)
        g_live.fetch_sub(malloc_size(block), std::memory_order_relaxed);
}

void* allocate(size_t size) {
    auto* block = std::malloc(size);
    count_allocation(block);
    return block;
}

void* reallocate(void* block, size_t, size_t new_size) {
    count_free(block);
    auto* moved = std::realloc(block, new_size);
    count_allocation(moved ? moved : block);
    return moved;
}

void release(void* block) {
    count_free(block);
    std::free(block);
}

void* allocate_aligned(size_t size, size_t alignment) {
    void* block = nullptr;
    if (posix_memalign(&block, std::max(alignment, sizeof(void*)), size) != 0)
        return nullptr;
    count_allocation(block);
    return block;
}

void trace(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fprintf(stderr, "[Jolt] ");
    std::vfprintf(stderr, format, arguments);
    std::fprintf(stderr, "\n");
    va_end(arguments);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    std::fprintf(stderr, "[Jolt] %s:%u: assertion (%s) failed %s\n", file, line, expression, message ? message : "");
    return true; // break
}
#endif

void initialize_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::Allocate = allocate;
        JPH::Reallocate = reallocate;
        JPH::Free = release;
        JPH::AlignedAllocate = allocate_aligned;
        JPH::AlignedFree = release;
        JPH::Trace = trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assert_failed;)
        // Kept for the life of the process: Jolt's type registry is global, not per PhysicsSystem.
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

// Collision groups (one bit each) and the groups each one collides with. A pair collides only when
// each one's group is in the other's mask.
constexpr uint32_t static_group = 1u << 0;
constexpr uint32_t dynamic_group = 1u << 1;
constexpr uint32_t ghost_group = 1u << 2;

JPH::ObjectLayer layer(uint32_t group, uint32_t mask) {
    return JPH::ObjectLayerPairFilterMask::sGetObjectLayer(group, mask);
}

// Contact callbacks run on Jolt's worker threads in no fixed order. They only append; the host
// sorts the list after the step.
class ContactRecorder final : public JPH::ContactListener {
public:
    uint64_t tick = 0;
    std::mutex mutex;
    std::vector<ContactStart> contacts;

    void OnContactAdded(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold&,
                        JPH::ContactSettings&) override {
        auto a = uint32_t(first.GetUserData()), b = uint32_t(second.GetUserData());
        if (a > b)
            std::swap(a, b);
        const auto lock = std::scoped_lock(mutex);
        contacts.push_back({tick, a, b});
    }
};

struct Scene {
    JPH::BroadPhaseLayerInterfaceMask broad_phase_layers{2};
    JPH::ObjectVsBroadPhaseLayerFilterMask object_vs_broad_phase{broad_phase_layers};
    JPH::ObjectLayerPairFilterMask object_pairs;
    ContactRecorder contacts;
    JPH::PhysicsSystem system;
    std::vector<JPH::BodyID> boxes; // dynamic bodies in creation order

    explicit Scene(uint32_t max_bodies) {
        broad_phase_layers.ConfigureLayer(JPH::BroadPhaseLayer(0), static_group, 0);
        broad_phase_layers.ConfigureLayer(JPH::BroadPhaseLayer(1), dynamic_group | ghost_group, 0);
        system.Init(max_bodies, 0, 65536, 20480, broad_phase_layers, object_vs_broad_phase, object_pairs);
        system.SetContactListener(&contacts);
    }
};

void hash_bytes(uint64_t& hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

Pose pose_of(const JPH::BodyInterface& bodies, JPH::BodyID id) {
    JPH::RVec3 position;
    JPH::Quat rotation;
    bodies.GetPositionAndRotation(id, position, rotation);
    return {{float(position.GetX()), float(position.GetY()), float(position.GetZ())},
            {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()}};
}

void hash_poses(uint64_t& hash, const Scene& scene) {
    const auto& bodies = scene.system.GetBodyInterfaceNoLock();
    for (const auto id : scene.boxes) {
        const auto pose = pose_of(bodies, id);
        hash_bytes(hash, &pose, sizeof(pose));
    }
}

} // namespace

BoxDropResult run_box_drop(const BoxDrop& drop) {
    initialize_jolt();
    auto temp = JPH::TempAllocatorImpl(16u << 20);
    auto jobs = JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, int(drop.worker_threads));
    auto scene = std::make_unique<Scene>(drop.boxes + 2);
    auto& bodies = scene->system.GetBodyInterface();

    auto floor = JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(50.0f, 0.5f, 50.0f)), JPH::RVec3(0.0f, -0.5f, 0.0f),
                                           JPH::Quat::sIdentity(), JPH::EMotionType::Static, layer(static_group, dynamic_group));
    floor.mUserData = drop.boxes + 1;
    bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);

    const JPH::RefConst<JPH::Shape> cube = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    const auto tilt = JPH::Quat::sRotation(JPH::Vec3(1.0f, 0.0f, 1.0f).Normalized(), 0.3f);
    const auto add_box = [&](uint32_t index, JPH::RVec3 position, JPH::ObjectLayer object_layer) {
        auto settings = JPH::BodyCreationSettings(cube, position, tilt, JPH::EMotionType::Dynamic, object_layer);
        settings.mUserData = index;
        scene->boxes.push_back(bodies.CreateAndAddBody(settings, JPH::EActivation::Activate));
    };
    for (uint32_t i = 0; i < drop.boxes; ++i) {
        const auto slot = i % 100, level = i / 100;
        const auto x = drop.boxes == 1 ? 0.0f : (float(slot % 10) - 4.5f) * 1.5f;
        const auto z = drop.boxes == 1 ? 0.0f : (float(slot / 10) - 4.5f) * 1.5f;
        add_box(i, JPH::RVec3(x, drop.drop_height + 0.5f + float(level) * 1.5f, z),
                layer(dynamic_group, static_group | dynamic_group));
    }
    if (drop.ghost)
        add_box(drop.boxes, JPH::RVec3(20.0f, drop.drop_height + 0.5f, 20.0f), layer(ghost_group, static_group));
    scene->system.OptimizeBroadPhase();

    const auto interval = 1.0f / float(drop.ticks_per_second);
    auto result = BoxDropResult{};
    auto saved = JPH::StateRecorderImpl();
    uint64_t hash = 14695981039346656037ull, replay_hash = hash;
    for (uint32_t tick = 0; tick < drop.ticks; ++tick) {
        if (drop.replay_from && tick == drop.replay_from)
            scene->system.SaveState(saved);
        scene->contacts.tick = tick;
        const auto before = std::chrono::steady_clock::now();
        result.update_errors |= scene->system.Update(interval, 1, &temp, &jobs) != JPH::EPhysicsUpdateError::None;
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
        result.step_ms_mean += ms;
        result.step_ms_max = std::max(result.step_ms_max, ms);
        hash_poses(hash, *scene);
        if (drop.replay_from && tick >= drop.replay_from)
            hash_poses(replay_hash, *scene);
    }
    result.step_ms_mean /= std::max(drop.ticks, 1u);
    result.trace_hash = hash;
    for (const auto id : scene->boxes) {
        result.final_poses.push_back(pose_of(bodies, id));
        result.awake_at_end += bodies.IsActive(id) ? 1 : 0;
    }
    result.contacts = scene->contacts.contacts;
    std::sort(result.contacts.begin(), result.contacts.end(), [](const auto& a, const auto& b) {
        return std::tie(a.tick, a.first, a.second) < std::tie(b.tick, b.first, b.second);
    });

    if (drop.replay_from && drop.replay_from < drop.ticks) {
        result.replay_checked = true;
        result.state_bytes = saved.GetData().size();
        saved.Rewind();
        auto again = uint64_t{14695981039346656037ull};
        if (scene->system.RestoreState(saved)) {
            for (auto tick = drop.replay_from; tick < drop.ticks; ++tick) {
                scene->system.Update(interval, 1, &temp, &jobs);
                hash_poses(again, *scene);
            }
            result.replay_matches = again == replay_hash;
        }
    }
    return result; // destroying the PhysicsSystem destroys its bodies
}

AllocationCounters jolt_allocations() {
    return {g_live.load(), g_peak.load(), g_allocations.load()};
}

void reset_jolt_peak() {
    g_peak.store(g_live.load());
}

} // namespace maya::prototype
