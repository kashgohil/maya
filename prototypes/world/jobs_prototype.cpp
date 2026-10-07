// Job system prototype for the world-scale decisions (#1060, docs/architecture/world-scale-decision.md).
//
// Four candidates behind one adapter: a Maya-owned pool (two priority queues under one lock), enkiTS,
// Taskflow, and Grand Central Dispatch. Each runs the same measurements:
//   1. spawn: 200,000 empty jobs submitted one at a time, then waited for;
//   2. parallel-for: 1,000,000 small items (a 64-step hash each), against one thread;
//   3. coarse: 96 jobs of 4 ms each (decode-sized), against the ideal for the worker count;
//   4. priority: while 1,200 background jobs of 2 ms queue, 40 urgent jobs: time from submit to start;
//   5. cancellation: 4,000 background jobs of 0.5 ms that check a token, cancelled after 20 ms;
//   6. Jolt: 5,000 boxes falling into a pile, stepped 300 times through a JPH::JobSystem adapter, alone
//      and while background jobs keep every worker busy (streaming during play).
// Jolt's own JobSystemThreadPool is the reference for 6. Lines starting UNEXPECTED report what the
// decision relies on and make the run fail.

#include "world/jolt_scene.hpp"

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <TaskScheduler.h>
#include <taskflow/algorithm/for_each.hpp>
#include <taskflow/taskflow.hpp>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <pthread/qos.h>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
int failures = 0;
void unexpected(const std::string& what) {
    std::printf("UNEXPECTED: %s\n", what.c_str());
    ++failures;
}

uint32_t hash_work(uint32_t seed, int steps) {
    auto x = seed * 0x9e3779b9u + 1;
    for (int i = 0; i < steps; ++i) {
        x ^= x >> 15;
        x *= 0x2c1b3c6du;
        x ^= x >> 12;
    }
    return x;
}
void spin_for(double milliseconds) {
    const auto start = Clock::now();
    while (ms_since(start) < milliseconds) {
    }
}

enum class Priority { high, background };
using Job = std::function<void()>;

/// What Maya would need from a job system, as the prototype uses it.
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    virtual int threads() const = 0; // threads that run jobs, the caller included where it helps
    /// Thread-safe from the owner thread and from jobs.
    virtual void submit(Priority priority, Job job) = 0;
    /// Waits for everything submitted so far.
    virtual void wait_all() = 0;
    virtual void parallel_for(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) = 0;
    /// Called on a thread the backend did not create before it submits; detach before it exits.
    virtual void attach_thread() {}
    virtual void detach_thread() {}
};

// --- A Maya-owned pool: two FIFO queues under one mutex, workers prefer the high queue. --------------
// `reserved` workers never take background jobs, so frame work always finds a free worker; `qos` is the
// workers' quality-of-service class, which macOS uses to pick cores and to preempt.
class OwnPool final : public Backend {
public:
    OwnPool(const char* name, int workers, int reserved = 0, qos_class_t qos = QOS_CLASS_USER_INITIATED)
        : m_name(name), m_reserved(reserved) {
        for (int i = 0; i < workers; ++i) m_workers.emplace_back([this, i, qos] {
            pthread_set_qos_class_self_np(qos, 0);
            run(i < m_reserved);
        });
    }
    ~OwnPool() override {
        {
            auto lock = std::lock_guard(m_mutex);
            m_stop = true;
        }
        m_ready.notify_all();
        for (auto& worker : m_workers) worker.join();
    }
    const char* name() const override { return m_name; }
    int threads() const override { return int(m_workers.size()); }
    void submit(Priority priority, Job job) override {
        m_pending.fetch_add(1, std::memory_order_relaxed);
        {
            auto lock = std::lock_guard(m_mutex);
            (priority == Priority::high ? m_high : m_low).push_back(std::move(job));
        }
        // A reserved worker woken for a background job would go back to sleep: wake them all instead.
        if (priority == Priority::background && m_reserved > 0) m_ready.notify_all();
        else m_ready.notify_one();
    }
    void wait_all() override {
        auto lock = std::unique_lock(m_mutex);
        m_idle.wait(lock, [&] { return m_pending.load() == 0; });
    }
    void parallel_for(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) override {
        auto left = std::atomic<uint32_t>{0};
        const auto chunks = (count + grain - 1) / grain;
        left = chunks;
        for (uint32_t c = 0; c < chunks; ++c) {
            submit(Priority::high, [&, c] {
                body(c * grain, std::min(count, (c + 1) * grain));
                left.fetch_sub(1, std::memory_order_release);
            });
        }
        while (left.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    }

private:
    void run(bool reserved) {
        for (;;) {
            auto job = Job{};
            {
                auto lock = std::unique_lock(m_mutex);
                m_ready.wait(lock, [&] { return m_stop || !m_high.empty() || (!reserved && !m_low.empty()); });
                if (m_high.empty() && (reserved || m_low.empty())) return; // stopping
                auto& queue = m_high.empty() ? m_low : m_high;
                job = std::move(queue.front());
                queue.pop_front();
            }
            job();
            if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                auto lock = std::lock_guard(m_mutex);
                m_idle.notify_all();
            }
        }
    }
    const char* m_name;
    int m_reserved;
    std::mutex m_mutex;
    std::condition_variable m_ready, m_idle;
    std::deque<Job> m_high, m_low;
    std::atomic<int64_t> m_pending{0};
    bool m_stop = false;
    std::vector<std::thread> m_workers;
};

