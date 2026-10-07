#include "maya/jobs/jobs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <pthread/qos.h>
#include <string>
#include <thread>
#include <vector>

namespace maya {
namespace detail {
using Clock = std::chrono::steady_clock;

uint64_t nanoseconds_since(Clock::time_point start) noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
bool final_status(JobStatus status) noexcept {
    return status == JobStatus::succeeded || status == JobStatus::failed || status == JobStatus::cancelled;
}

struct JobState {
    JobRuntime* runtime = nullptr;
    JobTier tier = JobTier::frame;
    JobFunction function;
    std::atomic<uint8_t> status{uint8_t(JobStatus::pending)};
    std::atomic<bool> cancel_requested{false};
    std::string error; // written by the job or by finish(), before the final status is published
    bool failed_by_job = false;
    std::atomic<int> unmet{0}; // prerequisites not yet done, plus one while submitting

    std::mutex mutex; // guards what follows
    std::vector<std::shared_ptr<JobState>> dependents;
    bool finished = false;
    JobStatus blocked = JobStatus::succeeded; // the first prerequisite outcome that was not success
    std::string blocked_reason;

    JobStatus load() const noexcept { return JobStatus(status.load(std::memory_order_acquire)); }
};

struct ScopeJobs {
    std::mutex mutex;
    std::vector<JobHandle> jobs;
};

struct CompletionItem {
    uint64_t request, generation;
    std::function<void()> apply;
};
struct CompletionState {
    std::mutex mutex;
    std::deque<CompletionItem> items;
};

namespace {
// Completion queues' totals, across the process.
std::atomic<uint64_t> g_applied{0}, g_discarded{0}, g_drain_ns{0};

thread_local JobRuntime* t_runtime = nullptr;
thread_local int t_tier = -1;
thread_local int t_worker = -1;

int default_workers() noexcept { return std::max(1, int(std::thread::hardware_concurrency()) - 1); }
} // namespace

class JobRuntime {
public:
    explicit JobRuntime(JobSystemConfig config) {
        // At most 64 workers a tier: a tier's sleepers are bits of one word.
        const int counts[2] = {std::min(64, config.frame_workers < 0 ? default_workers() : std::max(1, config.frame_workers)),
                               std::min(64, config.background_workers < 0 ? default_workers() : std::max(1, config.background_workers))};
        for (int t = 0; t < 2; ++t) {
            auto& tier = m_tiers[size_t(t)];
            for (int w = 0; w < counts[t]; ++w) tier.queues.push_back(std::make_unique<Queue>());
        }
        for (int t = 0; t < 2; ++t)
            for (int w = 0; w < counts[t]; ++w) m_tiers[size_t(t)].threads.emplace_back([this, t, w] { work(t, w); });
    }

    ~JobRuntime() {
        m_stopping.store(true, std::memory_order_release);
        for (auto& tier : m_tiers) {
            tier.epoch.fetch_add(1, std::memory_order_seq_cst);
            auto sleepers = tier.sleepers.exchange(0, std::memory_order_seq_cst);
            for (; sleepers; sleepers &= sleepers - 1) signal(*tier.queues[size_t(std::countr_zero(sleepers))]);
        }
        for (auto& tier : m_tiers)
            for (auto& thread : tier.threads) thread.join();
        // Jobs queued after their tier's workers left (dependents of the last jobs) end cancelled here.
        for (auto again = true; again;) {
            again = false;
            for (int t = 0; t < 2; ++t)
                while (auto job = take(t, -1)) {
                    run(job);
                    again = true;
                }
        }
    }

    JobHandle submit(JobTier tier, JobFunction function, std::span<const JobHandle> after) {
        auto state = std::make_shared<JobState>();
        state->runtime = this;
        state->tier = tier;
        state->function = std::move(function);
        m_tiers[size_t(tier)].submitted.fetch_add(1, std::memory_order_relaxed);
        state->unmet.store(int(after.size()) + 1, std::memory_order_relaxed);
        for (const auto& handle : after) {
            if (!handle.m_state) {
                release(state);
                continue;
            }
            auto& prerequisite = *handle.m_state;
            auto lock = std::unique_lock(prerequisite.mutex);
            if (!prerequisite.finished) {
                prerequisite.dependents.push_back(state);
                continue;
            }
            lock.unlock();
            prerequisite_done(state, prerequisite);
        }
        release(state);
        return JobHandle(state);
    }

