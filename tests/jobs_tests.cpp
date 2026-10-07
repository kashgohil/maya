// The job system (#1061, docs/jobs.md): tiers, dependencies, failure, cancellation, waiting inside jobs,
// parallel-for, scopes, shutdown with jobs in flight, completion queues, a stress run, and the two-tier
// property the decision rests on (docs/architecture/world-scale-decision.md#jobs-a-two-tier-pool).

#include "maya/jobs/jobs.hpp"
#include "maya/metrics/metrics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace maya;

namespace {
void spin_for(double milliseconds) {
    const auto watch = Stopwatch{};
    while (watch.milliseconds() < milliseconds) {
    }
}
// A job that holds its tier's attention until released, so later jobs stay queued behind dependencies.
struct Gate {
    std::atomic<bool> open{false};
    JobHandle job;
    Gate(JobSystem& system, JobTier tier) {
        job = system.submit(tier, [this](JobContext&) {
            while (!open.load()) std::this_thread::yield();
        });
    }
    void release() { open = true; }
    ~Gate() {
        release();
        job.wait();
    }
};
} // namespace

TEST_CASE("Jobs run on both tiers and are counted", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{2, 3});
    CHECK(system.workers(JobTier::frame) == 2);
    CHECK(system.workers(JobTier::background) == 3);
    auto ran = std::atomic<int>{0};
    auto handles = std::vector<JobHandle>{};
    for (int i = 0; i < 50; ++i) handles.push_back(system.submit(i % 2 ? JobTier::frame : JobTier::background, [&](JobContext&) { ++ran; }));
    for (const auto& h : handles) CHECK(h.wait() == JobStatus::succeeded);
    CHECK(ran == 50);
    const auto stats = system.stats();
    CHECK(stats.frame.submitted == 25);
    CHECK(stats.background.submitted == 25);
    CHECK(stats.frame.succeeded + stats.background.succeeded == 50);
    CHECK(stats.frame.queued == 0);
    CHECK(std::string(job_status_name(JobStatus::cancelled)) == "cancelled");
    CHECK(JobHandle{}.status() == JobStatus::cancelled); // an empty handle is no job
}

TEST_CASE("A job runs only after its prerequisites, in dependency order", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{3, 3});
    auto order = std::vector<int>{};
    auto mutex = std::mutex{};
    const auto record = [&](int value) {
        return [&, value](JobContext&) {
            spin_for(0.2);
            auto lock = std::lock_guard(mutex);
            order.push_back(value);
        };
    };
    // A chain across both tiers.
    auto previous = JobHandle{};
    for (int i = 0; i < 20; ++i) {
        const JobHandle after[] = {previous};
        previous = system.submit(i % 2 ? JobTier::frame : JobTier::background, record(i), previous ? std::span(after) : std::span<const JobHandle>{});
    }
    previous.wait();
    REQUIRE(order.size() == 20);
    for (int i = 0; i < 20; ++i) CHECK(order[size_t(i)] == i);
    // Fan-in: the last runs after all of the others.
    order.clear();
    auto parts = std::vector<JobHandle>{};
    for (int i = 0; i < 10; ++i) parts.push_back(system.submit(JobTier::background, record(i)));
    system.submit(JobTier::frame, record(99), parts).wait();
    REQUIRE(order.size() == 11);
    CHECK(order.back() == 99);
    // A prerequisite already done counts as met.
    const JobHandle done[] = {parts.front()};
    CHECK(system.submit(JobTier::frame, [](JobContext&) {}, done).wait() == JobStatus::succeeded);
}

TEST_CASE("Failures reach whoever waits, and stop what depends on them", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{2, 2});
    const auto failed = system.submit(JobTier::background, [](JobContext& context) { context.fail("the file is corrupt"); });
    CHECK(failed.wait() == JobStatus::failed);
    CHECK(failed.error() == "the file is corrupt");
    const auto threw = system.submit(JobTier::frame, [](JobContext&) { throw std::runtime_error("out of range"); });
    CHECK(threw.wait() == JobStatus::failed);
    CHECK(threw.error() == "out of range");

    auto ran = std::atomic<bool>{false};
    const JobHandle after[] = {failed};
    const auto dependent = system.submit(JobTier::frame, [&](JobContext&) { ran = true; }, after);
    CHECK(dependent.wait() == JobStatus::failed);
    CHECK(dependent.error() == "a prerequisite failed: the file is corrupt");
    // ...and transitively.
    const JobHandle after_dependent[] = {dependent};
    const auto further = system.submit(JobTier::background, [&](JobContext&) { ran = true; }, after_dependent);
    CHECK(further.wait() == JobStatus::failed);
    CHECK(further.error() == "a prerequisite failed: a prerequisite failed: the file is corrupt");
    CHECK_FALSE(ran);
    // The pool carries on.
    CHECK(system.submit(JobTier::frame, [](JobContext&) {}).wait() == JobStatus::succeeded);
    CHECK(system.stats().frame.failed + system.stats().background.failed == 4);
}

