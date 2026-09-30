#pragma once

// Jolt's process-wide state, private to MayaPhysics: this header includes Jolt.
#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystem.h>

namespace maya::detail {

/// Installs Jolt's allocator, trace, and assert hooks, then its factory and types, once per process.
/// They are kept until exit: Jolt's type registry is global, not per world.
void initialize_jolt();

/// The process's one job pool, created on first use with the configured worker count.
JPH::JobSystem& physics_jobs();

} // namespace maya::detail
