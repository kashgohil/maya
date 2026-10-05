# Measuring performance

[Issue #1004](https://work.rezee.app/kash/issues/1004) adds the instruments the [performance baseline](architecture/performance-baseline.md) asked for:

- CPU timing scopes for each frame;
- the GPU's own execution time for each frame;
- per-frame draw, instance, and triangle counts;
- tracked resource and asset bytes, kept apart from the memory the platform reports;
- a bounded live display in the editor;
- a benchmark runner with versioned manifests and machine-readable results.

[Issue #1026](https://work.rezee.app/kash/issues/1026) adds each render pass's GPU time and a presenting benchmark mode that measures display pacing.

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

**GPU time per pass** (#1026). Each frame timing also carries its passes (`GpuFrameTiming::passes`, `GpuPassTiming`), in encoding order:

- **What is sampled.** Metal writes GPU timestamps at each pass's stage boundaries (`MTLCounterSamplingPointAtStageBoundary`): the start and end of its vertex stage and of its fragment stage. That is the finest point Apple GPUs sample; they cannot sample at draws. Four samples a pass go into a timestamp sample buffer, one buffer per frame slot.
- **What a pass's time is.** On Apple's tile-based GPUs, a later pass's vertex stage runs while an earlier pass's fragment stage is still working, and the fragment stages run one after another. So the span from a pass's first sample to its last includes waiting for earlier passes. A pass's time is instead its vertex stage plus its fragment stage (`vertex_ms`, `fragment_ms`, `milliseconds()`), each stage's own interval. `start_ms` and `end_ms` place the pass within the frame, from the frame's GPU start.
- **Checked against the frame.** The samples are on the clock of the command buffer's `GPUStartTime` and `GPUEndTime` (converted with `MTLDevice::sampleTimestamps`). Every pass lies inside its frame's GPU execution, and the fragment stages add up to no more than the frame. Because vertex stages overlap, pass times can add up to slightly more than the frame. A pass outside its frame would be a measurement fault: the benchmark counts such passes (`gpu_pass_mismatches`), and the tests require none.
- **Names.** A pass is named by `RenderPassDesc::label`, or `pass N` (from 1) without one: the renderer's `view` and `present view`, the editor's `editor ui` and `texture thumbnail`.
- **Never estimated.** `gpu_pass_timing_supported()` is false, with `gpu_pass_timing_unavailable()` saying why, on the null device ("executes no GPU work"), on a GPU without stage-boundary timestamps, or when turned off. Then frames carry no passes.
- **Limits and switch.** At most `max_timed_passes` (64) passes a frame are timed; later ones are counted in `untimed_passes`. `set_gpu_pass_timing(false)` turns timing off from the next frame, for matched runs.
- **When.** Samples are resolved on the device's thread once their frame is known complete: when its timing is taken, or just before its slot is reused, frames in flight later.

**Display pacing** (#1026). A frame presented to a window reports when the display showed it (`take_present_timings()`, `PresentTiming`): Metal's drawable `presentedTime`, on the host clock, or never, when the drawable was replaced before it could be shown. Metal occasionally reports nothing at all for a presented drawable; the benchmark counts such frames as unreported. `display_refresh_rate()` is the window's screen's maximum refresh rate. Both are unavailable headless and on the null device.

**Counters.** `RhiStats` counts the current or most recent frame's passes, draws, instances, and triangles, as submitted. Triangles are the vertices or indices divided by three, times the instance count.

**Memory.** Two kinds are kept apart and never added together.

- **Tracked**, from descriptors:
  - live buffer bytes, and texture bytes (every mip level, compressed blocks included: `texture_bytes`);
  - bytes of destroyed resources awaiting GPU completion;
  - the device's own upload memory (per-frame bytes × frames in flight);
  - `AssetRegistry::residency()`: entries by state, resident mesh, material, and texture versions, versions also leased outside the registry, mesh GPU and CPU (picking) bytes, and texture GPU bytes (#1031; `resident_textures` and `texture_gpu_bytes` in results).
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
- each pass's GPU time (mean and P95, slowest first, passes with the same name summed per frame), or why pass times are unavailable;
- this frame's draws, instances, triangles, and passes;
- tracked buffer, texture, upload, and pending bytes;
- platform-reported GPU and process memory;
- resident assets.

Hovering a row explains what it measures. These are live observations of an interactive session, not benchmark results.

## The benchmark runner

```bash
maya_benchmark benchmarks/i1_10k.benchmark results.json
```

`maya_benchmark` runs a manifest headless and offscreen on the default Metal device. It measures as foreground work: its thread's quality of service is user-interactive. Started from a script, a long run could otherwise be scheduled as background work partway through. It writes JSON results (by default `<name>.results.json`) and prints a summary. It exits with 0 when the benchmark completed, 1 when it failed (the results say why and keep what completed), and 2 for bad arguments or manifests. Each frame runs exactly one 60 Hz simulation tick: a fixed-workload throughput run, labelled as such, not a live wall-clock run.

**Presenting runs** (#1026). With `present on`, the runner opens a window whose framebuffer is the manifest's resolution and also presents every frame's view into it, 1:1, synchronized with the display. The window floats in front of other windows, and the display is kept from sleeping, for the run: a hidden window or a sleeping display shows nothing, and the text summary warns about any frame that was not shown. The window's events are handled between frames, outside their timing. Each sampled frame's display time is recorded. After the last frame, the runner waits up to a second for the display to report on every sampled frame. The results are labelled as presenting, and offscreen runs say that pacing does not apply. Presenting runs are paced by the display, so their frame times include waiting for a drawable; compare them with each other, not with offscreen runs. [sample_present](../benchmarks/sample_present.benchmark) and [i1_10k_present](../benchmarks/i1_10k_present.benchmark) present the same frames as `sample` and `i1_10k`.

### Manifests

A manifest is a small versioned text file. Its keys may appear in any order, each at most once, and `#` starts a comment line.

```text
maya-benchmark 1
name "i1-10k"
workload instances           # instances, scene, load_cycles, play_cycles, physics, or import
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
overhead on                  # also a matched run without CPU scopes or GPU pass timing
present off                  # on: present every frame to a window and measure display pacing
```

| Workload | What it does |
| --- | --- |
| `instances` | I1. Generates `count` entities on a square grid sharing one mesh and one material, a seeded exact fraction spinning, and a camera and light. Each run is a fresh play session from the same scene, so every run replays the same ticks: warmup frames, then sampled frames. |
| `scene` | A project's saved scene (`scene`, or the startup scene) through its first camera, as the player runs it. |
| `load_cycles` | L1 load/unload. The generated scene is saved to a file. First, a malformed copy and a copy with a missing asset must be refused, leaving nothing behind. Then, for each cycle: load the file, start a play session, render `ticks` frames, stop, wait for the GPU, evict unused asset versions, and sample memory. |
| `play_cycles` | L1 play reset. Starts and stops play sessions from the same authored scene. The authored World must be unchanged afterwards. |
| `physics` | P1 physics stress ([recipe](architecture/performance-baseline.md#p1-physics-stress), #1024). Generates the P1 scene and ticks it back to back, headless: no project, views, or device work. Each worker configuration runs `runs` times. |
| `import` | glTF import and cooking ([below](#import), #1036). No project or views: each model is imported into a new project and loaded into the device, cold and warm. |

Cycle workloads take `cycles`, `ticks` (frames per cycle), and `slope_from`, the first cycle of the footprint slope's fit (default 11). The L1 manifests run 300 cycles and fit from cycle 101, once allocator warm-up has finished (#1024).

The physics workload takes these keys instead of the mesh, material, camera, and resolution:

```text
workload physics
count 5000                   # dynamic bodies: 40% dropped into the bin, 60% resting in patches
obstacles 500
scripted 500                 # bodies of the dropped set that carry the P1 script
sensors 50
queries 1000 100 20          # each tick: closest-hit rays, sphere overlaps, box casts
workers default 0            # physics worker threads, one configuration each; default is the engine's
warmup 300                   # ticks
samples 3000
runs 3
```

### Import

The import workload ([import_workload.cpp](../apps/benchmark/import_workload.cpp)) takes the folder of the content and its models instead of a project, the mesh, material, camera, and resolution:

```text
workload import
content "../build/render-samples"   # from tools/fetch_render_samples.sh
models "Models/ABeautifulGame/glTF-Binary/ABeautifulGame.glb" "Models/FlightHelmet/glTF/FlightHelmet.gltf"
runs 3
```

For each run and model, it copies the model (and the files a `.gltf` names) into a new project in the temporary folder, then measures, each in a new session: **import** ([import_gltf](import.md)); **cold load**, every mesh, texture, and material the import cataloged loaded through an empty [cook cache](assets.md#cook-cache), so cooked and written; and **warm load**, the same from the cache. It records the counts, triangles, texture GPU bytes, the cache's size and hits, and a digest of every cache entry. The run **fails** when runs cooked different bytes, or when a warm load missed the cache. The text summary gives each model's median. [r1_import](../benchmarks/r1_import.benchmark) runs R1's hero content; its results are in the [import doc](import.md#cost). The OS file cache is not controlled, so "cold" means an empty cook cache, not unread files.

The seed selection uses SplitMix64, so it is the same on every machine. Generated scenes are built with the same World, scene, and asset APIs as authored content. Before a run, the generated scene is validated against the project's catalog, just as a scene file is.

### Results

The JSON holds:

- the manifest;
- the environment, build, and quality settings (resolution, formats, antialiasing, lighting, presentation, simulation), including the thermal state at the start and at the end and the measuring thread's quality of service. The text summary warns when either thermal state is not nominal;
- counters, including `centers_in_view`: instances whose origin projects into the view;
- baseline and resident memory, tracked and reported;
- for each run: throughput, summaries of the frame, of each CPU scope, and of GPU time, the number of GPU samples missing, and the raw samples, with `null` for a missing GPU sample;
- for each run, per pass name: a summary of the GPU time of that frame's passes with the name (`gpu_pass_ms`), with the raw values; `gpu_pass_mismatches`, timed passes outside their frame's GPU time, which must be 0; and `untimed_passes`;
- for presenting runs, under `presentation`: frames `shown`, `not_shown`, and `unreported`, the display's `refresh_hz`, a summary of present-to-present intervals of consecutive shown frames (`interval_ms`, raw in `present_interval_ms`), and `missed_deadlines`, intervals longer than 1.5 refresh periods. A frame that was never shown makes the interval across it two periods, so it counts as missed. `null` when the refresh rate is unknown. ProMotion displays may run below their maximum rate when the system chooses; the rate recorded is the maximum;
- the uninstrumented run (no CPU scopes and no GPU pass timing), and the overhead of instrumentation on the CPU frame and on GPU time;
- for cycles: summaries after the ten warmup cycles, the slope of the process footprint in bytes per cycle over `footprint_slope_cycles` (from `slope_from` to the last), whether counts returned to the baseline, and every cycle;
- for physics, under `physics`: the recipe, the scene's counts, and for each run its worker threads, start time, and summaries per tick of the whole tick and each part (scripts, queries, other systems, body preparation, the step, synchronization, events, post-physics hooks, and the body commit), of active and sleeping bodies, touching pairs, solid contacts, events, and query hits; Jolt's heap at the start and end and its peak, the per-step scratch allocator's high water and capacity, the script VM's bytes, the process footprint at the start and end and its slope per tick, the state hash after the last tick, and the raw tick times. `deterministic` says whether every run and configuration ended in the same state; a mismatch, or a step that hit a physics limit, fails the benchmark;
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

### Pass times and pacing (#1026)

Observations, not budgets. The same machine and macOS, in the `nominal` thermal state throughout; a Release build of the uncommitted #1026 changes; 300 warmup and 3,000 sampled frames per run, three runs, at 1920×1080.

| Manifest | Frame mean (ms) | GPU mean (ms) | GPU per pass, mean (ms) | Overhead: CPU frame, GPU |
| --- | --- | --- | --- | --- |
| `sample`, offscreen | 0.040–0.042 | 0.043–0.046 | `view` 0.036–0.038 | +2.8%, +2.5% |
| `i1_10k`, offscreen | 2.12–2.14 | 0.77–0.91 | `view` 0.76–0.90 | −1.1%, −5.3% |
| `sample_present` | 8.30–8.31 | 0.25–0.27 | `view` 0.16, `present view` 0.11–0.13 | 0.0%, +5.1% |
| `i1_10k_present` | 8.30–8.34 | 2.00–2.05 | `view` 1.34–1.37, `present view` 0.65–0.68 | −0.1%, −4.4% |

- **Instrumentation overhead.** The matched runs without CPU scopes and pass timing differ in both directions, within the variation between runs, so no overhead of pass timing is measurable.
- **Every pass inside its frame.** No run had a pass outside its frame's GPU time, an untimed pass, or a missing GPU sample.
- **Presented frames, at 120 Hz.** In five of the six presenting runs every frame was shown. Present-to-present intervals averaged 8.34–8.36 ms with P95 and P99 at 8.333 ms, and 2–11 of 3,000 frames missed their deadline, nearly all by one refresh (16.7 ms intervals). In the sixth (`i1_10k_present`, run 3) four frames were never shown and one gap lasted 5.3 s while the window was disturbed; the runner flagged it as not valid.
- **Presenting runs are paced by the display.** Their frames take a refresh period, and their GPU times are higher than offscreen ones for the same work (`i1_10k`'s view pass 1.35 ms against 0.83 ms). Apple GPUs run lighter, paced work at lower clocks, so compare presenting runs only with each other.
- **Valid presenting runs need a visible window and a display that stays awake.** Earlier attempts while the display slept, or with the window behind others, reported most frames as never shown. The runner now keeps the display awake and its window in front, and warns about any frame that was not shown.

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
- [rhi_tests.cpp](../tests/rhi_tests.cpp) checks that Metal reports one positive GPU time per completed frame, keyed by serial, and an allocated size that covers what was tracked. Since #1026 it times three named and unnamed passes over twelve frames, taken only at the end, so most are resolved as their slots are reused. Every pass lies inside its frame and they finish in encoding order. The fragment stages add up to no more than the frame. Passes beyond the limit are counted, and turning timing off leaves frames without passes.
- [rhi_validation_tests.cpp](../tests/rhi_validation_tests.cpp) (#1026) checks the reasons pass and present timing are unavailable on the null device, and, with a scripted timing device ([timing_device.hpp](../tests/support/timing_device.hpp)), which passes a backend is asked to time, the limit, turning timing off from the next frame, present reports for shown and never-shown frames, and the bound on waiting present reports.
- [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) (#1026) presents 30 frames to a real window: each presented frame reports once (Metal occasionally misses one), the shown ones in order, a typical refresh period apart.
- [asset_tests.cpp](../tests/asset_tests.cpp) covers registry residency (states, resident versions, outside leases, and mesh bytes) through eviction.
- [engine_tests.cpp](../tests/engine_tests.cpp) checks that frame timing parts add up to the tick, that serials and intervals are right, and that a new session starts without an interval.
- [benchmark_tests.cpp](../tests/benchmark_tests.cpp) runs the runner on the null device:
  - **Manifests:** every key, defaults, and each mistake with its line.
  - **Counters against known sharing:** 50 instances give 50 draws and 600 triangles, with one mesh (two buffers) and one material; the spinning fraction is exact for any seed.
  - **Samples:** per-frame counts, missing GPU samples as `null`, and the uninstrumented run.
  - **Failures:** an upload overflow fails with its reason.
  - **Cycles:** load cycles return to the baseline after eviction and refuse the malformed and missing-asset scenes; play cycles leave the authored scene unchanged; the footprint is fitted from `slope_from`.
  - **Physics (#1024):** the physics keys; the recipe's proportions and a repeatable scene; a small run with and without workers that counts every part, renders nothing, and ends every run in the same state.
  - **JSON:** complete and balanced.
  - **Passes (#1026):** per-pass times summed by name for every sampled frame, none in the matched run, pass timing turned back on afterwards, passes outside their frame counted and flagged, and the reason when a device cannot time passes.
  - **Presenting (#1026):** the window's events every frame; shown, never-shown, and unreported frames; intervals and a missed deadline across a dropped frame; refusal without a window; and offscreen runs saying pacing does not apply.
- CTest runs the smoke manifests in [benchmarks/smoke](../benchmarks/smoke) on Metal, including `sample_present`, which presents to a window, and [p1_small](../benchmarks/p1_small.benchmark), which must end every run in the same state. It also checks the runner's refusal of a file that is not a manifest.
