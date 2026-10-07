#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace maya {
// The process's job system (docs/jobs.md; decided in docs/architecture/world-scale-decision.md#jobs-a-two-tier-pool).
// Two tiers of workers: frame work (physics, animation, extraction, parallel-fors) and background work
// (reading, decoding, cooking, generation). The tiers' threads run at different quality of service, so
// the kernel preempts background work when frame work arrives. Jobs never touch a World, never mutate
// the asset registry, and never encode GPU commands; results reach the owner thread through a
// CompletionQueue.

enum class JobTier : uint8_t { frame, background };
enum class JobStatus : uint8_t { pending, running, succeeded, failed, cancelled };
const char* job_status_name(JobStatus status) noexcept;

namespace detail {
struct JobState;
class JobRuntime;
struct ScopeJobs;
struct CompletionState;
} // namespace detail

/// What a running job sees: its cancellation, and a way to fail without throwing.
class JobContext {
public:
    /// True once the job or its system was asked to stop. Check it before each step of long work.
    bool cancelled() const noexcept;
    /// Ends the job as failed, with this message, when it returns.
    void fail(std::string message);

private:
    friend class detail::JobRuntime;
    explicit JobContext(detail::JobState& state) noexcept : m_state(state) {}
    detail::JobState& m_state;
};

using JobFunction = std::function<void(JobContext&)>;

/// A submitted job. Copies share the job; dropping every copy neither cancels nor waits for it.
class JobHandle {
public:
    JobHandle() = default;
    explicit operator bool() const noexcept { return static_cast<bool>(m_state); }
    JobStatus status() const noexcept;
    /// Succeeded, failed, or cancelled.
    bool done() const noexcept;
    /// For a failed job, its message; for a cancelled one, why. Empty otherwise.
    std::string error() const;
    /// Asks the job to stop. A job that has not started never runs; a running one sees
    /// JobContext::cancelled(). Either way it ends cancelled, never succeeded.
    void cancel() const noexcept;
    /// Blocks until the job is done. On a worker thread it runs other jobs of its tier meanwhile.
    JobStatus wait() const;

private:
    friend class detail::JobRuntime;
    explicit JobHandle(std::shared_ptr<detail::JobState> state) noexcept : m_state(std::move(state)) {}
    std::shared_ptr<detail::JobState> m_state;
};

struct JobSystemConfig {
    int frame_workers = -1;      // −1: one less than the hardware threads (at least 1)
    int background_workers = -1; // −1: the same
};

struct JobTierStats {
    int workers = 0;
    uint64_t submitted = 0, succeeded = 0, failed = 0, cancelled = 0;
    double busy_ms = 0.0; // time workers (and helping callers) spent in this tier's jobs
    size_t queued = 0;      // waiting to run now
    size_t queued_peak = 0; // since the system started or reset_peaks()
};
struct JobStats {
    JobTierStats frame, background;
    // Completion queues, across the process.
    uint64_t completions_applied = 0, completions_discarded = 0;
    double drain_ms = 0.0;
};

class JobSystem {
public:
    explicit JobSystem(JobSystemConfig config = {});
    /// Cancels every job, lets running ones return, and joins the workers.
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Runs `function` on `tier` once every job in `after` is done. If one of them failed or was
    /// cancelled, the job does not run: it ends failed or cancelled, naming the prerequisite.
    /// Exceptions from `function` end the job as failed with their message. Thread-safe.
    JobHandle submit(JobTier tier, JobFunction function, std::span<const JobHandle> after = {});
    /// Calls `body(begin, end)` over [0, count) in chunks of `grain`, on `tier`'s workers and the calling
    /// thread, and returns when every chunk is done. The first exception a chunk throws is rethrown here
    /// after the others finish.
    void parallel_for(JobTier tier, uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body);

    int workers(JobTier tier) const noexcept;
    JobStats stats() const;
    void reset_peaks() noexcept;

private:
    std::unique_ptr<detail::JobRuntime> m_runtime;
};

/// The process's job system, created on first use and kept until exit. Owners whose jobs refer to them
/// cancel and wait for those jobs (a JobScope does it) before they are destroyed.
JobSystem& job_system();
/// Sizes the process's job system; only before its first use (returns false after).
bool configure_job_system(JobSystemConfig config);

/// Jobs that belong to one owner: destroying the scope cancels them and waits, so no job outlives what it
/// refers to.
class JobScope {
public:
    explicit JobScope(JobSystem& system = job_system());
    ~JobScope();
    JobScope(const JobScope&) = delete;
    JobScope& operator=(const JobScope&) = delete;
    JobHandle submit(JobTier tier, JobFunction function, std::span<const JobHandle> after = {});
    void cancel() noexcept;
    /// Waits for every job submitted so far, and forgets the finished ones.
    void wait();
    size_t pending() const;

private:
    JobSystem& m_system;
    std::unique_ptr<detail::ScopeJobs> m_jobs;
};

/// Results for the owner thread. Jobs post to a Sink; the owner drains the queue in its finalize phase
/// under a time budget. Each completion names its request and generation; the owner's `current` check
/// discards stale ones (a request replaced, cancelled, or whose world closed) without applying them.
class CompletionQueue {
public:
    CompletionQueue();
    /// Discards what is pending; later posts through old sinks are discarded too.
    ~CompletionQueue();
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    class Sink {
    public:
        Sink() = default;
        /// Thread-safe. After the queue is gone, the completion is dropped without running.
        void post(uint64_t request, uint64_t generation, std::function<void()> apply) const;

    private:
        friend class CompletionQueue;
        std::weak_ptr<detail::CompletionState> m_queue;
    };
    Sink sink() const;

    struct Drained {
        size_t applied = 0, discarded = 0, remaining = 0;
        double ms = 0.0;
    };
    /// On the owner thread: applies completions in the order they were posted until `budget` is spent
    /// (always at least one), skipping those `current(request, generation)` rejects.
    Drained drain(std::chrono::microseconds budget, const std::function<bool(uint64_t request, uint64_t generation)>& current = {});
    size_t pending() const;

private:
    std::shared_ptr<detail::CompletionState> m_state;
};
} // namespace maya
