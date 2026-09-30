# Runtime and world contracts

Status: intended implementation contracts for [#990](https://work.rezee.app/kash/issues/990). [#1015](https://work.rezee.app/kash/issues/1015) adds the [physics](#physics-boundary) and [scripting](#scripting-boundary) boundaries on [Jolt Physics and Luau](physics-scripting-decision.md). See [the index](README.md) for current-code gaps and unresolved product decisions.

## Dependencies and ownership

The dependencies below are permitted public-interface dependencies, not required new libraries. Foundation includes math, typed identifiers, diagnostics, and service interfaces; it has no world, renderer, desktop, or editor dependency.

| Module | May depend on | Owns / must not depend on |
| --- | --- | --- |
| World | Foundation, asset-reference value types | Entity/component storage, persistent-ID lookup, hierarchy, authored data. No renderer, GPU backend, window, editor, physics-library, or scripting-VM types. |
| Assets | Foundation, abstract I/O/jobs; upload through an adapter | Registry, versions, dependency leases, loading state. No World/editor ownership; import UI stays in tools. Asset-reference types do not pull in loaders or RHI. |
| Simulation | World, Foundation, asset services, Maya physics/script interfaces | Per-world scheduler and physics/script instances. External library types remain behind adapters. No renderer/editor dependency. |
| Renderer and extraction | Read-only World queries, Assets, RHI, Foundation | View data, render snapshots, visibility, passes, transient allocations. Never owns or mutates authoritative World state. |
| RHI / Metal | Foundation; backend platform APIs | GPU resource storage, submission, completion, presentation. No World, Simulation, sample, or editor dependencies. |
| Runtime application/session | World, Assets, Simulation, Renderer, RHI | Composition and shutdown ordering. Services are injected; World does not call back into an owning Engine singleton. |
| Desktop host | Runtime entry points, GLFW/platform | Native window, OS events, input routing, wall clock. It outlives device shutdown. |
| Editor, player, capture tools, game projects | Runtime services; desktop host when needed | Their own tools/content/configuration. Runtime never depends on these callers. Editor undo/selection/inspectors are editor-only. |

Keep the #989 lifetime order: host window → device → application content during startup, and the reverse during shutdown. The application/session owns its worlds and their simulation contexts; an asset service may be shared between authoring and play worlds within that session. Readiness is explicit: partially initialized objects must be destructible, failures roll back acquired ownership, and cleanup does not throw.

Future shutdown first rejects new work, invalidates async request tokens, cancels and joins producers, and stops simulation. Script teardown runs while its required world/services still exist. Destroy world instances and release content/snapshot leases; the RHI retains pending GPU allocations until completion. Destroy services and the device before the window. Stopping a play world uses the same rules without destroying the shared session or authored world. Cancellation alone is not proof that a worker has stopped accessing memory.

## Identity and references

| Identity | Contract |
| --- | --- |
| EntityId | Typed opaque 128-bit persistent ID; zero is invalid. Unique within an authored world/document. Save/load preserves it; names, storage positions, and addresses are not identity. |
| AssetId | Separate typed opaque 128-bit persistent ID, unique within the project registry. Rename/move/reimport preserves it. A path is a locator, and a content hash/version identifies a revision, not the asset. |
| World instance token | Identifies a single live World lifetime. A recreated World gets a new token even when loading the same scene. Never serialize it. |
| EntityHandle | World token + slot + generation, checked on every resolution. Wrong world, dead slot, generation mismatch, or invalid sentinel returns an explicit failure. Slot reuse increments generation; retire a slot/token rather than wrap into an old valid identity. |
| Asset/resource runtime handle | Typed identity for a service/device lifetime and slot generation. Validate type, owner, and liveness. It is not an EntityHandle or serialized AssetId. |

Exact handle bit layout and storage strategy are #991/#993/#996 implementation choices; they must preserve these observable rules. Queries return bounded borrows or views, not owning raw pointers. A component borrow expires at the end of its declared access phase and always before a structural commit. No cached component pointer survives entity creation/destruction, component add/remove, storage relocation, or a job boundary.

An entity reference within a world stores EntityId and resolves through that world. A reference to another scene document also identifies the scene asset; it never searches all worlds for a coincidentally matching ID. A reference with multiple possible scene instances requires an explicit instance binding or fails as ambiguous. It must not pick the first match.

Loading a saved document preserves its IDs and rejects duplicates before publication. Duplicating entities or instantiating another copy of scene content in the same world allocates new IDs and remaps internal references as one transaction; external references are retained or explicitly rebound. A play clone preserves persistent IDs in a *different* World instance and reconstructs every runtime handle. Thus authoring and play can share serialized identity without sharing mutable state.

At the destruction commit boundary, invalidate handles and remove persistent-ID resolution before storage is recycled. Internal teardown may inspect retained component data; external resolution cannot revive it. Destroying an entity destroys its components and releases their leases, not the shared asset itself. Undo may restore its persistent ID with a fresh runtime generation; an old runtime handle stays invalid. In the initial hierarchy, deleting a parent deletes its subtree atomically unless an explicit reparent operation is included first. References from outside the subtree become unresolved.

## Assets, mutation, and publication

A serialized `AssetRef<T>` is a typed AssetId. It does not by itself force residency. Asset services issue explicit leases that keep a loaded version and its dependencies resident; render submissions hold the required resource leases. Multiple instances share the same asset version. Missing/loading/failed assets retain their IDs and diagnostics; rendering can use a declared placeholder. A required simulation asset must be ready before its dependent entity activates.

Loading and reload prepare a new version privately. At a controlled publication boundary, validate it and swap the current version atomically; a failed reload leaves the last good version usable and reports the error. Existing users may finish with old leases. Releasing the last lease makes a version eligible for eviction; GPU use and an explicit bounded cache may defer physical release. Entity deletion and GPU resource deletion are different events.

The world has one mutation owner initially. Systems declare read/write component access; parallel jobs may later execute only when those accesses and dependencies permit it. Workers return immutable results tagged with world/request/version tokens. They cannot create live entities, change component containers, call editor widgets, or publish GPU resources behind the owner's back.

Structural operations (create/destroy, component add/remove, parenting, scene/cell activation) are queued and applied at the [scheduled commit boundary](scheduling-contracts.md). Validate handles, property ranges, hierarchy cycles, and required dependencies before publishing a transaction. Failure leaves the previous world intact. Merge commands in a documented stable order (phase, system order, producer sequence), not worker completion order; conflicts return diagnostics instead of becoming races. Commands emitted by lifecycle callbacks enter a later transaction, never recursively mutate the commit in progress.

Prepare fallible additions before applying destructive removals in a mixed transaction. Activation hooks operate on a private staging context and buffer any world commands; they cannot directly alter pre-existing entities. If preparation fails, stop partially started additions, release their resources, and discard their buffered commands before publication. Successful preparation publishes the batch as one boundary operation. This makes world rollback concrete without claiming that arbitrary external script side effects can be undone.

## Coordinates and transforms

| Quantity | Convention |
| --- | --- |
| Units | Metres, seconds, kilograms; velocity m/s, acceleration m/s², force newtons, torque N·m. Gravity is world configuration, not a hardcoded gameplay assumption. |
| Axes | Right-handed, +X right, +Y up, camera/object forward -Z. Importers/adapters perform external convention conversion explicitly. |
| Rotation | Radians in runtime/property data, normalized quaternions stored x/y/z/w; identity (0,0,0,1). UI may display degrees with explicit conversion. Legacy Camera FOV/yaw/pitch degree inputs are an adapter exception. |
| Matrices | Column-major memory; column vectors. Local matrix = T × R × S, world matrix = parent world × local, clip position = projection × view × world × position. Positive rotation uses the right-hand rule. |
| Camera | Vertical FOV in radians; validate 0 < FOV < π, aspect > 0, 0 < near < far, finite inputs. View is inverse of a rigid camera pose; camera ancestry must not scale/shear the view. |
| Depth and rasterization | Initial perspective maps view-space -near to 0 and -far to 1, depth clear 1, compare less. Front faces are counterclockwise after backend convention conversion; back faces culled. Reversed depth would be a coordinated future change. |
| View dimensions | Framebuffer pixels, not logical window points. Zero-sized views skip allocation/rendering. Editor input converts logical coordinates and viewport origin/scale before picking. |

Local TRS is authoritative authored data; derived world matrices are cached, never a second editable transform. Initially local positions and rendering matrices remain floats near the origin. This does **not** approve a maximum world extent: world-position conversions must have named boundaries so a later precision strategy does not require changing entity identity or every component API.

Allow finite positive nonuniform scale for visual entities. Parent composition can produce shear: keep the full affine world matrix rather than decomposing it each frame. Transform normals with the inverse transpose of the world linear transform. Initially reject zero/negative scale, singular transforms, and nonfinite values at authoring/load boundaries; mirror authoring and its winding/tangent rules need an explicit extension. Normalize finite nonzero quaternions; reject a zero quaternion. Floating-point tolerances are explicit and tested in [#992 spatial operations](../spatial.md), not hidden in arbitrary clamps. That record also defines failure handling when finite local transforms compose into a world pose outside float matrix/inverse representability.

Reparenting rejects cycles and cross-world parents. Offer explicit keep-local or keep-world semantics. A keep-world operation that cannot represent the new local transform as supported TRS (for example, requiring local shear) fails without changing the hierarchy. Camera and physics adapters enforce their additional restrictions; they never silently discard scale/shear. Physics body scale is initially baked into shapes before activation; dynamic/kinematic bodies must be root entities with unit transform scale until an explicit parent/body synchronization design is validated.

## Physics boundary

Recorded for #1015. `MayaPhysics` ([#1017](../physics.md)) implements the interface, lifetime, motion types, shapes, filtering, and threading below; [#1019](../physics.md#authored-bodies) adds the components, and [#1021](https://work.rezee.app/kash/issues/1021) the queries and events. Gravity, which the contract calls a scene setting, is the scene's `maya.physics_settings` component. Jolt is chosen in the [decision record](physics-scripting-decision.md).

**The interface.** `MayaPhysics` is a static library with public headers in `include/maya/physics/`, and those headers contain no Jolt types. Jolt headers are included only by `src/maya/physics/`, and a CTest check enforces this, like the `*_no_editor_ui` checks. The library links MayaWorld and Jolt. MayaSimulation links MayaPhysics, and MayaRuntime receives it through MayaSimulation. Bodies are named across the interface by EntityHandle and EntityId. Jolt `BodyID`s, body pointers, and shapes never leave the library; a body's Jolt user data is an index into the adapter's own table.

**Process and world lifetime.**
- **Process setup.** Jolt's allocator, trace, and assert hooks, its factory, and its type registry are installed once per process before the first physics world, and kept until exit.
- **One physics world per play session.** It is created after the play World is built and before any start hook runs. It is destroyed before the play World is released. Authoring Worlds have no physics world: the editor's picking stays with the renderer, and #1022's debug views draw from the play session.
- **Body creation order.** Bodies are created in document order: roots in order, each followed by its descendants. Entities activated during play are added in activation order.
- **Resources.** A world owns its temporary allocator and its body, body-pair, and contact limits, all configurable per project; the prototype used 16 MiB of scratch. One job pool serves all physics worlds in a process. Its worker count is configurable and does not change results. Exceeding a limit fails that body's creation, or reports the step's error flags as a diagnostic with a counter. It never corrupts the world.

**Bodies and motion types.** A body is the `maya.rigid_body` component (#1019). An entity holds at most one component of each type, so a body's shape is the `maya.collider` on its own entity together with the colliders on its descendants that have no body of their own; more than one collider forms a compound shape. A collider with no body on its entity or any ancestor is static. Motion type is authored and has one pose writer, as in the [transform-authority table](scheduling-contracts.md#transform-authority):

| Motion type | Pose writer | Scripts and systems may |
| --- | --- | --- |
| Static (a collider without a body) | World data at a tick boundary | Move it with ordinary transform writes, which the adapter applies before the next step. Moving static colliders every tick is supported but costly. |
| Kinematic | Gameplay supplies a target pose each tick; the adapter moves the body to it | Set the kinematic target, or teleport. Direct transform writes are refused. |
| Dynamic | Physics | Add forces, impulses, and torques, set velocities, or teleport. Direct transform writes are refused. |

- **Changing motion type.** A command applied at the next tick boundary. It resets the entity's pose history.
- **Scale and hierarchy.** A static collider may sit anywhere in a hierarchy that has no body; its world scale is baked into its shape when it is created or moved. A kinematic or dynamic body must be a root entity with unit scale. Its descendants follow it as ordinary children, their colliders join its compound shape with their scale baked in, and they may not have bodies of their own.
- **Unsupported shape scale.** A sphere needs uniform scale, and a capsule needs uniform scale in its radius axes. A body or collider that breaks these rules stops Play from starting, with a message naming the entity, like a missing required asset.

**Shapes for this milestone.**
- **Primitives:** box (half extents), sphere (radius), and capsule (radius and half height along local Y), each with a local offset and rotation.
- **One shape per collider component.** Compound shapes come from colliders on a body's descendants, as above.
- **Later:** mesh, convex-hull, and height-field shapes come with the content pipeline (Milestone 3) and streaming (Milestone 4).

**Collision groups and masks.**
- **Groups.** A project names up to 16 collision groups. Each collider has one group and a mask of the groups it collides with.
- **Filtering.** Two colliders collide only if each one's group is in the other's mask, which makes filtering symmetric.
- **Default.** Group `Default` colliding with every group.
- **Broad-phase layers.** The adapter's choice (static and moving, then sensors if needed), never authored.
- **Sensors.** A sensor collider (a trigger) reports overlaps but produces no contact response.
- **Surfaces and bodies.** Friction and restitution are collider properties. Mass (or density), damping, gravity factor, and initial velocity are body properties.
- **Gravity.** A scene setting, (0, −9.81, 0) m/s² by default.

**Threading and callbacks.**
- **The step.** Runs on the world's owner thread, which blocks while Jolt's jobs run.
- **Jolt listeners** (contact, activation, and step) run on worker threads. They only append plain records (tick, event kind, the two body indices, contact point and normal, and impulse) to per-step buffers. They never touch the World, scripts, the asset service, or the renderer, and they never throw.
- **After the step.** On the owner thread, the adapter resolves indices to entities, sorts the records deterministically, and delivers them in phase 7 ([scheduling](scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick)).
- **Queries** (raycasts, shape casts, overlaps) run on the owner thread between steps, against the last completed step. Their results are sorted by distance and then EntityId, because Jolt's broad-phase order is not deterministic. Queries from inside Jolt callbacks are not allowed.

**Precision and errors.** Physics runs in single precision in the same metres, seconds, and kilograms as the World, and inherits the open world-extent decision. Jolt returns no exceptions; step error flags become diagnostics. Maya code called by Jolt is `noexcept`.

**Integration contracts for later work.** These are out of scope for this milestone, but their seams are fixed now:

| Later feature | How it attaches |
| --- | --- |
| Constraints and joints | A constraint component names two entities by EntityId. The constraint is created after both bodies and destroyed with either. Breaking it is an event. |
| Character controllers | A controller component owns its entity's pose, like a kinematic body. It moves in phase 4 from gameplay input, using Jolt's `CharacterVirtual`, and reports ground state after phase 6. |
| Skeletal animation and ragdolls | Animation drives kinematic bodies in phase 4, or reads dynamic ragdoll bodies after phase 6. One motion authority per body still applies. |
| Streaming cells | A cell's bodies are added or removed in its activation or unload transaction at phase 1. Constraints, like hierarchies, may not cross independently unloadable cells. |
| Networking and rollback | Needs `CROSS_PLATFORM_DETERMINISTIC` or server authority, decided with the multiplayer requirements. Jolt's state snapshots (334 bytes for one box in the prototype) are the rollback primitive. |
| GPU physics | Not planned. |

## Scripting boundary

Recorded for #1015. The scripting host ([#1018](../scripting.md)) implements it, with a minimal script asset kind; [#1020](../scripting.md#reload) completes script assets in the editor and adds reload, and [#1021](https://work.rezee.app/kash/issues/1021) adds bodies, queries, events, and `late_fixed_update`. Luau is chosen in the [decision record](physics-scripting-decision.md). When scripts run is in [scheduling](scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick). Where #1018 differs from or narrows this contract:
- **Stop after destruction.** When an entity is destroyed, its instance's `stop` runs at the next tick boundary, after the entity is gone; `self.entity:alive()` is false there.
- **Schema.** Script values are stored as one `values` property of `maya.script`, validated for names, types, and finite data. The host checks them against the script's declarations when an instance starts: values that do not fit are reported and replaced by defaults. The Inspector draws one field per declared property, sorted by name.
- **Types.** Asset-typed script properties, and asset handles in scripts, are not implemented yet.
- **Setup counts.** Loading a script and setting up its instances count against the memory limit; a script that cannot be set up is that script's error.

**Ownership.** The Luau VM lives inside MayaSimulation's scripting host; Luau types do not appear in Maya's public headers. There is one VM per play session, created at session start and closed after the last stop hook. It runs only on the world's owner thread. Each script asset version is compiled once and runs in its own sandboxed thread. Authoring Worlds run no scripts.

**Script components.**
- **Attachment.** A `maya.script` component attaches a script asset to an entity and stores the values of the script's exposed properties. This milestone allows one script component per entity; shared modules (`require`) are out of scope.
- **What a script returns.** A table of hooks and property declarations:

```lua
local Mover = {}
Mover.properties = {
    speed = { type = "number", default = 3, min = 0, unit = "m/s" },
}
function Mover:start() end
function Mover:fixed_update(dt: number) end
function Mover:stop() end
return Mover
```

- **Instances.** Each entity with the component gets its own instance (`self`), which carries its entity and its property values. Per-entity state lives in `self`, never in script globals.
- **The hooks** are `start`, `fixed_update`, `late_fixed_update`, `update`, `stop`, and the event hooks; their phases are in the scheduling contract.
- **Type annotations** are allowed. They are not checked yet; checking them in the editor with Luau's analyzer is a later option.

**Exposed properties.**
- **Declarations.** `properties` declares each property's type (`number`, `integer`, `boolean`, `string`, `vector`, `color`, `entity`, or an asset type), its default, and optionally its range, unit, and label.
- **Schema.** The host turns the declarations into a property schema for that script asset version. The Inspector, scene files, and scripts all use it through the [property system](../properties.md), with the same validation as built-in components.
- **Identity.** A script property is identified by its name, since scripts have no numeric property IDs.
- **Stale values.** Values for properties a script no longer declares are kept in the scene and reported as unused. They are neither applied nor silently dropped.

**The engine API.** Scripts reach the engine only through the read-only `maya` table and methods on `self`. Values cross the boundary as numbers, booleans, strings, Luau vectors, quaternions, and opaque entity and asset handles; handles are checked again on every use. No component pointer, Jolt object, or native address reaches a script.

| Area | Reads | Writes (commands, visible from the next tick) | Issue |
| --- | --- | --- | --- |
| Own entity and others | Component properties through the property system; names; hierarchy; find by EntityId | Property edits through the same validation as the Inspector; create and destroy entities | #1018 |
| Transforms | Local and world pose of any entity | Local transform of entities without a body, and of static colliders | #1018 |
| Bodies | Velocity, mass, sleeping, motion type | Force, impulse, torque, velocity, kinematic target, teleport, and wake | #1021 |
| Queries and events | Raycast, shape cast, and overlap against the last completed step | Event hooks receive contacts and triggers | #1021 |
| Time and input | Tick, simulation time, fixed interval, this tick's input frame | None | #1018 |
| Diagnostics | None | `maya.log(message)` | #1018 |

**What scripts may not write.** A script may not write the transform of a kinematic or dynamic body, write a property outside its schema's validation, or change the world except through commands. A refused write is a script error at the call that attempted it.

**Write conflicts.** Two writes to the same property in one tick are applied in the stable script order, so the last one wins, and each is reported as a conflict diagnostic.

**Sandbox.**
- **Libraries.** Scripts see the read-only Luau libraries `string`, `table`, `math`, `bit32`, `utf8`, `buffer`, `vector`, and `coroutine`, plus `maya`.
- **Removed.** The host removes:
  - `print`, replaced by `maya.log`;
  - `debug`;
  - `os`, whose clock and date break determinism; simulation time comes from the hooks;
  - `getfenv` and `setfenv`.
- **No code loading.** Luau already has no file, process, environment, or bytecode-loading functions. Scripts reach code only through the host, which compiles source text; bytecode is never loaded from content.
- **Random numbers.** `math.random` is reseeded from the play session's seed when the VM is created, instead of Luau's clock-and-address seed.

**Limits.**
- **Work budget.** Each hook call has a budget counted at Luau safepoints (loop back edges and calls), never in wall time, so the same script and inputs always pass or always fail.
- **Memory.** Each play session's VM has a memory limit.
- **Defaults.** #1018 sets 1,000,000 safepoints per call and 64 MiB per session. An empty hook costs about 97 ns; the sample's spin script needs fewer than 5 safepoints per call and plays within 512 KiB ([scripting](../scripting.md#cost)). A project sets other values in its file (`script_work`, `script_memory`; [projects](../projects.md#projects)). The prototype used 10 million safepoints and 4 MiB.
- **Coroutines.** They may run within a call, but a hook that yields is an error; waiting across ticks is a later design.

**Errors.**
- **Messages.** Syntax, runtime, budget, and memory errors report the script, the line, and the message, with a traceback where Luau provides one. An error in a native function called by a script becomes a script error at that call; C++ exceptions never cross the VM unconverted.
- **An error in a hook** discards the commands that hook issued in that call, and disables that script instance for the rest of the session: no more hooks except `stop` if `start` was entered. Everything else keeps running.
- **Reporting.** The editor shows a notice and a "script" entry in Diagnostics; the player logs it. Scripts never crash the editor or the player.
- **Native systems.** A failing native `SimulationSystem` still stops the session, as in [play](../play.md#play-sessions).

## Future streaming boundary

A cell is a residency/activation unit, not an identity namespace or a second World implementation. Cell content and generated content enter the same entity, asset, mutation, and persistence APIs. Persist generator seed, parameters, and version; regeneration cannot silently change the identity of surviving authored entities.

Use explicit unloaded → loading → ready → active → unloading states, plus failed/cancelled results. Loading prepares data off-thread; ready content becomes visible atomically through an activation transaction. Bound work and resident/pending bytes; split oversized content into smaller activation units rather than publishing a half-initialized cell. Numeric limits remain part of the performance decision.

Every request carries a world lifetime token and monotonically changing request generation. Unload/cancel invalidates that generation; late completion is discarded and releases its temporary leases. Unload commits removal from simulation and extraction, invalidates runtime handles, and releases cell leases. A later reload preserves persistent IDs but creates fresh handles. A weak cross-cell entity reference becomes unresolved while absent; it does not silently pin the cell. Explicit pinning must be visible in residency diagnostics and subject to policy.

Initial transform hierarchies cannot cross independently unloadable cells. Move a whole subtree together, retain it in an explicitly persistent unit, or use a non-owning entity reference. Save dirty state or record an explicit discard decision before unloading; a load failure must not erase authored data. Origin shifting, global coordinates, cell dimensions, and save-delta formats remain open before production streaming.