TEST_CASE("A cancelled job never reports success", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{1, 1});
    SECTION("cancelled before it starts, it never runs") {
        auto gate = Gate(system, JobTier::background);
        auto ran = std::atomic<bool>{false};
        const auto queued = system.submit(JobTier::background, [&](JobContext&) { ran = true; });
        queued.cancel();
        gate.release();
        CHECK(queued.wait() == JobStatus::cancelled);
        CHECK(queued.error() == "cancelled before it started");
        CHECK_FALSE(ran);
    }
    SECTION("cancelled while it runs, it sees the request and ends cancelled") {
        auto started = std::latch(1);
        auto saw = std::atomic<bool>{false};
        const auto running = system.submit(JobTier::background, [&](JobContext& context) {
            started.count_down();
            while (!context.cancelled()) std::this_thread::yield();
            saw = true; // returns normally: still cancelled, never succeeded
        });
        started.wait();
        running.cancel();
        CHECK(running.wait() == JobStatus::cancelled);
        CHECK(running.error() == "cancelled while it ran");
        CHECK(saw);
    }
    SECTION("a cancelled prerequisite cancels its dependents") {
        auto gate = Gate(system, JobTier::frame);
        const auto first = system.submit(JobTier::frame, [](JobContext&) {});
        first.cancel();
        const JobHandle after[] = {first};
        auto ran = std::atomic<bool>{false};
        const auto second = system.submit(JobTier::background, [&](JobContext&) { ran = true; }, after);
        gate.release();
        CHECK(second.wait() == JobStatus::cancelled);
        CHECK(second.error() == "a prerequisite was cancelled");
        CHECK_FALSE(ran);
    }
}

TEST_CASE("Waiting inside a job runs other jobs, so a full tier cannot deadlock", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{2, 1});
    // Eight jobs on two workers, each waiting for a job it queues behind itself: only helping finishes them.
    auto outer = std::vector<JobHandle>{};
    auto inner_ran = std::atomic<int>{0};
    for (int i = 0; i < 8; ++i)
        outer.push_back(system.submit(JobTier::frame, [&](JobContext&) {
            const auto inner = system.submit(JobTier::frame, [&](JobContext&) {
                spin_for(0.1);
                ++inner_ran;
            });
            CHECK(inner.wait() == JobStatus::succeeded);
        }));
    for (const auto& job : outer) CHECK(job.wait() == JobStatus::succeeded);
    CHECK(inner_ran == 8);
}

TEST_CASE("parallel_for covers every index once, from any thread", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{3, 2});
    for (const auto tier : {JobTier::frame, JobTier::background}) {
        auto hits = std::vector<std::atomic<int>>(10007);
        system.parallel_for(tier, uint32_t(hits.size()), 64, [&](uint32_t first, uint32_t last) {
            for (auto i = first; i < last; ++i) ++hits[i];
        });
        CHECK(std::all_of(hits.begin(), hits.end(), [](const auto& h) { return h.load() == 1; }));
    }
    // Nested inside a job of the same tier: the job's thread joins in.
    auto total = std::atomic<uint64_t>{0};
    system.submit(JobTier::frame, [&](JobContext&) {
              system.parallel_for(JobTier::frame, 1000, 10, [&](uint32_t first, uint32_t last) {
                  for (auto i = first; i < last; ++i) total += i;
              });
          }).wait();
    CHECK(total == 999u * 1000u / 2u);
    // The first exception is rethrown once every chunk is done.
    auto finished = std::atomic<int>{0};
    CHECK_THROWS_AS(system.parallel_for(JobTier::frame, 100, 1,
                        [&](uint32_t first, uint32_t) {
                            if (first == 37) throw std::runtime_error("chunk 37");
                            ++finished;
                        }),
        std::runtime_error);
    CHECK(finished == 99);
    system.parallel_for(JobTier::frame, 0, 1, [](uint32_t, uint32_t) { FAIL("nothing to do"); });
}