// --- Two tiers: frame workers at user-interactive QoS, background workers at utility QoS, each with all
// cores; the kernel preempts background threads when frame work arrives.
class TieredPool final : public Backend {
public:
    explicit TieredPool(int workers)
        : m_frame("frame", workers, 0, QOS_CLASS_USER_INTERACTIVE), m_background("background", workers, 0, QOS_CLASS_UTILITY) {}
    const char* name() const override { return "maya tiers"; }
    int threads() const override { return m_frame.threads(); }
    void submit(Priority priority, Job job) override {
        (priority == Priority::high ? m_frame : m_background).submit(Priority::high, std::move(job));
    }
    void wait_all() override {
        m_background.wait_all();
        m_frame.wait_all();
    }
    void parallel_for(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) override {
        m_frame.parallel_for(count, grain, body);
    }

private:
    OwnPool m_frame, m_background;
};

// --- enkiTS: task sets in priority pipes; the calling thread is thread 0. ----------------------------
class Enki final : public Backend {
public:
    explicit Enki(int threads) {
        auto config = m_scheduler.GetConfig();
        config.numTaskThreadsToCreate = uint32_t(threads - 1);
        config.numExternalTaskThreads = 1; // the feeder that stands in for streaming requests
        m_scheduler.Initialize(config);
    }
    ~Enki() override {
        m_scheduler.WaitforAll();
        m_scheduler.ShutdownNow();
    }
    const char* name() const override { return "enkiTS"; }
    int threads() const override { return int(m_scheduler.GetNumTaskThreads()); }
    void submit(Priority priority, Job job) override {
        auto task = std::make_unique<enki::TaskSet>(1, [job = std::move(job)](enki::TaskSetPartition, uint32_t) { job(); });
        task->m_Priority = priority == Priority::high ? enki::TASK_PRIORITY_HIGH : enki::TASK_PRIORITY_LOW;
        auto* raw = task.get();
        {
            auto lock = std::lock_guard(m_mutex);
            if (m_tasks.size() >= m_cleanup_at) { // amortized: completed tasks are freed in bulk
                std::erase_if(m_tasks, [](const auto& t) { return t->GetIsComplete(); });
                m_cleanup_at = std::max<size_t>(4096, m_tasks.size() * 2);
            }
            m_tasks.push_back(std::move(task));
        }
        m_scheduler.AddTaskSetToPipe(raw);
    }
    void wait_all() override {
        m_scheduler.WaitforAll();
        auto lock = std::lock_guard(m_mutex);
        m_tasks.clear();
    }
    void parallel_for(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) override {
        auto task = enki::TaskSet(count, [&](enki::TaskSetPartition range, uint32_t) { body(range.start, range.end); });
        task.m_MinRange = grain;
        m_scheduler.AddTaskSetToPipe(&task);
        m_scheduler.WaitforTask(&task);
    }
    void attach_thread() override { m_scheduler.RegisterExternalTaskThread(); }
    void detach_thread() override { m_scheduler.DeRegisterExternalTaskThread(); }

private:
    enki::TaskScheduler m_scheduler;
    std::mutex m_mutex;
    std::vector<std::unique_ptr<enki::TaskSet>> m_tasks;
    size_t m_cleanup_at = 4096;
};

