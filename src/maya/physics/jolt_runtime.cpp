#include "jolt_runtime.hpp"
#include "maya/physics/physics.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <malloc/malloc.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace maya {
namespace {

// Jolt's Free passes no size, so blocks are measured with malloc_size.
std::atomic<size_t> g_live{0};
std::atomic<size_t> g_peak{0};
std::atomic<uint64_t> g_allocations{0};

void count_allocation(void* block) noexcept {
    if (!block) return;
    const auto size = malloc_size(block);
    const auto live = g_live.fetch_add(size, std::memory_order_relaxed) + size;
    auto peak = g_peak.load(std::memory_order_relaxed);
    while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
    g_allocations.fetch_add(1, std::memory_order_relaxed);
}

void count_free(void* block) noexcept {
    if (block) g_live.fetch_sub(malloc_size(block), std::memory_order_relaxed);
}

void* allocate(size_t size) {
    auto* block = std::malloc(size);
    count_allocation(block);
    return block;
}

void* reallocate(void* block, size_t, size_t new_size) {
    const auto old_size = block ? malloc_size(block) : 0;
    auto* moved = std::realloc(block, new_size);
    if (moved) {
        g_live.fetch_sub(old_size, std::memory_order_relaxed);
        count_allocation(moved);
    }
    return moved;
}

void release(void* block) {
    count_free(block);
    std::free(block);
}

void* allocate_aligned(size_t size, size_t alignment) {
    void* block = nullptr;
    if (posix_memalign(&block, std::max(alignment, sizeof(void*)), size) != 0) return nullptr;
    count_allocation(block);
    return block;
}

void trace(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fprintf(stderr, "[Physics] ");
    std::vfprintf(stderr, format, arguments);
    std::fprintf(stderr, "\n");
    va_end(arguments);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    std::fprintf(stderr, "[Physics] %s:%u: Jolt assertion (%s) failed %s\n", file, line, expression, message ? message : "");
    return true;
}
#endif

int default_workers() noexcept {
    const auto hardware = int(std::max(1u, std::thread::hardware_concurrency()));
    return std::clamp(hardware - 1, 0, 7);
}

std::atomic<int> g_workers{-1}; // requested; -1 is the default
std::unique_ptr<JPH::JobSystemThreadPool> g_pool;
int g_pool_workers = -1; // what the pool runs

} // namespace

namespace detail {

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
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::JobSystem& physics_jobs() {
    const auto workers = physics_worker_threads();
    if (!g_pool) {
        g_pool = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);
        g_pool_workers = workers;
    } else if (g_pool_workers != workers) {
        g_pool->SetNumThreads(workers);
        g_pool_workers = workers;
    }
    return *g_pool;
}

} // namespace detail

PhysicsMemory physics_memory() noexcept {
    return {g_live.load(std::memory_order_relaxed), g_peak.load(std::memory_order_relaxed),
            g_allocations.load(std::memory_order_relaxed)};
}

void reset_physics_peak() noexcept {
    g_peak.store(g_live.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void set_physics_worker_threads(int count) {
    if (count < -1 || count > 64) throw std::invalid_argument("Physics worker threads must be -1 (default) or 0 to 64");
    g_workers.store(count, std::memory_order_relaxed);
}

int physics_worker_threads() noexcept {
    const auto requested = g_workers.load(std::memory_order_relaxed);
    return requested < 0 ? default_workers() : requested;
}

} // namespace maya
