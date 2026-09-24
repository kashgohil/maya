# Scheduling and rendering contracts

Status: intended implementation contracts for [#990](https://work.rezee.app/kash/issues/990). The current `Engine::tick` still calls one update followed by rendering; none of the future scheduler, scripting, physics, or job APIs below is implemented here.

## Clocks and modes

Keep separate monotonic wall time, integer simulation tick/time, and presentation time. Accumulate time in double precision; derive simulation time from the integer tick and configured fixed interval. A frame can contain zero, one, or several simulation ticks. The same recorded tick inputs must be consumable by player and editor play sessions.

Proposed initial interactive settings are 60 simulation ticks/second, at most four catch-up ticks per host frame, and at most 0.25 seconds of admitted wall delta. These are configurable engineering defaults to validate, not shipping performance guarantees. Reject nonfinite/negative deltas. Record wall time rejected by the clamp and whole ticks discarded after the catch-up cap; keep the fractional remainder. Simulation advances only for ticks actually executed. This bounds interactive work while exposing slowdown; it does not claim real-time simulation under overload.

| Mode | Time and input behavior |
| --- | --- |
| Authoring | No gameplay simulation. Apply undoable edit transactions at a host-frame boundary, update derived transforms, then extract views. The editor camera is tool state. |
| Play | Clone the authored world; accumulate admitted wall time, consume fixed ticks, interpolate for presentation. Input samples are assigned to ticks. |
| Pause / single step | Paused wall time does not accumulate. Step advances exactly one fixed tick. Display the current pose when paused/stepping; rebase wall time and accumulator on resume so there is no catch-up burst. |
| Capture / replay | Drive a requested output timestamp or recorded tick sequence independently of elapsed wall time. Do not clamp/drop simulation ticks; wait for declared content readiness. Rational output frame rate and integer frame index avoid cumulative frame-time drift. |

Input routing occurs before simulation. A pressed/released edge is consumed by one assigned tick, held state persists, and no-tick frames retain pending edges. UI-consumed events do not leak to gameplay; the editor's [input router](../editor.md#input-routing) (#999) gives each window event exactly one owner. Losing focus clears held gameplay actions. Benchmark/replay input bypasses live device state and logs the assigned tick. Fixed stepping does not promise bit-identical physics across hardware, thread schedules, or library versions; exact capture may require recorded state/caches.

Initially assign queued live edges to the next executed tick, retaining their order even if press and release both arrive before that tick. Catch-up ticks reuse held state but do not repeat edges. Discarding excess wall time must not silently discard input events. Tick-recorded replay uses its recorded assignments instead of reassigning from wall timestamps.

## Frame and fixed-tick order

At each host frame, poll events and update tool input. Publish bounded asset-service completions after checking their request tokens. Queue play-world commands; an active play world's structural changes become visible only at the next tick boundary. Authoring worlds instead commit at their host-frame boundary. Paused play worlds queue simulation changes until a step/resume; asset readiness and viewport redraw can continue.

For each fixed tick, execute the following order. The first implementation may be serial; parallel execution must preserve these dependencies and declare component access.

| Phase | Reads / writes and visibility |
| --- | --- |
| 1. Commit pending world commands | Apply commands from earlier ticks, tools, and validated load results in stable order. Invalidate destroyed entities, perform teardown, activate fully ready entities, and invoke their start hooks before their first update. New commands from hooks wait until a later tick. |
| 2. Establish tick input and pose history | Latch this tick's input; retain previous completed simulation poses for interpolation. Newly activated entities initialize previous = current. |
| 3. Pre-physics scripts/gameplay | Fixed-update hooks read the committed world and previous completed physics state. Write nonphysical local transforms through declared access, submit forces/impulses, kinematic targets, or explicit teleports. Structural edits remain queued. |
| 4. Animation and body preparation | Evaluate fixed-time animation/root motion for simulation-controlled objects, resolve nonphysical transform hierarchy, and submit static changes/kinematic targets to the physics adapter. A body has one selected motion authority. |
| 5. Physics step | Advance exactly one fixed interval. Only the physics adapter writes dynamic body poses/velocities during the solve; internal solver substeps are an adapter detail. |
| 6. Synchronize and resolve | Copy dynamic poses back into world simulation state, resolve dependent child transforms, and make this completed tick's pose/query state available. |
| 7. Events and post-physics hooks | Deliver buffered contact/trigger events on the world owner thread using validated handles. Post-physics hooks may read results and queue changes for the next tick; they cannot retroactively change the completed step. |

Start hooks run once per activation; stop hooks run once if start was entered, including partial failure, following #989 rollback semantics. An activation failure rolls back that transaction and reports it without corrupting the previously active world. Physics worker callbacks only append event data; they never execute scripts or structurally mutate the world. Events carry tick and entity identities, not retained component pointers. Recipients destroyed before delivery are skipped with defined diagnostics/counters. Unload-generated exit/teardown events must be distinguished from contacts produced by a solve.

Apply a count/byte/time budget to background finalization and activation admission before starting a transaction; already-started atomic publication cannot be interrupted halfway. Oversized units must be split during preparation or rejected with diagnostics. Log the actual activation tick and asset version. Replay/capture must reproduce those activation decisions or preload and wait for a declared ready set; stable command ordering alone cannot make asynchronous completion timing deterministic.

After the final tick (or with no tick), prepare render-only animation/tool updates, interpolate presentation data, extract views, and submit rendering. Render-time hooks have no write access to authoritative simulation poses; gameplay effects are commands for a future tick. A missing drawable or hidden/zero-sized viewport suppresses that view, not the simulation schedule. Whether an unfocused application pauses is explicit host policy.

## Transform authority

| Object state | Pose writer and allowed requests |
| --- | --- |
| Authoring world | Validated editor commands own local TRS. Undo/redo and serialization operate on authored properties. |
| Nonphysical play entity | Its declared gameplay/animation controller owns local TRS in the pre-physics phases. Systems cannot both write it without explicit ordering/authority. |
| Static collider | World data supplies its pose at a tick boundary; changes notify the physics adapter before the next solve. |
| Kinematic body | Gameplay/animation supplies a target before physics; the adapter owns conversion to solver motion and publishes the resulting simulation pose. |
| Dynamic body | Physics owns pose/velocity. Scripts use forces/impulses or an explicit teleport/reset command; animation consumes the result or drives a separate visual child. |
| Presentation | Extraction owns interpolated poses. These never update authored or physics state. |

For running play, interpolate previous/current completed poses with `alpha = accumulator / fixed_interval`, bounded to [0,1). This intentionally presents one simulation interval behind the accumulated clock. Interpolate translation/scale linearly and normalized rotations along the shortest quaternion arc; preserve hierarchy relationships when composing visual poses. Never linearly blend arbitrary affine matrices as the default hierarchy solution.

Teleports, origin changes, reparenting, spawning, and body-mode transitions reset the affected pose history (including dependent descendants) so presentation cannot interpolate across a discontinuity. Pause and single-step display alpha = 1 explicitly. Capture selects its sample times and any interpolation/cache policy explicitly; frame index is not a substitute for simulation tick index.

## Extraction and GPU ownership

Extraction runs after the world pose is consistent and takes read access for its duration. It produces immutable, per-view render data: camera matrices, view dimensions, instance transforms/normal matrices, mesh/material versions, lights, and validated identity mappings for picking. No pointer into movable component storage escapes into that data. Picking results carry a world/view version and resolve a handle again before selecting an entity.

A render snapshot holds leases on the exact asset/resource versions it references. A worker renderer can consume it without reading the live World. Editing/deleting an entity, unloading a cell, or reloading a material affects later snapshots; it cannot change an already encoded frame. Authoring and play views identify their worlds explicitly. Offscreen editor and capture targets use the same extraction/renderer as presentation views.

The RHI owns frame slots, per-draw allocations, submission completion, and deferred destruction. Every draw's constant data is independent. Do not reuse a frame allocation or destroy its buffers/textures until the last GPU submission using it completes; returning from `end_frame` is not completion. Retain CPU snapshots until encoding finishes and transfer/retain GPU-use leases through completion. On backpressure, use a bounded frame queue and wait/report the wait rather than grow memory indefinitely. #997 implements this with a default of three frames in flight and per-frame upload slots, and reports waits (see [the graphics device](../rhi.md#frame-pacing-and-upload-memory)). The final queue depth and renderer thread count still require #1004 measurements.

Cancellation prevents publication of obsolete snapshots but cannot cancel lifetime obligations of submitted GPU work. Resize/reload replaces target/resource versions and retires old ones after completion. Shutdown drains submissions after stopping producers. These contracts are implemented by #993 and #996–#998, not by this document. #998 [extracts snapshots](../renderer.md#render-snapshots) synchronously on the host thread; a worker renderer is not yet needed. One snapshot is shared by every view of a World, and each view carries its own camera matrices and dimensions. Material factors are copied into the snapshot instead of leasing material versions.
