# Scheduling and rendering contracts

Status: intended implementation contracts for [#990](https://work.rezee.app/kash/issues/990). `Engine::tick` still calls one update followed by rendering. [#1003](../play.md) implements the fixed clock and its modes (play, pause, single step), input assignment to ticks, and phases 1–3 of the fixed tick in `MayaSimulation`, shared by the player and editor play. Animation, capture, and a job system are not implemented yet. [#1015](https://work.rezee.app/kash/issues/1015) records how [physics and scripts run in the fixed tick](#physics-and-behavior-in-the-fixed-tick), and how reset, replay, and reload behave. [#1017](../physics.md) implements phases 4–6 for bodies made from code. [#1018](../scripting.md) runs script `start`, `fixed_update`, `update`, and `stop` hooks as one system, and [#1020](../scripting.md#reload) implements the reload rules below. [#1021](../physics.md#the-fixed-tick) implements phase 7: events, event hooks, `late_fixed_update` for native systems and scripts, and the stop events. Phase 7 reads the World as the previous tick committed it (the batch commits after it), and its body requests apply at the next tick's phase 4. [#1016](../play.md#between-ticks) implements phase 2's pose history and presentation between ticks. History is kept only for entities whose transform the tick changed, taken as its batch commits. Teleports that took effect in the tick's step, reparenting, and new entities reset it for the entity and its descendants. [#1023](../play.md#recording-and-replay) implements tick-recorded replay: recordings hold what this section lists, replays feed each tick its recorded input, and a recording from another build or Jolt configuration, with changed assets, or with a reload is refused with its reason.

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

As built, animation (#1038, [animation](../animation.md#playing)) is the last phase-3 system, after the scripts. It reads the committed world like them, and its joint writes commit with the tick's batch, so a script's clip change is sampled in the next tick. Joints are never moving bodies (bodies are roots), so animation submits no kinematic targets yet.

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

## Physics and behavior in the fixed tick

Recorded for #1015 on [Jolt Physics and Luau](physics-scripting-decision.md). The [physics](runtime-world-contracts.md#physics-boundary) and [scripting](runtime-world-contracts.md#scripting-boundary) boundaries define what each side owns. #1017–#1023 implement this section. All of it is implemented as of #1023.

**Scripts are one system among others.** All script instances run as a single `SimulationSystem` at a configured place in the session's system list (by default after the built-in systems). Within it, instances run in activation order, which is the order bodies are created: document order at session start, then spawn order. Native systems keep their existing contract. (#1018 starts the instances that arrive in one tick in World storage order, which is document order at session start. An entity created during play may take a freed slot, so its order within its tick follows storage rather than creation, deterministically either way.)

| Phase | What happens |
| --- | --- |
| 1. Commit | Commands from the previous tick apply in stable order. Entities are destroyed first (their `stop` hooks run and their bodies are removed), then created entities activate (bodies are added, then `start` hooks run in activation order). Motion-type changes and reparenting apply here and reset pose history. Commands issued by `start` and `stop` wait for the next tick. |
| 2. Input and history | This tick's input is latched and the previous completed poses are kept for [interpolation](#transform-authority). |
| 3. Pre-physics | Native systems and then script `fixed_update(self, dt)` hooks read the state committed by the last tick. They write through commands: property and transform edits, and body requests (force, impulse, torque, velocity, kinematic target, teleport, wake). |
| 4. Body preparation | The adapter first moves static colliders whose committed transforms changed. Then it applies this tick's body requests in the order they were issued (system order, then call order): forces, impulses, velocities, kinematic targets, and teleports. A teleport resets that entity's pose history. |
| 5. Physics step | One `Update` of exactly one fixed interval, with the configured number of collision steps (default one). |
| 6. Synchronize | Kinematic and dynamic body poses are written to their entities' transforms in one World batch, in body creation order, and their descendants resolve. The step's error flags become diagnostics. |
| 7. Events and post-physics | The step's buffered contact and trigger records are resolved to entities and sorted by event kind, then by the two EntityIds. Recipients destroyed meanwhile are skipped and counted. Events are delivered to event hooks (`on_contact_begin`, `on_contact_end`, `on_trigger_enter`, `on_trigger_exit`) and native listeners. Then `late_fixed_update(self, dt)` hooks run. Their commands apply at the next tick's phase 1. |

After the frame's last tick, `update(self, frame_dt)` hooks run once per host frame. They may read and log, but may not issue simulation commands, because what happens between frames would then depend on the frame rate. This narrows the render-time rule above, which allowed commands for a future tick, until a frame-rate-independent input path for them exists. Presentation interpolation and extraction follow.

**Reset.** Stopping play releases the play World, the physics world, and the script VM. The next Play builds all three again from the authored document; no state is carried over.

**What "the same result" means.** A replay produces the same result on every tick when these match:
- the machine and the Maya build;
- Jolt's configuration;
- the scene document and its asset versions;
- the session seed;
- the per-tick input sequence.

The same result means identical component values for every entity, identical body states, and identical event sequences. The physics worker count may differ; the prototype showed identical results for any count. Live play with the same wall-clock frame times also repeats exactly, as it does today.

**What replay does not promise.** It makes no promise across machines, CPU architectures, OS or compiler versions, Jolt or Luau versions, or builds with different options. A cross-machine promise would need Jolt's cross-platform determinism and a Luau audit; that decision comes with networking or capture requirements.

**What determinism rests on.**
- Stable creation, command, and event orders.
- Work budgets counted in safepoints, not time.
- A seeded `math.random`, and no wall clock in scripts.
- Entities created during play take their IDs from a sequence seeded by the session, not from `EntityId::generate`.
- Replay records hold the tick-indexed input, the session seed, the scene document and asset versions, the build, and any reload markers ([#1023](https://work.rezee.app/kash/issues/1023)).

**Where scripts can still vary.** Luau's `pairs` order over keys that are tables, functions, or userdata depends on addresses. Scripts must not let that order affect the simulation. #1023's replay tests use content that follows the rule.

**Reload.**
- **When a script is edited during authoring,** it recompiles at the next host frame. A syntax error is reported and the last good version stays current, as for any asset reload. The exposed-property schema updates and scene values are revalidated. No hook runs, because authoring runs no scripts.
- **When a script changes during play,** it compiles off the tick:
  - **If compilation fails,** the old version keeps running and the error is reported.
  - **If it succeeds,** the instances of that script are replaced at the next tick boundary (phase 1). Each old instance's `stop` runs, then a new instance starts from the new version, in the same activation order.
  - **State.** Exposed properties keep their current play values; everything else in `self` starts over.
  - **While paused,** the swap waits for the next step or resume.
  - **Replay.** A reload is recorded, and replaying a session that reloaded is not promised to match.
- **Next Play.** Always uses each script's current good version.

#1020 implements these rules ([reload](../scripting.md#reload)). The editor checks script files every quarter second rather than every frame. Within a swap, all the old instances stop before any new one starts, both in activation order. Reloads are recorded with their tick in `ScriptReloads::applied`, and phase 1 runs retirements, then swaps, then new activations.

## Extraction and GPU ownership

Extraction runs after the world pose is consistent and takes read access for its duration. It produces immutable, per-view render data: camera matrices, view dimensions, instance transforms/normal matrices, mesh/material versions, lights, and validated identity mappings for picking. No pointer into movable component storage escapes into that data. Picking results carry a world/view version and resolve a handle again before selecting an entity.

A render snapshot holds leases on the exact asset/resource versions it references. A worker renderer can consume it without reading the live World. Editing/deleting an entity, unloading a cell, or reloading a material affects later snapshots; it cannot change an already encoded frame. Authoring and play views identify their worlds explicitly. Offscreen editor and capture targets use the same extraction/renderer as presentation views.

The RHI owns frame slots, per-draw allocations, submission completion, and deferred destruction. Every draw's constant data is independent. Do not reuse a frame allocation or destroy its buffers/textures until the last GPU submission using it completes; returning from `end_frame` is not completion. Retain CPU snapshots until encoding finishes and transfer/retain GPU-use leases through completion. On backpressure, use a bounded frame queue and wait/report the wait rather than grow memory indefinitely. #997 implements this with a default of three frames in flight and per-frame upload slots, and reports waits (see [the graphics device](../rhi.md#frame-pacing-and-upload-memory)). The final queue depth and renderer thread count still require #1004 measurements.

Cancellation prevents publication of obsolete snapshots but cannot cancel lifetime obligations of submitted GPU work. Resize/reload replaces target/resource versions and retires old ones after completion. Shutdown drains submissions after stopping producers. These contracts are implemented by #993 and #996–#998, not by this document. #998 [extracts snapshots](../renderer.md#render-snapshots) synchronously on the host thread; a worker renderer is not yet needed. One snapshot is shared by every view of a World, and each view carries its own camera matrices and dimensions. Material factors are copied into the snapshot instead of leasing material versions.