    void parallel_for(JobTier tier, uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) {
        if (count == 0) return;
        grain = std::max(grain, 1u);
        const auto chunks = (count + grain - 1) / grain;
        if (chunks == 1) {
            body(0, count);
            return;
        }
        struct Shared {
            const std::function<void(uint32_t, uint32_t)>* body;
            uint32_t count, grain, chunks;
            std::atomic<uint32_t> next{0}, remaining{0};
            std::mutex mutex;
            std::exception_ptr error;
        };
        auto shared = std::make_shared<Shared>();
        shared->body = &body;
        shared->count = count, shared->grain = grain, shared->chunks = chunks;
        shared->remaining.store(chunks, std::memory_order_relaxed);
        // Helpers claim chunks until none are left; one that starts after the last chunk was claimed
        // returns without touching `body`, so the caller may return as soon as every chunk is done.
        const auto work = [shared] {
            for (auto c = shared->next.fetch_add(1); c < shared->chunks; c = shared->next.fetch_add(1)) {
                try {
                    (*shared->body)(c * shared->grain, std::min(shared->count, (c + 1) * shared->grain));
                } catch (...) {
                    auto lock = std::lock_guard(shared->mutex);
                    if (!shared->error) shared->error = std::current_exception();
                }
                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) shared->remaining.notify_all();
            }
        };
        const auto helpers = std::min<uint32_t>(chunks - 1, uint32_t(m_tiers[size_t(tier)].queues.size()));
        for (uint32_t h = 0; h < helpers; ++h) submit(tier, [work](JobContext&) { work(); }, {});
        const auto start = Clock::now();
        work();
        for (auto left = shared->remaining.load(std::memory_order_acquire); left != 0; left = shared->remaining.load(std::memory_order_acquire))
            shared->remaining.wait(left, std::memory_order_acquire);
        if (t_runtime != this) m_tiers[size_t(tier)].busy_ns.fetch_add(nanoseconds_since(start), std::memory_order_relaxed);
        if (shared->error) std::rethrow_exception(shared->error);
    }

    JobStatus wait(JobState& state) {
        if (t_runtime == this && t_tier >= 0) {
            // A worker waiting on a job: run others of its tier meanwhile, so waiting never starves the pool.
            for (auto status = state.load(); !final_status(status); status = state.load()) {
                if (auto job = take(t_tier, t_worker)) run(job);
                else std::this_thread::yield();
            }
            return state.load();
        }
        for (auto status = state.load(); !final_status(status); status = state.load())
            state.status.wait(uint8_t(status), std::memory_order_acquire);
        return state.load();
    }

    bool stopping() const noexcept { return m_stopping.load(std::memory_order_acquire); }
    int workers(JobTier tier) const noexcept { return int(m_tiers[size_t(tier)].queues.size()); }

