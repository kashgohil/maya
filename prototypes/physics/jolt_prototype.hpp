#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Jolt Physics prototype for #1015: a falling-box scene run through a Jolt PhysicsSystem, to check
// the library in this build before MayaPhysics (#1017) is written. Nothing here is engine API.
namespace maya::prototype {

/// Boxes dropped onto a static floor, stepped at a fixed rate.
struct BoxDrop {
    uint32_t boxes = 1; // 1 m cubes in 10 x 10 layers, 1.5 m apart, the first at drop_height
    uint32_t worker_threads = 0; // Jolt job threads; the calling thread also runs jobs
    uint32_t ticks = 180;
    uint32_t ticks_per_second = 60;
    float drop_height = 5.0f; // metres above the floor's top face, for the first layer
    /// An extra box whose collision group the floor's mask excludes: it falls through the floor.
    bool ghost = false;
    /// Save the whole physics state at this tick, finish the run, then restore it and replay the
    /// remaining ticks. 0 disables the check.
    uint32_t replay_from = 0;
};

struct Pose {
    float position[3];
    float rotation[4]; // x, y, z, w
};

/// A contact that started during a tick, between bodies named by creation index.
struct ContactStart {
    uint64_t tick;
    uint32_t first, second; // first < second
    bool operator==(const ContactStart&) const = default;
};

struct BoxDropResult {
    std::vector<Pose> final_poses; // the dropped boxes in creation order, then the ghost
    uint64_t trace_hash = 0; // FNV-1a over every box's pose after every tick
    uint32_t awake_at_end = 0; // dynamic bodies not asleep after the last tick
    std::vector<ContactStart> contacts; // sorted by tick, then bodies
    double step_ms_mean = 0.0;
    double step_ms_max = 0.0;
    bool update_errors = false; // PhysicsSystem::Update reported an error on some tick
    bool replay_checked = false;
    bool replay_matches = false; // the replayed ticks gave exactly the same poses
    size_t state_bytes = 0; // the saved state's size, when replay was checked
};

/// Runs the scene. Initializes Jolt's process-wide state (allocator hooks, factory, types) once.
BoxDropResult run_box_drop(const BoxDrop& drop);

/// Bytes currently allocated through Jolt's allocator hooks, and the peak since the last reset.
struct AllocationCounters {
    size_t live_bytes = 0;
    size_t peak_bytes = 0;
    uint64_t allocations = 0;
};
AllocationCounters jolt_allocations();
void reset_jolt_peak();

} // namespace maya::prototype
