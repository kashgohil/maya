#include "jolt_runtime.hpp"
#include "maya/jobs/jobs.hpp"
#include "maya/physics/physics.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
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

int default_workers() noexcept { return std::min(job_system().workers(JobTier::frame), 7); }

// Jolt on the process's job system (docs/jobs.md#physics): each Jolt job becomes a frame-tier job when
// its dependencies are met. Jolt splits a step into at most GetMaxConcurrency() parallel jobs, which is
// how the configured worker count still limits physics.
class SharedPoolJobs final : public JPH::JobSystemWithBarrier {
public:
    SharedPoolJobs() : JobSystemWithBarrier(JPH::cMaxPhysicsBarriers) { m_jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs); }
    void set_workers(int workers) noexcept { m_concurrency.store(workers + 1, std::memory_order_relaxed); }
    int GetMaxConcurrency() const override { return m_concurrency.load(std::memory_order_relaxed); }
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencies) override {
        auto index = m_jobs.ConstructObject(name, color, this, function, dependencies);
        while (index == AvailableJobs::cInvalidObjectIndex) { // every slot in use: let running jobs finish
            std::this_thread::yield();
            index = m_jobs.ConstructObject(name, color, this, function, dependencies);
        }
        auto* job = &m_jobs.Get(index);
        auto handle = JobHandle(job);
        if (dependencies == 0) QueueJob(job);
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        job_system().submit(JobTier::frame, [job](JobContext&) {
            job->Execute();
            job->Release();
        });
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
    }
    void FreeJob(Job* job) override { m_jobs.DestructObject(job); }

private:
    using AvailableJobs = JPH::FixedSizeFreeList<Job>;
    AvailableJobs m_jobs;
    std::atomic<int> m_concurrency{1};
};

std::atomic<int> g_workers{-1}; // requested; -1 is the default
std::unique_ptr<SharedPoolJobs> g_pool;
std::unique_ptr<JPH::JobSystemSingleThreaded> g_single; // zero workers: the stepping thread runs every job

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
    if (workers == 0) {
        if (!g_single) g_single = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        return *g_single;
    }
    if (!g_pool) g_pool = std::make_unique<SharedPoolJobs>();
    g_pool->set_workers(std::min(workers, job_system().workers(JobTier::frame)));
    return *g_pool;
}

} // namespace detail

std::string physics_configuration() {
    auto text = "Jolt " + std::to_string(JPH_VERSION_MAJOR) + "." + std::to_string(JPH_VERSION_MINOR) + "." +
                std::to_string(JPH_VERSION_PATCH);
#ifdef JPH_DOUBLE_PRECISION
    text += ", double precision";
#else
    text += ", single precision";
#endif
    text += ", " + std::to_string(sizeof(JPH::ObjectLayer) * 8) + "-bit object layers";
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
    text += ", cross-platform deterministic";
#endif
#ifdef JPH_ENABLE_ASSERTS
    text += ", asserts";
#endif
    return text;
}

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
    return requested < 0 ? default_workers() : std::min(requested, job_system().workers(JobTier::frame));
}

} // namespace maya