// --- Taskflow: one work-stealing executor; no priorities, so both kinds share its queues. ------------
class Taskflow final : public Backend {
public:
    explicit Taskflow(int threads) : m_executor(size_t(threads)) {}
    const char* name() const override { return "Taskflow"; }
    int threads() const override { return int(m_executor.num_workers()); }
    void submit(Priority, Job job) override { m_executor.silent_async(std::move(job)); }
    void wait_all() override { m_executor.wait_for_all(); }
    void parallel_for(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) override {
        auto flow = tf::Taskflow{};
        const auto chunks = (count + grain - 1) / grain;
        flow.for_each_index(uint32_t{0}, chunks, uint32_t{1},
            [&](uint32_t c) { body(c * grain, std::min(count, (c + 1) * grain)); });
        m_executor.run(flow).wait();
    }

private:
    tf::Executor m_executor;
};

// --- Grand Central Dispatch: the system's pool; priorities are quality-of-service classes. ----------
class Dispatch final : public Backend {
public:
    Dispatch() : m_group(dispatch_group_create()) {}
    ~Dispatch() override { dispatch_release(m_group); }
    const char* name() const override { return "GCD"; }
    int threads() const override { return int(std::thread::hardware_concurrency()); }
    void submit(Priority priority, Job job) override {
        auto* queue = dispatch_get_global_queue(priority == Priority::high ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0);
        dispatch_group_async_f(m_group, queue, new Job(std::move(job)), [](void* context) {
            auto* owned = static_cast<Job*>(context);
            (*owned)();
            delete owned;
        });
    }
    void wait_all() override { dispatch_group_wait(m_group, DISPATCH_TIME_FOREVER); }
    void parallel_for(uint32_t count, uint32_t grain, const std::function<void(uint32_t, uint32_t)>& body) override {
        struct Context {
            const std::function<void(uint32_t, uint32_t)>* body;
            uint32_t count, grain;
        } context{&body, count, grain};
        dispatch_apply_f((count + grain - 1) / grain, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &context,
            [](void* raw, size_t c) {
                const auto& ctx = *static_cast<Context*>(raw);
                (*ctx.body)(uint32_t(c) * ctx.grain, std::min(ctx.count, uint32_t(c + 1) * ctx.grain));
            });
    }

private:
    dispatch_group_t m_group;
};