    JobStats stats() const {
        auto stats = JobStats{};
        for (int t = 0; t < 2; ++t) {
            const auto& tier = m_tiers[size_t(t)];
            auto& out = t == 0 ? stats.frame : stats.background;
            out.workers = int(tier.queues.size());
            out.submitted = tier.submitted.load(std::memory_order_relaxed);
            out.succeeded = tier.succeeded.load(std::memory_order_relaxed);
            out.failed = tier.failed.load(std::memory_order_relaxed);
            out.cancelled = tier.cancelled.load(std::memory_order_relaxed);
            out.busy_ms = double(tier.busy_ns.load(std::memory_order_relaxed)) / 1e6;
            out.queued = size_t(std::max<int64_t>(0, tier.queued.load(std::memory_order_relaxed)));
            out.queued_peak = size_t(std::max<int64_t>(0, tier.queued_peak.load(std::memory_order_relaxed)));
        }
        stats.completions_applied = g_applied.load(std::memory_order_relaxed);
        stats.completions_discarded = g_discarded.load(std::memory_order_relaxed);
        stats.drain_ms = double(g_drain_ns.load(std::memory_order_relaxed)) / 1e6;
        return stats;
    }
    void reset_peaks() noexcept {
        for (auto& tier : m_tiers) tier.queued_peak.store(tier.queued.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

private:
    struct Queue {
        std::mutex mutex;
        std::deque<std::shared_ptr<JobState>> jobs;
        std::atomic<int> size{0}; // changed under the lock; read without it, so idle workers skip empty queues
        // Where the queue's worker sleeps: its own lock and condition, so waking one worker touches no
        // lock that another uses.
        std::mutex park_mutex;
        std::condition_variable park;
        bool signalled = false;
    };
    struct Tier {
        std::vector<std::unique_ptr<Queue>> queues; // one per worker
        std::vector<std::thread> threads;
        // Bumped on every push. A worker about to sleep sets its bit in `sleepers`, then reads the epoch
        // again; a waker claims a bit and signals that worker's own condition. (Not std::atomic::wait:
        // libc++ keys those waits through a shared table, so its notify_one can wake a thread waiting on
        // another atomic and lose the wake-up.)
        std::atomic<uint32_t> epoch{0};
        std::atomic<uint64_t> sleepers{0};
        std::atomic<uint32_t> next_queue{0};
        std::atomic<uint64_t> submitted{0}, succeeded{0}, failed{0}, cancelled{0}, busy_ns{0};
        std::atomic<int64_t> queued{0}, queued_peak{0};
    };

    void work(int tier_index, int worker) {
        // Frame work preempts background work at the kernel: the decision this system rests on.
        pthread_set_qos_class_self_np(tier_index == 0 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0);
        const auto name = std::string(tier_index == 0 ? "maya frame " : "maya background ") + std::to_string(worker);
        pthread_setname_np(name.c_str());
        t_runtime = this;
        t_tier = tier_index;
        t_worker = worker;
        auto& tier = m_tiers[size_t(tier_index)];
        for (;;) {
            if (auto job = take(tier_index, worker)) {
                if (tier.queued.load(std::memory_order_relaxed) > 0) wake(tier);
                run(job);
                continue;
            }
            // A moment's spinning before sleeping (a few microseconds of pause instructions, no system
            // calls) keeps workers awake through a burst of small jobs, which would otherwise cost a
            // sleep and a wake-up each.
            if (auto job = spin(tier)) {
                run(job);
                continue;
            }
            const auto epoch = tier.epoch.load(std::memory_order_seq_cst);
            if (auto job = take(tier_index, worker)) {
                run(job);
                continue;
            }
            if (stopping()) return;
            const auto bit = uint64_t{1} << worker;
            auto& queue = *tier.queues[size_t(worker)];
            tier.sleepers.fetch_or(bit, std::memory_order_seq_cst);
            if (tier.epoch.load(std::memory_order_seq_cst) == epoch && !stopping()) {
                park(queue);
            } else if (!(tier.sleepers.fetch_and(~bit, std::memory_order_seq_cst) & bit)) {
                // Something arrived after all, but a waker had already claimed this worker: take its signal.
                park(queue);
            }
        }
    }

    std::shared_ptr<JobState> spin(Tier& tier) {
        const auto tier_index = int(&tier - m_tiers.data());
        for (int i = 0; i < 2048; ++i) {
            if (tier.queued.load(std::memory_order_relaxed) > 0)
                if (auto job = take(tier_index, t_worker)) return job;
            if (stopping()) return nullptr;
#if defined(__aarch64__)
            __asm__ __volatile__("isb");
#else
            __builtin_ia32_pause();
#endif
        }
        return nullptr;
    }

    // A job from the worker's own queue, else stolen from another of the tier's queues.
    std::shared_ptr<JobState> take(int tier_index, int worker) {
        auto& tier = m_tiers[size_t(tier_index)];
        const auto n = tier.queues.size();
        const auto first = worker >= 0 ? size_t(worker) : size_t(tier.next_queue.load(std::memory_order_relaxed)) % n;
        for (size_t i = 0; i < n; ++i) {
            auto& queue = *tier.queues[(first + i) % n];
            if (queue.size.load(std::memory_order_acquire) == 0) continue;
            auto lock = std::lock_guard(queue.mutex);
            if (queue.jobs.empty()) continue;
            auto job = std::move(queue.jobs.front());
            queue.jobs.pop_front();
            queue.size.fetch_sub(1, std::memory_order_release);
            tier.queued.fetch_sub(1, std::memory_order_relaxed);
            return job;
        }
        return nullptr;
    }

    void enqueue(const std::shared_ptr<JobState>& state) {
        auto& tier = m_tiers[size_t(state->tier)];
        const auto own = t_runtime == this && t_tier == int(state->tier);
        const auto index = own ? size_t(t_worker) : size_t(tier.next_queue.fetch_add(1, std::memory_order_relaxed)) % tier.queues.size();
        {
            auto& queue = *tier.queues[index];
            auto lock = std::lock_guard(queue.mutex);
            queue.jobs.push_back(state);
            queue.size.fetch_add(1, std::memory_order_release);
        }
        const auto queued = tier.queued.fetch_add(1, std::memory_order_relaxed) + 1;
        for (auto peak = tier.queued_peak.load(std::memory_order_relaxed);
             queued > peak && !tier.queued_peak.compare_exchange_weak(peak, queued, std::memory_order_relaxed);) {
        }
        tier.epoch.fetch_add(1, std::memory_order_seq_cst);
        wake(tier);
    }

    // Wakes one sleeping worker by claiming its bit, so each sleeper is signalled at most once. The
    // epoch was bumped before this reads `sleepers`, and a worker sets its bit before reading the epoch
    // again, so a worker whose bit this misses sees the push itself.
    void wake(Tier& tier) {
        for (auto sleepers = tier.sleepers.load(std::memory_order_seq_cst); sleepers;) {
            const auto bit = sleepers & (~sleepers + 1);
            if (tier.sleepers.compare_exchange_weak(sleepers, sleepers & ~bit, std::memory_order_seq_cst)) {
                signal(*tier.queues[size_t(std::countr_zero(bit))]);
                return;
            }
        }
    }
    static void signal(Queue& queue) {
        {
            auto lock = std::lock_guard(queue.park_mutex);
            queue.signalled = true;
        }
        queue.park.notify_one();
    }
    static void park(Queue& queue) {
        auto lock = std::unique_lock(queue.park_mutex);
        queue.park.wait(lock, [&] { return queue.signalled; });
        queue.signalled = false;
    }

    void release(const std::shared_ptr<JobState>& state) {
        if (state->unmet.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        // Every prerequisite has reported; a failed or cancelled one decides the outcome without running.
        if (state->blocked != JobStatus::succeeded) finish(state, state->blocked, state->blocked_reason);
        else if (stopping()) finish(state, JobStatus::cancelled, "the job system shut down before it started");
        else enqueue(state);
    }

    void prerequisite_done(const std::shared_ptr<JobState>& dependent, const JobState& prerequisite) {
        const auto outcome = prerequisite.load();
        if (outcome != JobStatus::succeeded) {
            auto lock = std::lock_guard(dependent->mutex);
            if (dependent->blocked == JobStatus::succeeded) {
                dependent->blocked = outcome;
                dependent->blocked_reason = outcome == JobStatus::failed ? "a prerequisite failed: " + prerequisite.error
                                                                         : "a prerequisite was cancelled";
            }
        }
        release(dependent);
    }

public:
    void run(const std::shared_ptr<JobState>& state) {
        if (state->cancel_requested.load(std::memory_order_acquire) || stopping()) {
            finish(state, JobStatus::cancelled, stopping() ? "the job system shut down before it started" : "cancelled before it started");
            return;
        }
        state->status.store(uint8_t(JobStatus::running), std::memory_order_release);
        auto context = JobContext(*state);
        const auto start = Clock::now();
        try {
            state->function(context);
        } catch (const std::exception& error) {
            context.fail(error.what());
        } catch (...) {
            context.fail("an exception that is not a std::exception");
        }
        m_tiers[size_t(state->tier)].busy_ns.fetch_add(nanoseconds_since(start), std::memory_order_relaxed);
        if (state->failed_by_job) finish(state, JobStatus::failed, {});
        else if (state->cancel_requested.load(std::memory_order_acquire) || stopping()) finish(state, JobStatus::cancelled, "cancelled while it ran");
        else finish(state, JobStatus::succeeded, {});
    }

private:
    void finish(const std::shared_ptr<JobState>& state, JobStatus status, const std::string& reason) {
        if (status != JobStatus::failed || state->error.empty()) {
            if (!reason.empty()) state->error = reason;
        }
        state->function = nullptr; // what the job captured is released now, not with the last handle
        auto& tier = m_tiers[size_t(state->tier)];
        (status == JobStatus::succeeded ? tier.succeeded : status == JobStatus::failed ? tier.failed : tier.cancelled)
            .fetch_add(1, std::memory_order_relaxed);
        auto dependents = std::vector<std::shared_ptr<JobState>>{};
        {
            auto lock = std::lock_guard(state->mutex);
            state->finished = true;
            state->status.store(uint8_t(status), std::memory_order_release);
            dependents.swap(state->dependents);
        }
        state->status.notify_all();
        for (const auto& dependent : dependents) prerequisite_done(dependent, *state);
    }

    std::array<Tier, 2> m_tiers;
    std::atomic<bool> m_stopping{false};
};
} // namespace detail

const char* job_status_name(JobStatus status) noexcept {
    switch (status) {
    case JobStatus::pending: return "pending";
    case JobStatus::running: return "running";
    case JobStatus::succeeded: return "succeeded";
    case JobStatus::failed: return "failed";
    case JobStatus::cancelled: return "cancelled";
    }
    return "unknown";
}

bool JobContext::cancelled() const noexcept {
    return m_state.cancel_requested.load(std::memory_order_acquire) || m_state.runtime->stopping();
}
void JobContext::fail(std::string message) {
    m_state.failed_by_job = true;
    m_state.error = message.empty() ? std::string("the job failed") : std::move(message);
}

JobStatus JobHandle::status() const noexcept { return m_state ? m_state->load() : JobStatus::cancelled; }
bool JobHandle::done() const noexcept { return detail::final_status(status()); }
std::string JobHandle::error() const {
    if (!m_state) return "no job";
    return done() ? m_state->error : std::string{};
}
void JobHandle::cancel() const noexcept {
    if (m_state) m_state->cancel_requested.store(true, std::memory_order_release);
}
JobStatus JobHandle::wait() const { return m_state ? m_state->runtime->wait(*m_state) : JobStatus::cancelled; }

JobSystem::JobSystem(JobSystemConfig config) : m_runtime(std::make_unique<detail::JobRuntime>(config)) {}
JobSystem::~JobSystem() = default;
JobHandle JobSystem::submit(JobTier tier, JobFunction function, std::span<const JobHandle> after) {
    return m_runtime->submit(tier, std::move(function), after);
}
void JobSystem::parallel_for(JobTier tier, uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) {
    m_runtime->parallel_for(tier, count, grain, body);
}
int JobSystem::workers(JobTier tier) const noexcept { return m_runtime->workers(tier); }
JobStats JobSystem::stats() const { return m_runtime->stats(); }
void JobSystem::reset_peaks() noexcept { m_runtime->reset_peaks(); }

namespace {
std::mutex g_config_mutex;
JobSystemConfig g_config{};
bool g_created = false;
} // namespace

JobSystem& job_system() {
    static auto system = [] {
        auto lock = std::lock_guard(g_config_mutex);
        g_created = true;
        return std::make_unique<JobSystem>(g_config);
    }();
    return *system;
}
bool configure_job_system(JobSystemConfig config) {
    auto lock = std::lock_guard(g_config_mutex);
    if (g_created) return false;
    g_config = config;
    return true;
}

JobScope::JobScope(JobSystem& system) : m_system(system), m_jobs(std::make_unique<detail::ScopeJobs>()) {}
JobScope::~JobScope() {
    cancel();
    wait();
}
JobHandle JobScope::submit(JobTier tier, JobFunction function, std::span<const JobHandle> after) {
    auto handle = m_system.submit(tier, std::move(function), after);
    auto lock = std::lock_guard(m_jobs->mutex);
    if (m_jobs->jobs.size() >= 64) std::erase_if(m_jobs->jobs, [](const JobHandle& job) { return job.done(); });
    m_jobs->jobs.push_back(handle);
    return handle;
}
void JobScope::cancel() noexcept {
    auto lock = std::lock_guard(m_jobs->mutex);
    for (const auto& job : m_jobs->jobs) job.cancel();
}
void JobScope::wait() {
    auto jobs = std::vector<JobHandle>{};
    {
        auto lock = std::lock_guard(m_jobs->mutex);
        jobs = m_jobs->jobs;
    }
    for (const auto& job : jobs) job.wait();
    auto lock = std::lock_guard(m_jobs->mutex);
    std::erase_if(m_jobs->jobs, [](const JobHandle& job) { return job.done(); });
}
size_t JobScope::pending() const {
    auto lock = std::lock_guard(m_jobs->mutex);
    return size_t(std::count_if(m_jobs->jobs.begin(), m_jobs->jobs.end(), [](const JobHandle& job) { return !job.done(); }));
}

CompletionQueue::CompletionQueue() : m_state(std::make_shared<detail::CompletionState>()) {}
CompletionQueue::~CompletionQueue() {
    auto lock = std::lock_guard(m_state->mutex);
    detail::g_discarded.fetch_add(m_state->items.size(), std::memory_order_relaxed);
    m_state->items.clear();
}
void CompletionQueue::Sink::post(uint64_t request, uint64_t generation, std::function<void()> apply) const {
    const auto queue = m_queue.lock();
    if (!queue) {
        detail::g_discarded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto lock = std::lock_guard(queue->mutex);
    queue->items.push_back({request, generation, std::move(apply)});
}
CompletionQueue::Sink CompletionQueue::sink() const {
    auto sink = Sink{};
    sink.m_queue = m_state;
    return sink;
}
CompletionQueue::Drained CompletionQueue::drain(std::chrono::microseconds budget, const std::function<bool(uint64_t, uint64_t)>& current) {
    const auto start = detail::Clock::now();
    auto drained = Drained{};
    for (;;) {
        auto item = detail::CompletionItem{};
        {
            auto lock = std::lock_guard(m_state->mutex);
            if (m_state->items.empty()) break;
            item = std::move(m_state->items.front());
            m_state->items.pop_front();
        }
        if (current && !current(item.request, item.generation)) {
            ++drained.discarded;
        } else {
            item.apply();
            ++drained.applied;
        }
        if (detail::Clock::now() - start >= budget) break;
    }
    {
        auto lock = std::lock_guard(m_state->mutex);
        drained.remaining = m_state->items.size();
    }
    const auto elapsed = detail::nanoseconds_since(start);
    drained.ms = double(elapsed) / 1e6;
    detail::g_applied.fetch_add(drained.applied, std::memory_order_relaxed);
    detail::g_discarded.fetch_add(drained.discarded, std::memory_order_relaxed);
    detail::g_drain_ns.fetch_add(elapsed, std::memory_order_relaxed);
    return drained;
}
size_t CompletionQueue::pending() const {
    auto lock = std::lock_guard(m_state->mutex);
    return m_state->items.size();
}
} // namespace maya
