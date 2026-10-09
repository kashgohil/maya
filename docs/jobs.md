# Jobs

[#1061](https://work.rezee.app/kash/issues/1061) adds the process's job system, `MayaJobs` ([jobs.hpp](../include/maya/jobs/jobs.hpp)), as [#1060 decided](architecture/world-scale-decision.md#jobs-a-two-tier-pool). Physics, texture cooking, and environment cooking run on it. [Asynchronous loading](assets.md#asynchronous-loading) ([#1062](https://work.rezee.app/kash/issues/1062)) prepares assets on it, and generation will build on it.

## Two tiers

| Tier | Workers | Quality of service | Runs |
| --- | --- | --- | --- |
| `JobTier::frame` | one less than the hardware threads (13 on the M4 Pro) | user-interactive | physics, animation, extraction, parallel-fors: work a frame waits for |
| `JobTier::background` | the same | utility | reading, decoding, cooking, generation: work no frame waits for |

The tiers are separate threads, so a worker busy with a 2 ms decode never holds up frame work. The kernel preempts utility threads when user-interactive ones become ready. The [timing test](#tests) checks that frame work is not slowed by background work, and that urgent jobs start at once. `configure_job_system({frame, background})` sizes the process's system before its first use (tests make their own `JobSystem`). Workers are named `maya frame N` and `maya background N` in profilers.

## Jobs

`job_system().submit(tier, function, after)` queues `function(JobContext&)` once every job in `after` is done, and returns a `JobHandle`.
- **Status.** pending → running → succeeded, failed, or cancelled. `wait()` blocks until the job is done and returns its status; `error()` says why a job failed or was cancelled.
- **Failure.** A job fails by calling `context.fail(message)` or by throwing; the message reaches whoever waits, as a value. A job whose prerequisite failed does not run: it fails with "a prerequisite failed: …", and so on down a chain.
- **Cancellation** is cooperative. `cancel()` stops a job that has not started (it never runs), and a running job sees `context.cancelled()`, which it checks between steps. A cancelled job ends cancelled even if it returns normally; it never reports success. Dependents of a cancelled job are cancelled.
- **Waiting inside a job.** A worker that waits runs other jobs of its tier meanwhile, so jobs that wait for jobs cannot exhaust the tier.
- **Queues.** Each worker has its own queue; a job submitted from a worker goes to that worker's queue, others round-robin, and an idle worker takes from its own queue, then from the others', skipping empty ones without locking them.
- **Sleeping and waking.** A worker with nothing to do spins for a few microseconds (pause instructions, no system calls), so a burst of small jobs finds it awake; then it sleeps on its own lock and condition. To sleep, it sets its bit in the tier's sleeper mask and reads the tier's epoch, which every submission bumps, once more; a submission claims one sleeper's bit and signals only that worker. A worker whose bit a submission missed has seen the bumped epoch, so no wake-up is lost, and each sleeper is signalled at most once. `std::atomic::wait` is not used for this: libc++ keys those waits through a table shared by unrelated atomics, so its `notify_one` can wake the wrong thread, and a version that relied on it stranded jobs with every worker asleep.

`parallel_for(tier, count, grain, body)` calls `body(begin, end)` over chunks of `grain` on the tier's workers and the calling thread, and returns when every chunk is done. Chunks are claimed as workers become free, so a parallel-for inside a job of the same tier cannot deadlock. The first exception a chunk throws is rethrown after the rest finish.

## Rules

- Jobs never touch a World, never mutate the asset registry, and never encode GPU commands. They read what they were given and produce plain data.
- Results that the owner must apply (a decoded texture to upload, a cell to activate) go through a `CompletionQueue`.
- **Owners scope their jobs.** A `JobScope` cancels and waits for the jobs submitted through it when it is destroyed, so no job outlives the owner it refers to. An owner (a registry, a session, a world) destroys its scope before anything its jobs use.
- **Shutdown order.** The process's job system lives until exit. Owners are shut down in this order: stop submitting, cancel and wait for their scopes, then destroy the registry, the World, and the device. A `JobSystem` that is destroyed cancels every job: queued ones end cancelled without running, running ones see `cancelled()`, and the destructor joins the workers.

## Completions on the owner thread

A `CompletionQueue` belongs to an owner (#1062's registry, #1064's streamer). Jobs post through its `Sink` with a request and a generation, and the owner calls `drain(budget, current)` in its finalize phase.
- Completions are applied in the order they were posted, until the budget is spent; at least one is applied each call, so a queue never stalls.
- `current(request, generation)` decides whether a completion is still wanted. One for a request that was replaced, cancelled, or whose world closed is discarded without running, as the [streaming contract](architecture/runtime-world-contracts.md#streaming-boundary) requires.
- A sink outlives its queue safely: what it posts after the queue is gone is dropped.

## Physics

Jolt's jobs run on the frame tier through a `JPH::JobSystemWithBarrier` adapter ([jolt_runtime.cpp](../src/maya/physics/jolt_runtime.cpp)), replacing Jolt's own thread pool. `set_physics_worker_threads(n)` now limits how many frame workers a step uses at once: −1 is the frame tier's workers, at most 7 (as before); 0 runs every job on the stepping thread; larger counts are capped at the frame tier. Results are the same for any count ([physics](physics.md#process-wide-state-memory-and-limits)).

## Cooking

ASTC compression and environment prefiltering run as parallel-fors on the job system, with the calling thread taking part. astcenc's threads claim blocks as they arrive, so its thread indices run on however many pool threads are free. A `threads` of 1 cooks on the calling thread alone. The cooked bytes are the same whatever the tier or thread count (the R1 import's cache digests matched the code before #1061 exactly).

**A cook someone waits for runs on the frame tier**, the default for `TextureCookOptions::tier` and `cook_environment`'s `tier`: an explicit wait (`acquire`, `reload`, a loading screen) blocks the owner thread, so its cooks are frame work. On the background tier the R1 import's cold load took 80% longer (ABeautifulGame 15.0 s against 8.3 s), because macOS runs utility threads mostly on efficiency cores even on an idle machine. Background workers at a higher quality of service (user-initiated) would cook at full speed, but then frame work slowed 2.5–4× under background load in the [timing test](#tests), the property the tiers exist for. So the background tier stays at utility, and since [#1062](https://work.rezee.app/kash/issues/1062) the loads frames start (`request`, `try_acquire`) cook there, where nothing waits for them (`AssetCooker::set_tier`).

## Instruments

`job_system().stats()` reports, per tier: workers; jobs submitted, succeeded, failed, and cancelled; the time workers (and callers helping a parallel-for) spent in jobs; jobs queued now and the peak since `reset_peaks()`. It also reports, across every completion queue, the completions applied and discarded and the time spent draining. The benchmark runner records them over each benchmark, as `jobs` in its [results](performance.md#results), with jobs and busy time per frame.

## Tests

[jobs_tests.cpp](../tests/jobs_tests.cpp), CTest `maya_jobs` (cpu):
- both tiers run and count their jobs;
- dependencies run in order, in chains across tiers and fan-in, and a finished prerequisite counts as met;
- failures by `fail` and by exception reach the waiter and propagate through dependents;
- cancellation before a job starts (it never runs), while it runs (it sees the request and ends cancelled), and through a dependency;
- jobs that wait for jobs on a tier with two workers do not deadlock;
- parallel-for covers every index once from the owner thread and from inside a job, and rethrows a chunk's exception;
- a scope cancels and waits for its jobs before its owner goes;
- destroying a system with 400 dependent jobs in flight ends every one;
- completion queues apply in order, under a budget (at least one each call), discard stale completions, and drop posts after the queue is gone;
- bursts of 2,000 empty jobs on systems of 1, 2, 4, and 13 workers, 40 rounds each with pauses that let the workers fall asleep, each burst finishing by a deadline: a lost wake-up fails here rather than hanging (a mutation that claims a sleeper without signalling it fails in the first rounds);
- a stress run of 100,000 jobs with random dependencies and cancellation, where every job ends, none fails, and jobs cancelled before they could start never run.

`maya_jobs_timing` (cpu, run alone) holds the two-tier property: a frame of 64 parallel 0.1 ms chunks takes no more than 1.5× as long while background work fills every background worker, and urgent frame jobs start within 1 ms at P99. `maya_physics`' "[jobs]" case checks that physics steps on the frame tier, that zero workers keeps it on the stepping thread with the same results, and the worker cap.

Sanitizers: the jobs, physics, and asset tests pass under UBSan, and the jobs tests under Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`). ThreadSanitizer and AddressSanitizer do not run on the reference machine (with Xcode's clang 17 on macOS 26, TSan crashes at startup and ASan hangs, even for a trivial program), so data races are covered by the stress case and repeated runs (100 consecutive passes of the suite) until a toolchain with a working TSan is available.

## Measurements

Release on the M4 Pro reference machine (thermal state nominal), against a Release build of the code before #1061, two rounds each:

| | Before #1061 | Frame tier (the default) | Background tier |
| --- | --- | --- | --- |
| R1 import, cold load of ABeautifulGame (median of 3) | 8.31–8.38 s | 8.32–8.41 s | 15.0–15.1 s |
| R1 import, cold load of FlightHelmet | 3.92–3.93 s | 3.91–3.93 s | 6.70–6.80 s |
| Cook cache digests | — | identical | identical |

The final code (with the wake-up fix below) against the old, run back to back on a warmer machine: ABeautifulGame 8.53 and 8.94 s against 9.03 and 8.66 s, FlightHelmet 4.07 and 4.03 s against 4.36 and 4.07 s; equal within run-to-run noise.

| P1 (`p1_physics`, three runs per configuration) | Jolt's pool, before #1061 | The frame tier |
| --- | --- | --- |
| Tick, default workers (7): mean / P99 | 6.14–6.19 / 6.68–6.75 ms | 6.02–6.11 / 6.48–6.63 ms |
| Tick, no workers: mean / P99 | 11.6–12.0 / 13.1–15.2 ms | 11.4–11.6 / 13.0–13.2 ms |
| Final state | `77030e51…` | the same |

The final code in a later round: mean 6.16–6.23 ms and P99 6.69–6.92 ms against the old code's 6.22–6.27 and 6.77–6.85 ms in the same round, with the same final state; the budget is a P99 of 8.5 ms.

P1 uses about 27.5 frame-tier jobs a tick.

**Spawn cost.** Submitting 200,000 empty jobs in a burst and waiting for them costs 0.82–0.85 µs a job with 13 workers a tier, 0.29–0.36 µs with 4, and 0.15 µs with 1 (the #1060 prototype's single-lock pool: 2.4 µs). With many workers the cost is workers waking for jobs that last nanoseconds; jobs worth running on a pool are far longer. The two-tier timing test: a frame of 64 parallel 0.1 ms chunks takes 0.54–0.58 ms idle and 0.53–0.56 ms with background work on every background worker; with the background tier at user-initiated instead of utility, 1.3–2.5 ms.