TEST_CASE("A scope cancels and waits for its jobs before its owner goes", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{2, 2});
    auto finished = std::atomic<int>{0}, cancelled = std::atomic<int>{0};
    {
        struct Owner {
            std::vector<int> data = std::vector<int>(1000, 1);
        } owner;
        auto scope = JobScope(system);
        for (int i = 0; i < 200; ++i)
            scope.submit(JobTier::background, [&](JobContext& context) {
                for (int step = 0; step < 50; ++step) {
                    if (context.cancelled()) {
                        ++cancelled;
                        return;
                    }
                    owner.data[size_t(step)] += 1; // refers to the owner: must not outlive it
                    spin_for(0.02);
                }
                ++finished;
            });
        CHECK(scope.pending() > 0);
    }
    CHECK(finished + cancelled <= 200); // jobs that never started neither finished nor saw a cancel
    CHECK(system.stats().background.queued == 0);
    auto scope = JobScope(system);
    for (int i = 0; i < 10; ++i) scope.submit(JobTier::frame, [](JobContext&) {});
    scope.wait();
    CHECK(scope.pending() == 0);
}

TEST_CASE("Destroying the job system with jobs in flight ends every job", "[jobs]") {
    auto handles = std::vector<JobHandle>{};
    auto ran = std::atomic<int>{0};
    {
        auto system = JobSystem(JobSystemConfig{2, 2});
        for (int i = 0; i < 400; ++i) {
            auto after = std::vector<JobHandle>{};
            if (i >= 2) after = {handles[size_t(i - 2)]};
            handles.push_back(system.submit(i % 3 ? JobTier::background : JobTier::frame,
                [&](JobContext& context) {
                    ++ran;
                    for (int step = 0; step < 20 && !context.cancelled(); ++step) spin_for(0.05);
                },
                after));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto cancelled = 0;
    for (const auto& h : handles) {
        REQUIRE(h.done());
        CHECK(h.status() != JobStatus::failed);
        cancelled += h.status() == JobStatus::cancelled;
    }
    CHECK(cancelled > 0);
    CHECK(ran < 400);
}

TEST_CASE("Completions reach the owner in order, under a budget, and stale ones are discarded", "[jobs]") {
    auto system = JobSystem(JobSystemConfig{2, 2});
    auto queue = CompletionQueue{};
    auto sink = queue.sink();
    auto jobs = std::vector<JobHandle>{};
    for (uint64_t r = 0; r < 100; ++r) {
        const JobHandle after[] = {jobs.empty() ? JobHandle{} : jobs.back()};
        jobs.push_back(system.submit(JobTier::background, [sink, r](JobContext&) { sink.post(r, r % 4 == 0 ? 1 : 2, [] {}); }, after));
    }
    jobs.back().wait();
    CHECK(queue.pending() == 100);
    const auto before = system.stats();
    auto applied = std::vector<uint64_t>{};
    // Requests posted at generation 1 have since been replaced: the owner says they are stale.
    auto current = [](uint64_t, uint64_t generation) { return generation == 2; };
    // A zero budget still makes progress: one completion a call.
    auto first = queue.drain(std::chrono::microseconds(0), current);
    CHECK(first.applied + first.discarded == 1);
    auto total = first;
    while (queue.pending()) {
        const auto drained = queue.drain(std::chrono::microseconds(1000), current);
        total.applied += drained.applied;
        total.discarded += drained.discarded;
    }
    CHECK(total.applied == 75);
    CHECK(total.discarded == 25);
    const auto after = system.stats();
    CHECK(after.completions_applied - before.completions_applied == 75);
    CHECK(after.completions_discarded - before.completions_discarded == 25);

    // In post order.
    auto order = std::vector<int>{};
    for (int i = 0; i < 5; ++i) sink.post(uint64_t(i), 0, [&order, i] { order.push_back(i); });
    queue.drain(std::chrono::milliseconds(10));
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
    // A sink that outlives its queue drops what it posts.
    auto orphan = CompletionQueue::Sink{};
    {
        auto gone = CompletionQueue{};
        orphan = gone.sink();
        gone.sink().post(1, 1, [] { FAIL("a discarded queue's completion ran"); });
    }
    auto ran = false;
    orphan.post(2, 1, [&] { ran = true; });
    CHECK_FALSE(ran);
}

TEST_CASE("Bursts of small jobs never strand a job, whatever the worker count", "[jobs][stress]") {
    // A lost wake-up leaves every worker asleep with a job queued. Each burst must finish by a deadline,
    // so a lost wake-up fails here instead of hanging (it once did, with four workers).
    for (const auto workers : {1, 2, 4, 13}) {
        auto system = JobSystem(JobSystemConfig{workers, workers});
        for (int round = 0; round < 40; ++round) {
            const auto tier = round % 2 ? JobTier::frame : JobTier::background;
            auto handles = std::vector<JobHandle>{};
            for (int i = 0; i < 2000; ++i) handles.push_back(system.submit(tier, [](JobContext&) {}));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            auto all_done = false;
            while (!all_done && std::chrono::steady_clock::now() < deadline) {
                all_done = std::all_of(handles.begin(), handles.end(), [](const JobHandle& h) { return h.done(); });
                if (!all_done) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            INFO(workers << " workers, round " << round);
            REQUIRE(all_done);
            // Let the workers fall asleep between bursts, which is when a wake-up can be lost.
            if (round % 4 == 3) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

TEST_CASE("100,000 small jobs with random dependencies and cancellation all end", "[jobs][stress]") {
    auto system = JobSystem(JobSystemConfig{});
    auto random = std::mt19937(1061);
    auto handles = std::vector<JobHandle>{};
    handles.reserve(100000);
    auto ran = std::atomic<uint64_t>{0};
    auto gate = Gate(system, JobTier::background);
    auto cancelled_early = std::vector<size_t>{};
    for (size_t i = 0; i < 100000; ++i) {
        auto after = std::vector<JobHandle>{};
        if (i > 0 && random() % 4 == 0) after.push_back(handles[random() % i]);
        const auto gated = i == 0 || random() % 100 == 0;
        if (gated) after.push_back(gate.job); // these cannot start before the gate opens
        const auto tier = random() % 3 ? JobTier::background : JobTier::frame;
        handles.push_back(system.submit(tier, [&](JobContext& context) {
            if (!context.cancelled()) ran.fetch_add(1, std::memory_order_relaxed);
        }, after));
        if (gated && random() % 2 == 0) {
            handles.back().cancel();
            cancelled_early.push_back(i);
        } else if (random() % 50 == 0) {
            handles.back().cancel(); // may already have run: any outcome but failure
        }
    }
    gate.release();
    auto counts = std::array<uint64_t, 5>{};
    for (const auto& h : handles) ++counts[size_t(h.wait())];
    CHECK(counts[size_t(JobStatus::failed)] == 0);
    CHECK(counts[size_t(JobStatus::succeeded)] + counts[size_t(JobStatus::cancelled)] == 100000);
    for (const auto i : cancelled_early) CHECK(handles[i].status() == JobStatus::cancelled);
    // Every success ran; a job cancelled after its body ran ends cancelled, so some that ran did not succeed.
    CHECK(ran >= counts[size_t(JobStatus::succeeded)]);
    const auto stats = system.stats();
    CHECK(stats.frame.queued + stats.background.queued == 0);
}

TEST_CASE("Frame work is not slowed by background work, and urgent jobs start at once", "[jobs][timing]") {
    auto system = JobSystem(JobSystemConfig{});
    const auto frame = [&] {
        const auto watch = Stopwatch{};
        system.parallel_for(JobTier::frame, 64, 1, [](uint32_t, uint32_t) { spin_for(0.1); });
        return watch.milliseconds();
    };
    const auto median = [&](auto&& measure) {
        auto samples = std::vector<double>{};
        for (int i = 0; i < 15; ++i) samples.push_back(measure());
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };
    frame();
    const auto idle = median(frame);
    // Streaming while playing: keep twice the background workers' count of 1 ms jobs queued.
    auto stop = std::atomic<bool>{false};
    auto feeder = std::thread([&] {
        auto queued = std::atomic<int>{0};
        while (!stop) {
            while (queued < system.workers(JobTier::background) * 2) {
                ++queued;
                system.submit(JobTier::background, [&queued](JobContext&) {
                    spin_for(1.0);
                    --queued;
                });
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        while (queued > 0) std::this_thread::yield();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto busy = median(frame);
    auto latencies = std::vector<double>{};
    for (int i = 0; i < 40; ++i) {
        const auto watch = std::make_shared<Stopwatch>();
        auto started = std::make_shared<double>(0.0);
        system.submit(JobTier::frame, [watch, started](JobContext&) { *started = watch->milliseconds(); }).wait();
        latencies.push_back(*started);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop = true;
    feeder.join();
    const auto urgent = summarize(latencies);
    INFO("frame " << idle << " ms idle, " << busy << " ms under background load; urgent start P50 " << urgent.p50 << " ms, P99 " << urgent.p99 << " ms");
    CHECK(busy <= idle * 1.5);
    CHECK(urgent.p99 <= 1.0);
}
