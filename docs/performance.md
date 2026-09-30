# Measuring performance

[Issue #1004](https://work.rezee.app/kash/issues/1004) adds the instruments the [performance baseline](architecture/performance-baseline.md) asked for:

- CPU timing scopes for each frame;
- the GPU's own execution time for each frame;
- per-frame draw, instance, and triangle counts;
- tracked resource and asset bytes, kept apart from the memory the platform reports;
- a bounded live display in the editor;
- a benchmark runner with versioned manifests and machine-readable results.

## Aggregation

`MayaMetrics` ([metrics.hpp](../include/maya/metrics/metrics.hpp)) is CPU-only and shared by the editor display and the benchmark runner.

- **Summaries.** `summarize(samples)` gives the count, mean, minimum, P50, P95, P99, and maximum. Percentiles use the declared nearest-rank rule: the `ceil(p × N)`-th smallest sample, one-based. Nothing is discarded as an outlier; a hitch is the maximum and drives the tail.
- **Throughput.** Throughput is sampled frames divided by total sampled seconds, not an average of instantaneous FPS.
- **Live windows.** `SampleWindow` keeps the most recent samples, 240 by default, for live displays.
- **Clock.** `Stopwatch` reads the monotonic `steady_clock`.

## What is measured

**CPU frame scopes.** `Engine::tick` times four exclusive scopes, which add up to the tick. It passes them to `Application::on_frame_timing` as a `FrameTiming`, together with the frame's submission serial and the wall interval since the previous frame.

| Scope | What it covers |
| --- | --- |
| `update` | `Application::on_update`: input, simulation, UI building |
| `wait` | `GraphicsDevice::begin_frame`: waiting for a frame slot (frames in flight) |
| `render` | `Application::on_render`: extraction and encoding |
| `submit` | `GraphicsDevice::end_frame`: presenting and committing |
| `interval` | Wall time between frames, including waiting for the display |

The editor splits `render` further into extraction, viewport encoding, and UI encoding. The benchmark splits a frame into simulation, wait, extraction, encoding, and submission.

**GPU time.** Metal records each frame command buffer's `GPUStartTime` and `GPUEndTime` when the frame completes, in its completion handler. `take_gpu_timings()` returns them as `GpuFrameTiming {frame, milliseconds}`, keyed by the frame's submission serial, so a sample is never matched to the wrong frame.

- They are measured on the GPU's timeline and never estimated from CPU submission time.
- `gpu_timing_supported()` is false on the null device, where GPU time is reported unavailable.
- At most 1,024 timings wait to be taken; older ones are dropped and counted.
- Per-pass GPU timing is not implemented and is reported unavailable. Frames contain few passes, so frame time is the useful figure for now.

**Counters.** `RhiStats` counts the current or most recent frame's passes, draws, instances, and triangles, as submitted. Triangles are the vertices or indices divided by three, times the instance count.

**Memory.** Two kinds are kept apart and never added together.

- **Tracked**, from descriptors:
  - live buffer bytes, and texture bytes (width × height × bytes per pixel);
  - bytes of destroyed resources awaiting GPU completion;
  - the device's own upload memory (per-frame bytes × frames in flight);
  - `AssetRegistry::residency()`: entries by state, resident mesh and material versions, versions also leased outside the registry, and mesh GPU and CPU (picking) bytes.
- **Platform-reported:**
  - `GraphicsDevice::reported_memory()`: Metal's `currentAllocatedSize` for the device;
  - `process_memory()`: the process's physical footprint, resident size, and peak resident size.

  On unified memory, GPU allocations are part of the process footprint.

**Identification.** `system_info()` reports:

- hardware model, CPU, core counts by kind, and memory;
- macOS version and build;
- the Metal device and whether its memory is unified;
- thermal state and low-power mode.

The benchmark also records its build: `git describe --dirty` revision, build type, sanitizers, and compiler. A dirty revision is followed by a hash of the uncommitted changes, so two builds with different changes have different names. These come from `cmake/build_info.cmake` at build time (`maya/core/build_info.hpp`, shared with [play recordings](play.md#recording-and-replay)), so they describe the binary that ran.

## In the editor

The Diagnostics panel's **Performance** section summarizes the last 240 frames, refreshed four times a second:

- the frame interval (mean, P95, P99, and FPS);
- mean CPU time per scope;
- extraction, viewport, and UI encoding;
- GPU time (mean, P95, P99), or why it is unavailable;
- this frame's draws, instances, triangles, and passes;
- tracked buffer, texture, upload, and pending bytes;
- platform-reported GPU and process memory;
- resident assets.

Hovering a row explains what it measures. These are live observations of an interactive session, not benchmark results.

## The benchmark runner

```bash
maya_benchmark benchmarks/i1_10k.benchmark results.json
```

`maya_benchmark` runs a manifest headless and offscreen on the default Metal device. It measures as foreground work: its thread's quality of service is user-interactive. Started from a script, a long run could otherwise be scheduled as background work partway through. It writes JSON results (by default `<name>.results.json`) and prints a summary. It exits with 0 when the benchmark completed, 1 when it failed (the results say why and keep what completed), and 2 for bad arguments or manifests. Nothing is presented, so display pacing does not apply. Each frame runs exactly one 60 Hz simulation tick: a fixed-workload throughput run, labelled as such, not a live wall-clock run.

### Manifests

A manifest is a small versioned text file. Its keys may appear in any order, each at most once, and `#` starts a comment line.

```text
maya-benchmark 1
name "i1-10k"
workload instances           # instances, scene, load_cycles, or play_cycles
project "../samples/basic_scene"
mesh 6d617961 2              # catalog IDs of the shared mesh and material
material 6d617961 11
count 10000
rotating 0.1                 # exactly round(0.1 × count) spin, chosen by seed
spacing 1.5
seed 990
camera 0 130 150 0 0 0       # position, then target
resolution 1920 1080
upload_mib 64                # per-frame upload memory; each instance takes 256 bytes
warmup 300
samples 3000
runs 3
overhead on                  # also a matched run without CPU scopes
```

| Workload | What it does |
| --- | --- |
| `instances` | I1. Generates `count` entities on a square grid sharing one mesh and one material, a seeded exact fraction spinning, and a camera and light. Each run is a fresh play session from the same scene, so every run replays the same ticks: warmup frames, then sampled frames. |
| `scene` | A project's saved scene (`scene`, or the startup scene) through its first camera, as the player runs it. |
| `load_cycles` | L1 load/unload. The generated scene is saved to a file. First, a malformed copy and a copy with a missing asset must be refused, leaving nothing behind. Then, for each cycle: load the file, start a play session, render `ticks` frames, stop, wait for the GPU, evict unused asset versions, and sample memory. |
| `play_cycles` | L1 play reset. Starts and stops play sessions from the same authored scene. The authored World must be unchanged afterwards. |

The seed selection uses SplitMix64, so it is the same on every machine. Generated scenes are built with the same World, scene, and asset APIs as authored content. Before a run, the generated scene is validated against the project's catalog, just as a scene file is.

### Results

The JSON holds:

- the manifest;
- the environment, build, and quality settings (resolution, formats, antialiasing, lighting, presentation, simulation), including the thermal state at the start and at the end and the measuring thread's quality of service. The text summary warns when either thermal state is not nominal;
- counters, including `centers_in_view`: instances whose origin projects into the view;
- baseline and resident memory, tracked and reported;
- for each run: throughput, summaries of the frame, of each CPU scope, and of GPU time, the number of GPU samples missing, and the raw samples, with `null` for a missing GPU sample;
- the uninstrumented run, and the overhead of instrumentation;
- for cycles: summaries after the ten warmup cycles, the slope of the process footprint in bytes per cycle, whether counts returned to the baseline, and every cycle;
- the rejected cases;
- `unavailable`: each metric that could not be measured, with the reason.

The protocol's three independent runs are the `runs` of one invocation, each a fresh session. Invoke the runner again to compare across processes.

## Thermal state

Results from a warm machine are not comparable with results from a cool one.

- **What #1005 saw.** Back-to-back runs over about 40 minutes moved the machine to the `fair` thermal state. `i1_100k` frames then rose steadily from 21 ms to 37 ms, while GPU time stayed flat. In two earlier attempts, frame time stepped up about fivefold partway through the third run and stayed there. The runner now records the thermal state at the end as well as the start.
- **Protocol.** Start each manifest on a machine in the `nominal` state, leave a pause between manifests, and keep attempts that were disturbed, recorded as such, rather than silently repeating them.

GPU timings are taken from the device every 64 frames in both the instrumented and uninstrumented runs. Before #1005 the uninstrumented run took them only at its end, so with more than 1,024 samples it lost the earliest.

## First observations

The milestone's baselines, measured on a clean commit, are in the [acceptance record](acceptance.md#baselines).

These are observations of one run of each manifest, not budgets.

- **Machine:** a Mac16,7 with an Apple M4 Pro (10 performance and 4 efficiency cores, a unified-memory GPU) and 24 GiB of memory, running macOS 26.6.2 (25G83). The thermal state was nominal and low-power mode was off.
- **Build:** Release, at revision `9995e00` plus the uncommitted #1004 changes.
- **Rendering:** 1920×1080 offscreen, one tick per frame, with 300 warmup and 3,000 sampled frames per run, and three runs. The frame time is the CPU time from the tick to submission. The GPU time is the frame's GPU execution time.

| Manifest | Frame mean / P95 / P99 (ms) | GPU mean / P95 (ms) | Draws, triangles | Unique meshes, materials |
| --- | --- | --- | --- | --- |
| `sample` | 0.04 / 0.10 / 0.13 | 0.04 / 0.08 | 4, 42 | 2, 4 |
| `i1_1k` | 0.22–0.23 / 0.24 / 0.25 | 0.10–0.18 / 0.17–0.30 | 1,000, 12,000 | 1, 1 |
| `i1_10k` | 2.04–2.05 / 2.08–2.11 / 2.12–2.21 | 1.10–1.15 / 1.21 | 10,000, 120,000 | 1, 1 |
| `i1_10k_subset` | 2.03 / 2.06–2.08 / 2.13–2.17 | 0.99–1.09 / 1.15 | 10,000, 120,000 (3,310 in view) | 1, 1 |
| `i1_100k` | 20.9–21.1 / 21.1–21.9 / 21.4–22.7 | 6.6–7.1 / 8.9–9.1 | 100,000, 1,200,000 | 1, 1 |

Ranges are across the three runs.

- **Missing GPU samples.** None of the runs lost a GPU sample.
- **Where the CPU time goes.** At 10,000 instances, a frame spends 0.14 ms simulating, 0.47 ms extracting, and 1.43 ms encoding. At 100,000 instances it spends 1.9 ms, 4.7 ms, and 14.2 ms.
  - Encoding is one draw with its own uploaded constants per instance.
  - Without culling, the subset view costs the same as the full one.
- **Instrumentation overhead.** The matched runs without CPU scopes differed by −3.0% to +0.5% of the frame, in both directions. That is within the variation between runs, so no overhead was measurable.
- **Asset sharing.** In every I1 case the counters match the known sharing: one resident mesh (1,680 bytes in two buffers) and one material, whatever the instance count. The upload high water is 256 bytes per instance.
- **Memory.** Tracked textures are the 1920×1080 view target (15.8 MiB). Tracked upload memory is 192 MiB, or 384 MiB for `i1_100k`. Metal reports 209 MiB for the device (401 MiB for `i1_100k`), which is mostly that upload memory.

| Cycles | Load or start (ms, mean / P95) | First frame (ms) | Stop (ms) | Back to baseline | Footprint |
| --- | --- | --- | --- | --- | --- |
| `l1_load` (100 × 10,000 entities from a file) | 37.3 / 38.5 | 2.42 | 0.12 | buffers, textures, pending retirements, leases, resident meshes | 218 MiB empty; 418–432 MiB after every cycle; slope −25 KB/cycle |
| `l1_play` (100 play sessions from the authored scene) | 6.6 / 6.8 | 2.34 | 0.12 | the same | 218 MiB empty; 408–417 MiB; slope −18 KB/cycle |

- **Refusals.** The malformed and missing-asset scenes were refused, with their line and asset ID, and nothing was left behind.
- **Authored scene.** It was unchanged after 100 play sessions.
- **Load times vary between invocations.** An earlier run of the same binary measured 30.8 ms per load.
- **Retained footprint, not attributed.**
  - The footprint rises by about 200 MiB during the first cycle and then stays flat; its slope over cycles 11–100 is slightly negative.
  - The tracked counters are back at their baseline after every cycle: no buffers, no leases, and no resident meshes.
  - The retained memory is therefore not engine allocations the counters know about. It is consistent with allocator and driver retention.
  - Attributing it needs a memory profiler; it is recorded here, not called a leak or ignored.
- **Results files.** The JSON files, with raw samples, are not committed; the manifests are, and running them reproduces the files.

## Tests

- [metrics_tests.cpp](../tests/metrics_tests.cpp) checks aggregation on known samples:
  - nearest-rank percentiles for 1–100 and for ten samples;
  - a hitch driving P99;
  - one sample, duplicates, empty sets, and out-of-range p;
  - windows that keep the most recent samples;
  - the monotonic stopwatch.
- [rhi_validation_tests.cpp](../tests/rhi_validation_tests.cpp) covers:
  - per-frame pass, draw, instance, and triangle counts, with refused draws not counted and a reset each frame;
  - tracked bytes for buffers, textures, pending retirements, and upload memory;
  - GPU time and platform memory being unavailable on the null device;
  - the bound on waiting GPU timings.
- [rhi_tests.cpp](../tests/rhi_tests.cpp) checks that Metal reports one positive GPU time per completed frame, keyed by serial, and an allocated size that covers what was tracked.
- [asset_tests.cpp](../tests/asset_tests.cpp) covers registry residency (states, resident versions, outside leases, and mesh bytes) through eviction.
- [engine_tests.cpp](../tests/engine_tests.cpp) checks that frame timing parts add up to the tick, that serials and intervals are right, and that a new session starts without an interval.
- [benchmark_tests.cpp](../tests/benchmark_tests.cpp) runs the runner on the null device:
  - **Manifests:** every key, defaults, and each mistake with its line.
  - **Counters against known sharing:** 50 instances give 50 draws and 600 triangles, with one mesh (two buffers) and one material; the spinning fraction is exact for any seed.
  - **Samples:** per-frame counts, missing GPU samples as `null`, and the uninstrumented run.
  - **Failures:** an upload overflow fails with its reason.
  - **Cycles:** load cycles return to the baseline after eviction and refuse the malformed and missing-asset scenes; play cycles leave the authored scene unchanged.
  - **JSON:** complete and balanced.
- CTest runs the smoke manifests in [benchmarks/smoke](../benchmarks/smoke) on Metal and checks the runner's refusal of a file that is not a manifest.