// --- Jolt on any backend: jobs are queued at high priority when their dependencies are met. ---------
class JoltAdapter final : public JPH::JobSystemWithBarrier {
public:
    explicit JoltAdapter(Backend& backend) : JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), m_backend(backend) {
        m_jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
    }
    int GetMaxConcurrency() const override { return m_backend.threads(); }
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencies) override {
        JPH::uint32 index;
        while ((index = m_jobs.ConstructObject(name, color, this, function, dependencies)) == AvailableJobs::cInvalidObjectIndex)
            std::this_thread::yield();
        auto* job = &m_jobs.Get(index);
        auto handle = JobHandle(job);
        if (dependencies == 0) QueueJob(job);
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        m_backend.submit(Priority::high, [job] {
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
    Backend& m_backend;
    AvailableJobs m_jobs;
};

struct Percentiles {
    double p50, p99, max;
};
Percentiles percentiles(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) { return values[std::min(values.size() - 1, size_t(q * double(values.size())))]; };
    return {at(0.5), at(0.99), values.back()};
}

struct Result {
    std::string name;
    double spawn_ns = 0, parallel_speedup = 0, coarse_efficiency = 0;
    Percentiles urgent{};
    double cancel_drain_ms = 0;
    int cancel_ran = 0;
    double jolt_ms = 0, jolt_busy_ms = 0;
};

// Steps 5,000 boxes falling onto a ground slab; returns the mean step time over steps 60-300.
double step_pile(JPH::JobSystem& jobs, Backend* busy_backend) {
    auto scene = prototype::PhysicsScene{};
    auto& bodies = scene.system.GetBodyInterface();
    bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(60, 1, 60)), JPH::RVec3(0, -1, 0),
                                JPH::Quat::sIdentity(), JPH::EMotionType::Static, prototype::static_layer),
        JPH::EActivation::DontActivate);
    const auto box = JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3::sReplicate(0.25f)));
    auto ids = std::vector<JPH::BodyID>{};
    for (int i = 0; i < 5000; ++i) {
        const auto x = float(i % 25) * 0.6f - 7.5f, z = float((i / 25) % 20) * 0.6f - 6.0f, y = 0.5f + float(i / 500) * 0.6f;
        auto settings = JPH::BodyCreationSettings(box, JPH::RVec3(x, y, z), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, prototype::moving_layer);
        ids.push_back(bodies.CreateBody(settings)->GetID());
    }
    const auto state = bodies.AddBodiesPrepare(ids.data(), int(ids.size()));
    bodies.AddBodiesFinalize(ids.data(), int(ids.size()), state, JPH::EActivation::Activate);
    scene.system.OptimizeBroadPhase();

    auto stop = std::atomic<bool>{false};
    auto feeder = std::thread{};
    if (busy_backend) {
        // Streaming while playing: keep roughly twice the worker count of 1 ms background jobs queued.
        feeder = std::thread([&] {
            busy_backend->attach_thread();
            auto queued = std::atomic<int>{0};
            const auto target = busy_backend->threads() * 2;
            while (!stop.load()) {
                while (queued.load() < target) {
                    ++queued;
                    busy_backend->submit(Priority::background, [&queued] {
                        spin_for(1.0);
                        --queued;
                    });
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            while (queued.load() > 0) std::this_thread::yield();
            busy_backend->detach_thread();
        });
    }
    auto total = 0.0;
    for (int step = 0; step < 300; ++step) {
        const auto start = Clock::now();
        scene.system.Update(1.0f / 60.0f, 1, &scene.temp, &jobs);
        if (step >= 60) total += ms_since(start);
    }
    if (busy_backend) {
        stop = true;
        feeder.join();
        busy_backend->wait_all();
    }
    // The pile must have come to rest on the ground, not fallen through or exploded.
    auto lowest = 1e9f, highest = -1e9f;
    for (const auto id : ids) {
        const auto y = float(bodies.GetCenterOfMassPosition(id).GetY());
        lowest = std::min(lowest, y);
        highest = std::max(highest, y);
    }
    if (lowest < 0.0f || highest > 10.0f) unexpected("the box pile did not settle (y from " + std::to_string(lowest) + " to " + std::to_string(highest) + ")");
    for (const auto id : ids) bodies.RemoveBody(id);
    for (const auto id : ids) bodies.DestroyBody(id);
    return total / 240.0;
}

Result measure(Backend& backend) {
    auto result = Result{backend.name()};
    const auto threads = backend.threads();

    { // 1. spawn
        auto done = std::atomic<int>{0};
        const auto start = Clock::now();
        for (int i = 0; i < 200000; ++i) backend.submit(Priority::high, [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        backend.wait_all();
        result.spawn_ns = ms_since(start) * 1e6 / 200000.0;
        if (done != 200000) unexpected(std::string(backend.name()) + " ran " + std::to_string(done.load()) + " of 200,000 jobs");
    }
    { // 2. parallel-for
        auto values = std::vector<uint32_t>(1000000);
        auto start = Clock::now();
        for (uint32_t i = 0; i < values.size(); ++i) values[i] = hash_work(i, 64);
        const auto serial = ms_since(start);
        const auto expected = std::accumulate(values.begin(), values.end(), uint64_t{0});
        std::fill(values.begin(), values.end(), 0u);
        start = Clock::now();
        backend.parallel_for(uint32_t(values.size()), 4096, [&](uint32_t begin, uint32_t end) {
            for (auto i = begin; i < end; ++i) values[i] = hash_work(i, 64);
        });
        result.parallel_speedup = serial / ms_since(start);
        if (std::accumulate(values.begin(), values.end(), uint64_t{0}) != expected) unexpected(std::string(backend.name()) + " parallel-for missed items");
    }
    { // 3. coarse
        const auto start = Clock::now();
        for (int i = 0; i < 96; ++i) backend.submit(Priority::background, [] { spin_for(4.0); });
        backend.wait_all();
        const auto ideal = 4.0 * std::ceil(96.0 / double(threads));
        result.coarse_efficiency = ideal / ms_since(start);
    }
    { // 4. priority
        for (int i = 0; i < 1200; ++i) backend.submit(Priority::background, [] { spin_for(2.0); });
        auto latencies = std::vector<double>(40);
        for (int i = 0; i < 40; ++i) {
            const auto submitted = Clock::now();
            backend.submit(Priority::high, [&, i, submitted] {
                latencies[size_t(i)] = ms_since(submitted);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        backend.wait_all();
        result.urgent = percentiles(latencies);
    }
    { // 5. cancellation: jobs check a shared token before they start their work
        auto cancelled = std::atomic<bool>{false};
        auto ran = std::atomic<int>{0};
        for (int i = 0; i < 4000; ++i) {
            backend.submit(Priority::background, [&] {
                if (cancelled.load(std::memory_order_acquire)) return;
                spin_for(0.5);
                ran.fetch_add(1);
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto start = Clock::now();
        cancelled = true;
        backend.wait_all();
        result.cancel_drain_ms = ms_since(start);
        result.cancel_ran = ran;
    }
    { // 6. Jolt
        auto jobs = JoltAdapter(backend);
        step_pile(jobs, nullptr); // warm up
        result.jolt_ms = step_pile(jobs, nullptr);
        result.jolt_busy_ms = step_pile(jobs, &backend);
    }
    return result;
}

// Jolt's own pool, with a separate Maya pool producing background work beside it.
Result measure_jolt_pool(int threads) {
    auto result = Result{"Jolt pool"};
    auto jobs = JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads - 1);
    step_pile(jobs, nullptr);
    result.jolt_ms = step_pile(jobs, nullptr);
    auto other = OwnPool("other", threads - 1);
    result.jolt_busy_ms = step_pile(jobs, &other);
    return result;
}
} // namespace

int main() {
    const auto threads = int(std::thread::hardware_concurrency());
    std::printf("Job system prototype (#1060): %d hardware threads; Release build expected\n", threads);
    auto jolt = prototype::JoltRuntime{};
    auto results = std::vector<Result>{};
    {
        auto pool = OwnPool("maya pool", threads - 1);
        results.push_back(measure(pool));
    }
    {
        auto pool = OwnPool("maya rsv 4", threads - 1, 4);
        results.push_back(measure(pool));
    }
    {
        auto pool = TieredPool(threads - 1);
        results.push_back(measure(pool));
    }
    {
        auto enki = Enki(threads);
        results.push_back(measure(enki));
    }
    {
        auto flow = Taskflow(threads);
        results.push_back(measure(flow));
    }
    {
        auto gcd = Dispatch();
        results.push_back(measure(gcd));
    }
    results.push_back(measure_jolt_pool(threads));

    std::printf("\n%-10s %9s %9s %8s %22s %16s %16s\n", "candidate", "spawn ns", "pfor x", "coarse", "urgent p50/p99/max ms",
        "cancel ms (ran)", "jolt ms (busy)");
    for (const auto& r : results) {
        if (r.name == "Jolt pool") {
            std::printf("%-10s %9s %9s %8s %22s %16s %7.2f (%6.2f)\n", r.name.c_str(), "-", "-", "-", "-", "-", r.jolt_ms, r.jolt_busy_ms);
            continue;
        }
        std::printf("%-10s %9.0f %9.2f %7.0f%% %7.2f/%6.2f/%6.2f %9.2f (%4d) %7.2f (%6.2f)\n", r.name.c_str(), r.spawn_ns,
            r.parallel_speedup, r.coarse_efficiency * 100.0, r.urgent.p50, r.urgent.p99, r.urgent.max, r.cancel_drain_ms,
            r.cancel_ran, r.jolt_ms, r.jolt_busy_ms);
    }
    // What the decision relies on: frame work on the tiered pool is not slowed by background work, starts
    // at once, and Jolt runs on it about as fast as on its own pool.
    const auto find = [&](const char* name) { return *std::find_if(results.begin(), results.end(), [&](const Result& r) { return r.name == name; }); };
    const auto tiers = find("maya tiers"), jolt_pool = find("Jolt pool");
    if (tiers.jolt_busy_ms > 1.5 * tiers.jolt_ms) unexpected("background work slowed Jolt on the tiered pool: " + std::to_string(tiers.jolt_busy_ms) + " ms busy vs " + std::to_string(tiers.jolt_ms) + " ms");
    if (tiers.urgent.p99 > 1.0) unexpected("urgent jobs on the tiered pool waited " + std::to_string(tiers.urgent.p99) + " ms at P99");
    if (tiers.jolt_ms > 1.3 * jolt_pool.jolt_ms) unexpected("Jolt on the tiered pool took " + std::to_string(tiers.jolt_ms) + " ms a step vs " + std::to_string(jolt_pool.jolt_ms) + " ms on its own pool");
    if (failures) std::printf("\n%d unexpected results\n", failures);
    return failures ? 1 : 0;
}
