# Performance baseline and workload proposals

Status: proposed experiment plan for [#990](https://work.rezee.app/kash/issues/990), to validate end-to-end in [#1005](https://work.rezee.app/kash/issues/1005). [#1004](../performance.md) implements the instruments, the runner, and manifests for the sample, I1, and L1; its first observations are recorded there. #1015 specifies the [physics stress workload P1](#p1-physics-stress). #1005 validated the milestone and recorded [regression budgets for the M4 Pro reference runs](../acceptance.md#budgets), approved on 29 September 2026. **No production budgets are established by this record, and those regression budgets are not production budgets.** The existing smoke tests check lifecycle, not visual correctness or performance.

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

## P1: physics stress

Specified for [#1015](https://work.rezee.app/kash/issues/1015) and measured in [#1024](https://work.rezee.app/kash/issues/1024), once MayaPhysics (#1017), bodies and colliders (#1019), scripts (#1018), and queries and events (#1021) exist. It is the "physics stress scene" of DOC-58's scale acceptance. It follows the protocol below. The benchmark runner builds the scene through the same World and component APIs as authored content, then runs it headless with no views, so it measures simulation, not rendering.

**Recipe, version 1** (manifest `p1_physics`, seed `990`). Any change to a count, shape, placement rule, or setting makes a new version.

| Part | Content |
| --- | --- |
| Ground | One static box, 200 × 1 × 200 m, top face at y = 0. |
| Static obstacles | 500 static boxes. Each axis is 0.5–3 m, yaw is uniform, and positions are seeded in the 160 × 160 m field outside the bin. |
| Bin | Four static walls, 4 m high, around a 20 × 20 m square at the origin. A kinematic paddle (an 18 × 1 × 0.5 m box, 0.5 m above the floor) turns about Y at 1 rad/s and keeps the bin's bodies awake. |
| Dynamic bodies | 5,000 in all: 60% 1 m boxes, 25% spheres of radius 0.5 m, and 15% capsules of radius 0.3 m with half height 0.5 m. Density 1,000 kg/m³, friction 0.5, restitution 0. **Active set:** 2,000 dropped into the bin from a seeded grid above it. **Sleeping set:** 3,000 laid in 30 single-layer 10 × 10 patches resting on the ground across the field. |
| Scripts | 500 bodies of the active set carry one script. Its `fixed_update` reads its body's velocity and adds a small force toward the bin's centre. Its `on_contact_begin` counts contacts. |
| Sensors | 50 sensor boxes (2 m) at seeded places in the bin. |
| Collision groups | `Static`, `Dynamic`, and `Trigger`. Static collides with Dynamic, Dynamic with Dynamic, and Trigger with Dynamic. |
| Queries each tick | Generated from the seed and the tick index, so they vary but repeat. **Raycasts:** 1,000 closest-hit rays up to 100 m, from seeded points on a 60 m sphere around the bin (5–30 m high) toward seeded points in the bin. **Overlaps:** 100 sphere overlaps (radius 2 m) in the bin. **Shape casts:** 20 box casts (1 m cube, 20 m long). |
| Settings | 60 Hz, one collision step, gravity (0, −9.81, 0) m/s². Physics worker threads: the default (recorded), and 0 in a second configuration. |
| Run | 300 warmup ticks (the drop settles into steady churn), 3,000 sampled ticks, three runs per configuration. Fixed-workload throughput mode: ticks run back to back with no wall-clock pacing. |

**Report.**
- **Timing per tick:** count, mean, P50, P95, P99, and maximum, for the whole fixed tick and for each part: scripts (phase 3), body preparation (4), the physics step (5), synchronization (6), events and post-physics hooks (7), and queries.
- **Counts per tick:** active and sleeping bodies, body pairs, contact constraints, events delivered, and query hits.
- **Memory:**
  - Jolt heap, live and peak, from the allocator hooks;
  - temporary-allocator high water;
  - script VM bytes;
  - process footprint at the start and end, with its per-tick slope.
- **Failures:**
  - Any step error flag, or a hit body, pair, or contact limit, is recorded as a failed run, not averaged in.
  - The run stores the hash of every body's pose after the last tick. The three runs, and the two worker-thread configurations, must match; a mismatch is a determinism failure.

**Sizes.**
- `p1_small`: a tenth of every count (500 dynamic bodies, 50 obstacles, 100 rays, and so on) and 120 sampled ticks. A CTest smoke run that must complete.
- `p1_physics`: the baseline, measured on the M4 Pro reference machine when cool (nominal thermal state at the start and end of each run, as for the [approved budgets](../acceptance.md#budgets)).
- `p1_20k`: four times the dynamic bodies. A stress input that reports the largest completed case; it gets no budget.

**As implemented** ([#1024](https://work.rezee.app/kash/issues/1024); manifests `p1_small`, `p1_physics`, and `p1_20k` in [benchmarks](../../benchmarks), workload `physics` in [performance](../performance.md#manifests)). Where the recipe leaves a detail open, version 1 fixes it:
- **Placement.** The field is cut into 13 m cells outside the bin, shuffled by the seed. The resting patches take the first cells, and the obstacles are spread over the rest, kept 2.2 m inside their cells, so the two never overlap. Patches are 10 × 10 bodies 1.15 m apart; capsules lie on their side. The dropped set fills 15 × 15 layers 1.2 m apart from 3 m up, with seeded jitter and yaw.
- **Groups.** Static (0) holds the ground, walls, obstacles, and the paddle; Dynamic (1) the bodies; Trigger (2) the sensors.
- **The paddle and the queries** are native systems before the scripts in phase 3. Rays use `raycast_nearest`, the closest-hit query; overlaps and casts report every body.
- **Limits.** The engine's defaults, raised only as the scene needs: `p1_20k` has 32,768 bodies, 164,448 body pairs, and 82,224 contact constraints.
- **Counts.** Jolt does not report its contact constraint count, so the report counts touching pairs and solid contacts instead, and says so under `unavailable`.
- **Parts of the tick.** Scripts are the script system's `fixed_update`; queries are the query system's. Events and post-physics hooks are the events phase and every system's `late_fixed_update`. The body commit is reported separately.
- **Determinism.** The state hash is `PlaySession::state_hash`: every entity's transform, every body's pose, velocities, and sleep state, and the event and message trace.

Budgets for P1 were approved on 1 October 2026 from #1024's baselines ([acceptance](../acceptance.md#physics-budgets)).

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
| Platforms, hardware, output quality | macOS/Metal implementation base; two proposed profiles above, both **unmeasured** (not available). #1005 measured an M4 Pro as its own profile ([acceptance](../acceptance.md#baselines)). | Product/platform choice and representative content on named machines. |
| Frame pacing and simulation rate | Proposed 60 fps / 60 Hz. Offscreen CPU frame P99 on a cool M4 Pro: 0.12–0.13 ms (sample), 2.04–2.06 ms (I1 10k), 21.3–21.7 ms (I1 100k, over the target; the full 100k protocol throttles). Present pacing is unmeasured. | P95/P99 frame pacing, input latency, solver stability, sustained thermal runs. |
| CPU/GPU subsystem limits and headroom | Unallocated. #1004/#1005 breakdowns exist (encoding dominates at scale). M4 Pro regression budgets were approved in [acceptance](../acceptance.md#budgets) on 29 September 2026. | #1004 timing breakdown, overlap/waits, and expected additional physics/render features. |
| Memory and residency ceiling | Unallocated. Tracked counts return exactly to the empty session; a ~200 MiB process-footprint plateau after the first load is unattributed. | I1/L1 live/peak attribution, available device memory and other process/system demands. |
| Load, activation, unload, edit-preview limits | Unallocated. A 10,000-entity scene loads in 34 ms (P95 35 ms) on a cool M4 Pro, and a P95 of 46 ms while throttled; edit-to-preview is unmeasured. | Timed L1 and authoring traces; acceptable user-visible stalls and pending-work limits. |
| World extent / positional accuracy | Unresolved; local float transforms only | Sweep origin offsets (for example 0, 100 m, 1 km, 10 km), measure camera/picking/physics error, then choose tolerances and coordinate strategy. The sweep is an experiment, not a supported range. |
| Physics, procedural, cinematic scale | Unresolved. The physics stress recipe [P1](#p1-physics-stress) is specified (#1015); its baselines come from #1024. Procedural and cinematic recipes are not specified. | P1 baselines on named hardware; representative authored slice with fixed generator and capture recipes. |

An approved budget must record workload version, hardware profile, quality/resolution, numerical limit, measurement method, rationale, and the review decision. Until then, results are observations. Regression thresholds and image tolerances are established from repeated baseline variance and review, not invented from a single run.
