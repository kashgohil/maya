# Physics

[Issue #1017](https://work.rezee.app/kash/issues/1017) adds rigid-body physics to play sessions: the `MayaPhysics` library on [Jolt Physics 5.6.0](architecture/physics-scripting-decision.md). It follows the [physics boundary](architecture/runtime-world-contracts.md#physics-boundary) and runs phases 4–6 of the [fixed tick](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick). Bodies are made from code for now: authored colliders and bodies follow in #1019, the script API and events in #1021.

## The library

`MayaPhysics` / `Maya::Physics` ([physics.hpp](../include/maya/physics/physics.hpp)) links MayaWorld, and Jolt privately. MayaSimulation links it, so the player and the editor's play mode share it.

- **Jolt stays inside.** Only `src/maya/physics/` includes Jolt headers, and the public header has no Jolt types. Bodies are named by EntityHandle; Jolt's body IDs never leave the library. The CTest check `maya_physics_headers` ([check_physics_headers.cmake](../cmake/check_physics_headers.cmake)) fails the build's tests if any other source, header, or test includes Jolt.
- **Jolt is part of every build.** The first configure fetches it with GLFW and Catch2; only the Jolt library itself is built. The options are in the [decision record](architecture/physics-scripting-decision.md#jolt-physics).
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

## Bodies

A `BodyDesc` makes a body for one entity:

| Field | Meaning |
| --- | --- |
| `motion` | Static, kinematic, or dynamic (below). |
| `colliders` | One or more box, sphere, or capsule shapes, each with an offset and rotation in the entity's local space. Several form a compound shape. A capsule's `half_height` is half its straight section, excluding the caps. |
| `mass`, `density` | Mass in kg for a dynamic body, or 0 to derive it from the density (kg/m³, default 1,000). |
| `friction`, `restitution` | Surface response; restitution is 0 to 1. |
| `linear_damping`, `angular_damping`, `gravity_factor` | Per-body motion settings. |
| `linear_velocity`, `angular_velocity` | Initial velocities, for dynamic bodies only. |
| `group`, `mask` | Collision group 0–15 and the groups it collides with. Two bodies collide only when each one's group is in the other's mask. |

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
6. **Commit.** The World batch commits as one transaction. Then bodies of destroyed entities are removed, and requested bodies are created and removed in request order. This is the next tick's phase 1.

Pause and single step need nothing extra: a paused session runs no ticks, and a step runs one tick with one physics step. Events and post-physics hooks (phase 7) arrive in #1021; pose interpolation for display arrives in #1016.

If creating a body fails at the commit (a refused entity, or a full world), the session stops with the reason. The World keeps that tick's batch, and bodies created before the failure are kept.

## Process-wide state, memory, and limits

Jolt's setup is global:
- **Once per process.** The allocator hooks, trace and assert hooks, factory, and type registry are installed before the first physics world and kept until exit.
- **The job pool.** One pool serves every world. `set_physics_worker_threads(n)` sets its size: −1 is the default, one less than the hardware threads and at most 7. The calling thread also runs jobs, and results do not depend on the count.
- **Memory.** `physics_memory()` reports the bytes Jolt has allocated through the hooks, with the peak and the number of allocations.

`PhysicsSettings`, passed to `PlaySession::start`, sets each world's limits:

| Setting | Default | When exceeded |
| --- | --- | --- |
| `gravity` | (0, −9.81, 0) m/s² | — |
| `max_bodies` | 16,384 | Creating a body fails: `the physics world is full (16384 bodies)`. |
| `max_body_pairs`, `max_contact_constraints` | 65,536 and 32,768 | Jolt drops the excess for that step and reports it; `stats()` counts the step and which cache was full. |
| `collision_steps` | 1 | — |
| `temp_allocator_bytes` | 4 MiB | Per-step scratch memory; a larger step falls back to malloc. |

An empty world reserves 20.6 MiB through the hooks for these limits, but it is created in about 0.02–0.09 ms and adds almost nothing to the process footprint until bodies use it. After a session, its Jolt memory is back to what it was before. Only the job pool and the type registry stay for the process.

`PhysicsStats` counts:
- bodies by motion type, and awake bodies;
- steps, and steps with errors by kind;
- bodies created and removed;
- the last tick's time in each phase.

The Diagnostics panel and debug views arrive with #1022.

## Determinism

Within one build on one machine, the same scene, systems, and inputs give identical poses on every tick, whatever the worker count:
- Bodies are created in request order (at session start, a system's request order).
- Requests apply in the order they were made.
- Poses are written back in creation order.

Jolt's callbacks, which arrive in any order, are not used yet. Cross-machine determinism is not promised ([scheduling](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick)).

## Cost

Release on the M4 Pro reference machine (thermal state nominal), with the default 7 workers. Boxes 1 m wide are dropped in 10 × 10 layers onto a static floor. The timings are over 300 ticks after the first, with all bodies awake:

| Bodies | Whole tick, mean | P95 | Jolt step | Synchronize | Preparation and body commit |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 0.47 ms | 0.58 ms | 0.34 ms | 0.03 ms | < 0.01 ms |
| 10,000 | 5.7 ms | 12.8 ms | 4.4 ms | 0.32 ms | 0.03 ms |

The rest of the tick is the World commit of the moved poses. The 10,000-body P95 comes from the pile's collisions. It is an observation for P1 ([#1024](https://work.rezee.app/kash/issues/1024)), not a budget.

## Tests

[physics_tests.cpp](../tests/physics_tests.cpp) (`maya_physics_tests`, CPU) covers:
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
