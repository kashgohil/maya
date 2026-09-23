# Runtime and world contracts

Status: intended implementation contracts for [#990](https://work.rezee.app/kash/issues/990). See [the index](README.md) for current-code gaps and unresolved product decisions.

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

Allow finite positive nonuniform scale for visual entities. Parent composition can produce shear: keep the full affine world matrix rather than decomposing it each frame. Transform normals with the inverse transpose of the world linear transform. Initially reject zero/negative scale, singular transforms, and nonfinite values at authoring/load boundaries; mirror authoring and its winding/tangent rules need an explicit extension. Normalize finite nonzero quaternions; reject a zero quaternion. Floating-point tolerances must be explicit and tested in #992, not hidden in arbitrary clamps.

Reparenting rejects cycles and cross-world parents. Offer explicit keep-local or keep-world semantics. A keep-world operation that cannot represent the new local transform as supported TRS (for example, requiring local shear) fails without changing the hierarchy. Camera and physics adapters enforce their additional restrictions; they never silently discard scale/shear. Physics body scale is initially baked into shapes before activation; dynamic/kinematic bodies must be root entities with unit transform scale until an explicit parent/body synchronization design is validated.

## Future streaming boundary

A cell is a residency/activation unit, not an identity namespace or a second World implementation. Cell content and generated content enter the same entity, asset, mutation, and persistence APIs. Persist generator seed, parameters, and version; regeneration cannot silently change the identity of surviving authored entities.

Use explicit unloaded → loading → ready → active → unloading states, plus failed/cancelled results. Loading prepares data off-thread; ready content becomes visible atomically through an activation transaction. Bound work and resident/pending bytes; split oversized content into smaller activation units rather than publishing a half-initialized cell. Numeric limits remain part of the performance decision.

Every request carries a world lifetime token and monotonically changing request generation. Unload/cancel invalidates that generation; late completion is discarded and releases its temporary leases. Unload commits removal from simulation and extraction, invalidates runtime handles, and releases cell leases. A later reload preserves persistent IDs but creates fresh handles. A weak cross-cell entity reference becomes unresolved while absent; it does not silently pin the cell. Explicit pinning must be visible in residency diagnostics and subject to policy.

Initial transform hierarchies cannot cross independently unloadable cells. Move a whole subtree together, retain it in an explicitly persistent unit, or use a non-owning entity reference. Save dirty state or record an explicit discard decision before unloading; a load failure must not erase authored data. Origin shifting, global coordinates, cell dimensions, and save-delta formats remain open before production streaming.
