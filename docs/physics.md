# Physics

[Issue #1017](https://work.rezee.app/kash/issues/1017) adds rigid-body physics to play sessions: the `MayaPhysics` library on [Jolt Physics 5.6.0](architecture/physics-scripting-decision.md). It follows the [physics boundary](architecture/runtime-world-contracts.md#physics-boundary) and runs phases 4–6 of the [fixed tick](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick). [Issue #1019](https://work.rezee.app/kash/issues/1019) adds the components that author bodies in the editor and in scene files. Systems can also make bodies from code. [Issue #1018](scripting.md) lets scripts move entities that have no body, or a static one. [Issue #1021](https://work.rezee.app/kash/issues/1021) adds [queries](#queries), [contact and trigger events](#contact-and-trigger-events) with phase 7, and the [script body API](scripting.md#bodies-queries-and-events).

## The library

`MayaPhysics` / `Maya::Physics` ([physics.hpp](../include/maya/physics/physics.hpp)) links MayaWorld, and Jolt privately. MayaSimulation links it, so the player and the editor's play mode share it.

- **Jolt stays inside.** Only `src/maya/physics/` includes Jolt headers, and the public header has no Jolt types. Bodies are named by EntityHandle; Jolt's body IDs never leave the library. The CTest check `maya_library_headers` ([check_library_headers.cmake](../cmake/check_library_headers.cmake)) fails the build's tests if any other source, header, or test includes Jolt; it checks Luau's boundary too ([scripting](scripting.md)).
- **Jolt is part of every build.** The first configure fetches it with GLFW and Catch2; only the Jolt library itself is built. The options are in the [decision record](architecture/physics-scripting-decision.md#jolt-physics).
- **Positions are double** (since [#1065](https://work.rezee.app/kash/issues/1065)). Jolt is built with `DOUBLE_PRECISION` (`JPH_DOUBLE_PRECISION`), as [#1060 decided](architecture/world-scale-decision.md#coordinates-double-positions): body positions, kinematic targets and teleports, query origins and hit points, contact points, and events' points cross as `math::DVec3`; velocities, forces, normals, and shapes stay float. A shape cast or overlap passes its own origin as Jolt's base offset, so its contact points stay precise. A body is 160 bytes instead of 128. A stack and a rolling sphere 100 km from the origin end within 0.1 mm of the same at it ([sweep](spatial.md#world-positions)).
- **One physics world per play session.** `PlaySession::start` creates a `PhysicsWorld` after it builds the World and before any system starts. The session destroys it before the World. Authoring Worlds never have one.

```cpp
class Dropper final : public maya::SimulationSystem {
public:
    std::string_view name() const override { return "Dropper"; }
    void fixed_update(maya::TickContext& tick) override {
        const auto crate = *tick.world.find(crate_id);
        if (tick.tick == 0) {
            auto body = maya::BodyDesc{};                       // a 1 m dynamic box by default
            body.colliders = {{maya::BoxShape{{0.5f, 0.5f, 0.5f}}}};
            tick.bodies.create(crate, body);                     // exists from the next tick
        } else if (tick.input.went_down(maya::KeyCode::Space)) {
            tick.bodies.add_impulse(crate, {0.0f, 5.0f, 0.0f}); // before this tick's step
        }
        if (const auto state = tick.physics.state(crate)) { /* state->linear_velocity, sleeping, ... */ }
    }
    maya::EntityId crate_id;
};
```

## Authored bodies

Three components describe physics in a scene. They are saved like any other component and edited in the [Inspector](inspector.md#physics-components).

| Component | Properties | Meaning |
| --- | --- | --- |
| `maya.collider` (ID 8) | `shape` (box, sphere, capsule), `half_extents`, `radius`, `half_height`, `offset`, `rotation`, `friction`, `restitution`, `sensor`, `group`, `mask` | A shape in the entity's local space. `half_height` is half the capsule's straight section. A sensor reports overlaps (#1021) and gets no contact response. |
| `maya.rigid_body` (ID 9) | `motion` (dynamic, kinematic), `mass` (0 derives it from `density`), `density`, `linear_damping`, `angular_damping`, `gravity_factor`, `linear_velocity`, `angular_velocity` | Makes the entity a moving body. Initial velocities are for dynamic bodies; a kinematic body stays where it is until something sets a target. |
| `maya.physics_settings` (ID 10) | `gravity` (m/s², default (0, −9.81, 0)) | The scene's physics settings. At most one per scene; without one, the defaults apply. |

When Play starts, `authored_physics` ([authored_physics.hpp](../include/maya/simulation/authored_physics.hpp)) turns these components into bodies in document order. `PhysicsWorld::create_bodies` creates them all or none, before any system starts:

- **Moving bodies.** An entity with a rigid body is a kinematic or dynamic body. Its shape is its own collider plus the colliders on entities below it that have no rigid body. Those colliders are placed relative to the body, and their entity's scale is baked into their shape. More than one collider forms a compound shape.
- **Static bodies.** An entity with a collider and no rigid body on it or an ancestor is a static body. Its world scale is baked into its shape.
- **One material and filter per body.** A body takes its friction and restitution from its first collider (its own, then the first below it in hierarchy order). All its colliders must share one collision group, mask, and sensor setting.
- **The rules of [Bodies](#bodies) apply.** A moving body is a root entity with unit scale. To scale a mesh, put it on an entity below the body, or size the collider instead.

If any body cannot be made, Play does not start. The reason names the entity by ID and name: `Physics: entity 6d617961 201 "Red cube": a rigid body needs a collider on its entity or on an entity below it`. The editor shows it in a notice and in Diagnostics; the player prints it and exits with code 1.

**Collision groups.** A collider's `group` (0–15) and `mask` (bit *n* set: collides with group *n*) decide what it collides with. Two colliders collide only when each one's group is in the other's mask. A project names its groups in its [project file](projects.md#collision-groups), and the editor edits the names in its Collision groups window. Group 0, `Default`, is where every collider starts, and a new collider's mask includes every group.

The sample project's [physics.scene](../samples/basic_scene/assets/physics.scene) is a floor, a stack of five crates, a crate that falls beside them, and a beacon turned by a [script](scripting.md): `maya_player samples/basic_scene physics.scene`.

## Bodies

A `BodyDesc` makes a body for one entity:

| Field | Meaning |
| --- | --- |
| `motion` | Static, kinematic, or dynamic (below). |
| `colliders` | One or more box, sphere, or capsule shapes, each with an offset, rotation, and scale in the entity's local space. The scale stretches the shape along its own axes; a sphere needs it uniform and a capsule needs X and Z to match. Several colliders form a compound shape. A capsule's `half_height` is half its straight section, excluding the caps. |
| `mass`, `density` | Mass in kg for a dynamic body, or 0 to derive it from the density (kg/m³, default 1,000). |
| `friction`, `restitution` | Surface response; restitution is 0 to 1. |
| `linear_damping`, `angular_damping`, `gravity_factor` | Per-body motion settings. |
| `linear_velocity`, `angular_velocity` | Initial velocities, for dynamic bodies only. |
| `group`, `mask` | Collision group 0–15 and the groups it collides with. Two bodies collide only when each one's group is in the other's mask. |
| `sensor` | Detects overlaps without a contact response. |

`validate_body` explains what is wrong with a description. Creating a body also checks the entity:

| Motion type | Who writes the pose | Rules when the body is created |
| --- | --- | --- |
| Static | World data. Writing the entity's transform (or a parent's) moves the body before the next step. A changed world scale rebuilds its shapes. | Its world scale is baked into its shapes. It may not sit below a kinematic or dynamic body. |
| Kinematic | Physics moves it to the target a system sets each tick. Without a target, it stops. | A root entity with unit scale. No entity below it may have a body. |
| Dynamic | Physics. Systems push it with forces, impulses, torques, and velocities, or teleport it. | As for kinematic. |

Scaled shapes follow the contract:
- a sphere needs uniform scale;
- a capsule needs the same scale on X and Z;
- a rotated collider cannot take nonuniform scale;
- a world transform with shear cannot be used.

The entity needs a transform. Bodies created or refused are named in errors by entity ID: `cannot create a dynamic body for entity 70 2: a dynamic body must be a root entity`.

## Requests during a tick

`TickContext` now also carries `bodies`, the tick's `BodyCommands`, and `physics`, a read-only `PhysicsWorld` (`has_body`, `motion_type`, `state`, `stats`). Requests are checked when they are made, and a broken rule throws, so the system fails with the reason. As for any failing system, the session stops ([play](play.md#play-sessions)).

| Request | Applies | Allowed on |
| --- | --- | --- |
| `create(entity, body)` | At the tick's commit; the body exists from the next tick. The entity may be one this tick's commands create. | An entity without a body |
| `remove(entity)` | At the tick's commit; the entity stays. | Any body |
| `add_force`, `add_torque` | This step only | Dynamic |
| `add_impulse`, `add_angular_impulse`, `set_linear_velocity`, `set_angular_velocity` | Before this step | Dynamic |
| `set_kinematic_target(entity, position, rotation)` | The body reaches it at the end of this step | Kinematic |
| `teleport(entity, position, rotation)` | Before this step; velocities are kept | Kinematic, dynamic |
| `wake(entity)` | Before this step | Kinematic, dynamic |

The session also refuses a system's World commands that would take a body's pose from physics:
- setting the transform of a kinematic or dynamic body;
- reparenting one;
- removing any body's transform;
- moving something with a body below a moving body.

The message names the system and the entity: `Tick 1: Lifter failed: cannot set the transform of entity 70 2: its dynamic body's pose is written by physics; use a force, a velocity, a kinematic target, or a teleport`.

Destroying an entity removes its body when the tick commits. A handle to a destroyed entity is refused before it reaches Jolt.

## The fixed tick

`PlaySession::run_tick` now runs:

1. **Input** (phase 2).
2. **Systems** (phase 3), in order. After each one, its World commands are checked against the bodies.
3. **Preparation** (phase 4). Static bodies whose committed world transforms changed move first. Then the tick's requests apply in the order they were made. Kinematic bodies without a target stop.
4. **Step** (phase 5). One `PhysicsSystem::Update` of the fixed interval, with `collision_steps` collision iterations (default 1).
5. **Synchronize** (phase 6). Each kinematic and dynamic body whose pose changed has its transform added to the tick's World batch, in body creation order. Bodies whose entities this batch destroys are skipped.
6. **Events and post-physics** (phase 7, #1021). The step's contact and trigger events are resolved and sorted ([events](#contact-and-trigger-events)). Then each system's `late_fixed_update` runs in order, with the events in `TickContext::events` and `physics` showing the completed step. Their World commands join the tick's batch and are checked like phase 3's. Their body requests wait: creations and removals apply at this commit, and everything else applies first in the next tick's phase 4, so the completed step cannot change.
7. **Commit.** The World batch commits as one transaction. Then bodies of destroyed entities are removed, and requested bodies are created and removed in request order, phase 3's and then phase 7's. This is the next tick's phase 1.

During phase 7 the World is still as the previous tick committed it, since the batch commits after it: a moving body's transform there lags its `state`, which is the completed step's.

Pause and single step need nothing extra: a paused session runs no ticks, and a step runs one tick with one physics step. Views show poses between ticks ([play](play.md#between-ticks), #1016); a teleport resets the body's pose history, so it jumps rather than sliding.

When a session stops, a last `late_fixed_update` runs with `TickContext::stopping` set and events that end every contact and trigger still in progress, marked `removed`. Nothing systems do then is kept.

If creating a body fails at the commit (a refused entity, or a full world), the session stops with the reason. The World keeps that tick's batch, and bodies created before the failure are kept.

## Queries

`PhysicsWorld` answers queries against the last completed step, on the owner thread between steps: in phase 3 the previous tick's, in phase 7 this tick's. Scripts and native systems see the same state, and the same query repeats exactly.

| Query | Returns |
| --- | --- |
| `raycast(origin, direction, distance, filter)` | Every body the ray hits within `distance`. A ray starting inside a shape hits it at distance 0. |
| `raycast_nearest(origin, direction, distance, filter)` | Exactly the first of `raycast`'s results, or none, without collecting the others (#1024). It keeps only the nearest hit as Jolt reports them, preferring the lower EntityId at an exact tie. In P1's dense scene it is about five times faster than taking the first of every hit. Scripts' `maya.raycast` uses it. |
| `shape_cast(shape, origin, rotation, direction, distance, filter)` | Every body a box, sphere, or capsule swept along the direction touches. One that touches at the start is at distance 0. |
| `overlap(shape, position, rotation, filter)` | Every body the shape touches where it is. |

- **One hit per body** (`QueryHit`): the entity's handle and EntityId, the nearest point, the hit surface's outward normal, and the distance.
- **Order.** By distance, then EntityId; overlaps (all at distance 0) are by EntityId. Jolt's broad-phase order never shows through.
- **Filter** (`QueryFilter`): `groups`, a bit set of the collision groups to hit (default all); `sensors`, whether to hit sensors (default not); `ignore`, an entity whose body is skipped.
- **Refusals.** A zero or non-finite direction, a negative distance, or a shape a collider could not have throws `std::invalid_argument` with the reason.

## Contact and trigger events

A Jolt contact listener runs on the worker threads during the step. It only appends plain records (the two bodies and sub-shapes, and for a new contact its point, normal, and approach speed) under a lock, and never throws; a record it cannot store is lost, never the step. In phase 7, `take_events` turns them into `PhysicsEvent`s on the owner thread:

- **Per pair of bodies.** A compound body's sub-shapes are counted together: a pair's contact begins when its first sub-shape pair touches and ends when its last one parts.
- **Kinds.** `contact_begin` and `contact_end`, or `trigger_enter` and `trigger_exit` when either collider is a sensor. A sensor never makes a contact.
- **Sleeping keeps contacts.** Jolt removes a body's contacts when it falls asleep; Maya keeps the pair and ends it only if the bodies are awake after a step and no longer touching. A stack that settles and sleeps does not end its contacts.
- **Contents.** The tick, both EntityIds (`first` the lower), for a begin or enter the point, the normal from `first` toward `second`, and the approach speed along it. Jolt knows no impulse when a contact is added, so the speed stands in for it. Events hold identities, never component pointers.
- **Order.** Sorted by kind, then `first`, then `second`: the same events in the same order for any worker count.
- **Removed bodies.** A body removed with its entity, by `BodyCommands::remove`, or when the session stops ends its contacts and triggers with `removed` set. The first two are delivered in the next tick's phase 7, the last in the session's final `late_fixed_update`.
- **Recipients.** `first_entity` and `second_entity` are the entities' handles, or empty when an entity is gone or being destroyed in the tick's batch. Each empty one counts in `PhysicsStats::event_recipients_skipped`; `events` counts all events.

## Process-wide state, memory, and limits

Jolt's setup is global:
- **Once per process.** The allocator hooks, trace and assert hooks, factory, and type registry are installed before the first physics world and kept until exit.
- **The job pool.** Since #1061, Jolt runs on the process's [job system](jobs.md#physics), on its frame tier, rather than a pool of its own. `set_physics_worker_threads(n)` limits how many frame workers a step uses at once: −1 is the default (the frame tier's workers, at most 7), 0 runs every job on the stepping thread, and larger counts are capped at the frame tier. The calling thread also runs jobs, and results do not depend on the count.
- **Memory.** `physics_memory()` reports the bytes Jolt has allocated through the hooks, with the peak and the number of allocations.

`PhysicsSettings`, passed to `PlaySession::start`, sets each world's limits:

| Setting | Default | When exceeded |
| --- | --- | --- |
| `gravity` | (0, −9.81, 0) m/s² | — |
| `max_bodies` | 16,384 | Creating a body fails: `the physics world is full (16384 bodies)`. |
| `max_body_pairs`, `max_contact_constraints` | 65,536 and 32,768 | Jolt drops the excess for that step and reports it; `stats()` counts the step and which cache was full. |
| `collision_steps` | 1 | — |
| `temp_allocator_bytes` | 4 MiB | Per-step scratch memory; a larger step falls back to malloc. |

An empty world reserves 20.6 MiB through the hooks for these limits, but it is created in about 0.02–0.09 ms and adds almost nothing to the process footprint until bodies use it. After a session, its Jolt memory is back to what it was before. Only the job system and the type registry stay for the process.

`PhysicsStats` counts:
- bodies by motion type, and awake bodies;
- steps, and steps with errors by kind;
- bodies created and removed;
- body pairs touching now (`contacts`, and `overlaps` with a sensor), and queries asked (#1022);
- the per-step scratch allocator's high water and its preallocated capacity (#1024); beyond the capacity, Jolt falls back to malloc;
- the last tick's time in each phase.

The editor's Diagnostics panel shows them while playing ([editor](editor.md#physics-debug-views)).

## Debug views

[Issue #1022](https://work.rezee.app/kash/issues/1022) draws physics over a view, through the renderer's [debug lines](renderer.md#debug-lines). `PhysicsDebugOptions` ([physics_debug.hpp](../include/maya/simulation/physics_debug.hpp)) chooses categories and collision groups:

| Category | Draws | Color |
| --- | --- | --- |
| Colliders | Every solid collider's outline. | Cyan |
| Body state | The same outlines, colored by body: static; kinematic (dimmer while asleep); dynamic, active or sleeping. | Grey-blue; violet; green or slate |
| Triggers | Every sensor's outline. | Amber |
| Contacts | Each contact point of the last step, as a cross, and its normal, as an arrow. | Red and orange |
| Queries | The last tick's raycasts (their line), shape casts (the shape at both ends and the line between), and overlaps (the shape), and a cross at each hit. | Blue, hits pink |

Body state takes precedence over Colliders when both are on. A body (or contact) is drawn only when its collision group, or one of the two groups, is in `groups`.

- **Play views** (`play_physics_debug`) draw from the play session's physics world: every body's colliders as they were described, at the entity's shown pose, so outlines stay on meshes between ticks ([play](play.md#between-ticks)). `PhysicsWorld::for_each_debug_body` gives each body's entity, motion type, sleep, sensor flag, group, and colliders.
- **Authoring views** (`authored_physics_debug`) have no physics world. They draw each collider component at its entity's world pose, with body state from the authored motion: the nearest rigid body at or above the entity, else static. Contacts and queries have nothing to show.
- **Capture.** Contacts and queries are kept only while `PhysicsWorld::set_debug_capture(true)`, which views turn on while they show them (`PlaySession::set_physics_debug_capture`). Then each step keeps every contact point of its solid contacts, from Jolt's added and persisted callbacks, sorted by position (`debug_contacts`), and each query is kept with its hits (`debug_queries`) until the session starts the next tick. Sleeping bodies report no contact points. Off, nothing is kept, and the only cost is a flag read in each callback. Capture never changes the simulation: the same session's state hashes match with and without it.
- **Not through Jolt's `DebugRenderer`.** The [decision record](architecture/physics-scripting-decision.md) expected debug drawing to go through Jolt's `DebugRenderer`. Jolt compiles it only into Debug and Release builds (not a build without a type), and it draws shapes as triangle meshes, hundreds of edges for a sphere. Maya's collider shapes are boxes, spheres, and capsules, so outlines are made from their descriptions instead: the same drawing in every build and in authoring and play views, and cheap at 10,000 colliders.
- **The player** draws every category with `--debug-physics` ([play](play.md#the-player)), and never otherwise.

Making the lines costs, in Release with every body resting on a floor (`maya_simulation_tests "Physics debug cost*"`, hidden):

| Colliders | Outlines | Every category |
| --- | --- | --- |
| 1,000 | 0.05 ms | 0.12 ms (16,016 lines) |
| 10,000 | 0.57 ms | 1.25 ms (160,016 lines) |

Drawing them is in [renderer](renderer.md#debug-lines).

## Determinism

Within one build on one machine, the same scene, systems, and inputs give identical poses on every tick, whatever the worker count:
- Bodies are created in request order (at session start, a system's request order).
- Requests apply in the order they were made.
- Poses are written back in creation order.

Jolt's callbacks, which arrive in any order, are not used yet. Cross-machine determinism is not promised ([scheduling](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick)).

## Cost

Release on the M4 Pro reference machine (thermal state nominal), with the default 7 workers. Boxes 1 m wide are dropped in 10 × 10 layers onto a static floor. The timings are over 300 ticks after the first, with all bodies awake:

| Bodies | Whole tick, mean | P95 | Jolt step | Synchronize | Events (#1021) | Preparation and body commit |
| --- | --- | --- | --- | --- | --- | --- |
| 1,000 | 0.48 ms | 0.60 ms | 0.35 ms | 0.03 ms | 0.006 ms (6 events per tick) | < 0.01 ms |
| 10,000 | 6.2 ms | 14.1 ms | 4.6 ms | 0.35 ms | 0.21 ms (490 events per tick) | 0.03 ms |

The rest of the tick is the World commit of the moved poses. The events phase grows with how many contacts begin and end, about 0.4 µs per event including Jolt's records, not with the number of bodies. #1021's first version walked every contact pair each tick (0.27 ms at 10,000 bodies); it now visits only pairs that stopped touching. The 10,000-body P95 comes from the pile's collisions. It is an observation for P1 ([#1024](https://work.rezee.app/kash/issues/1024)), not a budget.

## Tests

[physics_debug_tests.cpp](../tests/physics_debug_tests.cpp) (`maya_simulation_tests`) covers the debug views (#1022): authoring outlines by category, motion, sensor, and group, with offsets and child colliders; a capsule on a scaled entity keeping round caps; play outlines from the physics world at shown poses, with sleeping and kinematic colors; capture only when asked, sorted contacts, queries with their hits, a new query list each tick, and identical state hashes over 300 ticks with and without capture. [physics_debug_gpu_tests.cpp](../tests/physics_debug_gpu_tests.cpp) renders each category on Metal and compares it with its [reference image](../tests/references/physics-debug), as the #1005 references are compared.

[physics_tests.cpp](../tests/physics_tests.cpp) (`maya_physics_tests`, CPU) covers queries and events (#1021), and that `raycast_nearest` equals the first of `raycast` in every field for filtered rays, an exact tie, and 400 rays into a settling pile (#1024):
- **Queries:** raycasts, shape casts, and overlaps with group, sensor, and ignore filters; distances, points, and normals; one hit per body, nearest first; the same results on repeated calls; refused queries.
- **Events:**
  - one begin or enter per pair, contacts held while bodies sleep, and a sphere passing through a sensor entering and exiting once;
  - the same events at the same ticks with 0 and 4 workers;
  - order by kind and EntityId in a tick where two boxes land together, made in the opposite order;
  - a recipient being destroyed in the tick its contact begins left out and counted;
  - removals: a destroyed entity's and a removed body's contacts ending as `removed`, the tick after;
  - session stop ending every contact and trigger in progress.
- **Phase 7:** systems read the completed step; a velocity set there does not change it, and the next tick's phase 3 still sees it, but it applies before the next step; transform writes to bodies are refused there too.

It also covers authored bodies (#1019):
- an authored stack of six boxes that settles and stays at rest;
- settings from the components (mass, damping, gravity factor, initial velocity, kinematic motion, scene gravity);
- compound bodies from colliders on scaled child entities;
- groups, masks, and sensors;
- every refusal at Play start, with nothing left behind;
- identical results with 0 and 4 workers.

It also covers bodies made from code (#1017):
- **Description checks:** body description validation.
- **Motion:**
  - free fall against the fixed-step integration (position and velocity), with the World showing the body's exact pose;
  - a box that comes to rest on a static floor and sleeps;
  - kinematic targets, stopping without one, and pushing a dynamic box;
  - teleports that keep velocity;
  - forces that last one step, impulses, velocities, and mass overrides.
- **Refusals** with their messages:
  - transform writes, reparenting, and transform removal on bodies;
  - requests on the wrong motion type or on an entity without a body;
  - non-finite values;
  - duplicate bodies;
  - scale and hierarchy rules.
- **Static colliders** that bake a parent's scale, follow its transform, and rebuild on a new scale.
- **Compound bodies.**
- **Lifetime:**
  - bodies for entities created in the same tick;
  - removal when an entity is destroyed;
  - stale handles refused;
  - explicit removal.
- **Scheduling:** pause and single step advance one interval.
- **Determinism:** identical pose traces of 300 bodies over 240 ticks with 0, 1, and 4 workers, and on repeated runs.
- **Limits:** a full world fails the next body and keeps the earlier ones; invalid settings are refused.
- **Memory:** 100 play sessions return Jolt's memory exactly to the baseline.
- **Cost:** `maya_physics_tests "[cost]"` prints the cost table above; it is hidden from normal runs.
