# Performance baseline and workload proposals

Status: proposed experiment plan for [#990](https://work.rezee.app/kash/issues/990), to implement and measure in [#1004](https://work.rezee.app/kash/issues/1004) and validate end-to-end in [#1005](https://work.rezee.app/kash/issues/1005). **No timings, scale claims, or production budgets are established by this record.** The existing smoke tests check lifecycle, not visual correctness or performance.

## Candidate measurement envelope

Use available hardware matching these candidate tiers; these are not purchase recommendations or minimum shipping specifications. Record the exact machine before comparing runs. If a tier is unavailable, mark it unmeasured rather than substituting an unnamed machine.

| Profile | Proposed hardware / output | Purpose |
| --- | --- | --- |
| Baseline | Apple M1, 8-core GPU, 16 GiB unified memory; 1920×1080 framebuffer | Constrained macOS/Metal baseline for the first authorable runtime. |
| Headroom | Apple M2 Pro, 16-core GPU, 32 GiB unified memory; 2560×1440 framebuffer | Separate higher-resolution/capacity comparison; not interchangeable with the baseline. |

Both use native resolution, no dynamic resolution, one view, and the milestone's current directional-light/material path. Record depth/color formats, AA, texture settings, shadow/PBR support, VSync/present mode, and actual drawable dimensions. Unsupported features are recorded as unavailable, not silently enabled in a quality label. A later realistic PBR/shadow reference scene gets a new version and separate baseline.

Candidate interactive target: 60 presented frames/second (16.67 ms frame interval) and 60 fixed simulation ticks/second. This is a proposal to evaluate. CPU, GPU, P95/P99, memory, activation, loading, and iteration budgets are **unallocated** until representative content and hardware are approved. Do not interpret the 16.67 ms figure as a guarantee or allocate 16.67 ms separately to every subsystem.

## Repeatable workloads

Each workload has a versioned manifest: seed `990`, entity/asset counts, assets and hashes, camera poses/path, light/material parameters, render settings, input/tick sequence, warmup/sample lengths, and termination condition. Commit small generators/manifests alongside implementation; do not hide content changes behind an unchanged benchmark name.

| Workload | Proposed recipe | Measurements and correctness checks |
| --- | --- | --- |
| V1: visual reference | A 20 m room with a floor, shared cubes/pyramids, overlapping near/far objects, a camera, one directional light, and distinct material colors. Include rotated nonuniform scale and parent/child transforms. Capture named fixed views and a fixed 600-tick camera path. | Compare depth/occlusion, material independence, aspect ratio, transform/normal correctness, and editor/player view equivalence. Use reference images with a declared per-platform tolerance; inspect initial references before blessing them. This is a foundation visual test, not the realistic-rendering quality bar. |
| I1: repeated instances | One indexed cube mesh (12 triangles), one material and one 256×256 texture, shared by 1,000 / 10,000 / 100,000 entities on a seeded grid. Only a deterministic 10% rotate each tick. Record separate views with all instances in the frustum and with a fixed subset in the frustum; save the expected visible counts. | World update, extraction, submission, GPU time, draw/instance/triangle counts, component bytes, unique asset bytes. Instance count may grow; unique mesh/material/texture allocations must not grow per entity. 100,000 is a stress input, not a promised capacity. Log capability-dependent culling/batching results. |
| L1: load/unload and play reset | Load a versioned 10,000-entity I1 scene, render 120 ticks, unload/stop, retire GPU work, sample memory; repeat 100 cycles. Separately enter/exit play from the same authored scene 100 times. Include malformed scene and missing-asset cases. | Load/decode/finalization time, longest activation/removal frame, peak/resident bytes, outstanding jobs/leases/resources, stale-handle rejection, and authored-state equality after play. Compare a steady-state plateau after warmup; allocator/cache retention must be attributed, not automatically called a leak or ignored. |
| S1: future streaming/physics extension | Version a fixed traversal across at least three cells, with one persistent cross-cell reference, delayed/cancelled load completions, and an explicit body/query workload. Repeat crossings and unloads. | Cell activation budgets, cancelled work, unresolved references, contact stability and memory pressure. Recipe, cell extent, velocity, active/sleeping body mix, constraints, queries, and error tolerances must be fixed when these systems exist. This is not a prerequisite benchmark implementation for the authorable-runtime milestone. |

The current two-object sample remains a fast regression case. It cannot stand in for I1 or L1. Scale the workload down explicitly if resource limits are reached, preserve the failure record, and report the largest completed case; never silently change density to achieve a target. Benchmark generation must use the same world/asset APIs as authored content.

## Measurement protocol

1. Build Release without sanitizers; preserve commit, compiler, SDK, OS, CMake options, architecture, and instrumentation configuration. Correctness/sanitizer runs are separate evidence. Record hardware model, CPU/GPU core counts, memory capacity, power/thermal conditions, resolution, and background load.
2. Fix asset hashes, scene/seed, view path, quality, input, and simulation policy. Run player and editor separately; an editor run records panel/view count and preview overhead. The current `--smoke N` interface does not configure this benchmark protocol; #1004 supplies a runner/manifest interface.
3. For V1/I1 steady-state measurements, propose 300 warmup frames followed by 3,000 sampled frames and three independent runs. Replay the same tick/camera sequence; label this fixed-workload throughput mode separately from a live wall-clock run that exercises catch-up/dropped-time behavior. Do not discard sampled hitches as outliers. Record failed/incomplete runs.
4. Separate resident-content runs from first-load/cold-cache attempts. State which application caches were cleared; never claim a cold OS/disk cache without controlling it. For L1, retain all 100 cycle samples; use cycles 1–10 as warmup and compare later cycles for trend. Report load and post-retirement memory independently.
5. Measure CPU scopes with a monotonic clock: command commit, simulation (when implemented), transform propagation, extraction, render encoding/submission, asset finalization, and waits. Name inclusive/exclusive scopes and worker overlap so totals are not misleading.
6. Measure GPU frame/pass execution asynchronously against the relevant submission ID. Do not substitute CPU command-encoding or `end_frame` duration. Unsupported timing is null/unavailable with a reason. Track samples lost through unavailable/disjoint timing separately. Report present pacing and CPU/GPU waits independently from execution cost.
7. Report count, arithmetic mean, P50, P95, P99, and maximum for frame, subsystem, and load/cycle timings. Use a declared nearest-rank percentile on sorted samples (`ceil(p × N)`, one-based). Compute throughput FPS as sampled frames / total sampled seconds, not the average of instantaneous FPS. Keep raw samples and each run's summaries; do not pool unlike profiles.
8. Report live/peak entity and component counts, unique asset versions, active/pending GPU resource counts and descriptor/allocation bytes, in-flight bytes, cache retention, and process memory separately. On unified memory, tracked GPU bytes and process memory may overlap: never sum them into a fictitious total. Label allocation estimates versus platform-reported residency.
9. After L1 unload, drain work and sample against a warmed empty-session baseline. Live world/component/lease counts should return to the declared baseline. Bounded caches must have explicit policy/bytes; distinguish them from unexplained monotonic growth. Include both live-memory snapshots and peak usage while content is resident.
10. Measure instrumentation overhead with matched enabled/disabled runs and report it. Save the manifest, raw samples, aggregate report, visual captures, errors, and unavailable metrics together. Reject baseline comparisons when the workload or critical configuration differs unless the difference is the experiment itself.

Future import/cook and edit-to-preview measurements follow the same protocol: identify source assets, cache state, edit action, start/end event, and result readiness. Report them as unavailable until implemented; scene-load latency is not a substitute for import time.

## Budget decision record

The initial report should contain the following fields even when unresolved:

| Budget / envelope | Current status | Evidence needed to approve it |
| --- | --- | --- |
| Platforms, hardware, output quality | macOS/Metal implementation base; two proposed profiles above | Product/platform choice and representative content on named machines. |
| Frame pacing and simulation rate | Proposed 60 fps / 60 Hz | P95/P99 frame pacing, input latency, solver stability, sustained thermal runs. |
| CPU/GPU subsystem limits and headroom | Unallocated | #1004 timing breakdown, overlap/waits, and expected additional physics/render features. |
| Memory and residency ceiling | Unallocated | I1/L1 live/peak attribution, available device memory and other process/system demands. |
| Load, activation, unload, edit-preview limits | Unallocated | Timed L1 and authoring traces; acceptable user-visible stalls and pending-work limits. |
| World extent / positional accuracy | Unresolved; local float transforms only | Sweep origin offsets (for example 0, 100 m, 1 km, 10 km), measure camera/picking/physics error, then choose tolerances and coordinate strategy. The sweep is an experiment, not a supported range. |
| Physics, procedural, cinematic scale | Unresolved | Representative authored slice with fixed body/query/generator/capture recipes. |

An approved budget must record workload version, hardware profile, quality/resolution, numerical limit, measurement method, rationale, and the review decision. Until then, results are observations. Regression thresholds and image tolerances are established from repeated baseline variance and review, not invented from a single run.
