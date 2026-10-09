# World storage and component lifecycle

[Issue #991](https://work.rezee.app/kash/issues/991) implements the identity/storage part of the [architecture contracts](architecture/README.md). `MayaWorld` (`Maya::World`) is a CPU-only C++20 library with no GLFW, renderer, Metal, or editor dependency. `MayaRuntime` links it publicly. Include [world.hpp](../include/maya/world/world.hpp) and [components.hpp](../include/maya/world/components.hpp).

The legacy `Scene` drawing helper was removed by #998. The sample now opens a [scene file](scene.md) into a World and draws it through [render extraction](renderer.md), loading assets through the [#993 asset registry](assets.md). [Hierarchy/camera calculations](spatial.md) are implemented by #992. [Shared property metadata and validated edits](properties.md) are implemented by #994. [Scene persistence](scene.md) is implemented by #995. A physics/script scheduler remains follow-up work.

## Create and query

```cpp
maya::World world;
auto commands = world.commands();
const auto id = maya::EntityId::generate();
const auto pending = commands.create(id);
commands.add(pending, maya::NameComponent{"Main camera"});
commands.add(pending, maya::TransformComponent{});
commands.add(pending, maya::CameraComponent{});

const auto result = world.commit(commands);
if (!result) {
    // Report result.error and result.command_index; no part of the batch was published.
    return;
}
const auto camera = result.created[pending.index];
world.with<maya::NameComponent>(camera, [](auto& name) { name.value = "Camera A"; });
std::as_const(world).for_each<maya::TransformComponent, maya::CameraComponent>(
    [](maya::EntityHandle entity, const auto& transform, const auto& camera_data) {
        // Read data; copy what the caller needs. Do not retain component references.
    });
```

`with<T>` returns false without invoking the callback for an invalid handle or absent component. Const World queries expose const references. `for_each<Ts...>` visits entities with every requested component, using the smallest pool as its candidate set. `for_each_entity` enumerates live handles, including entities with no components. `has<T>`, `component_count<T>`, `size`, `find(EntityId)`, and `persistent_id(EntityHandle)` do not transfer ownership.

## Identity and ownership

- `EntityId` and `AssetId` are distinct 128-bit value types with `high`/`low` words. Zero is invalid. Generated IDs use a randomly seeded per-thread generator; uniqueness is still validated at commit. These are not cryptographic capabilities. The [scene format](scene.md) encodes them as two hexadecimal words; never serialize C++ object memory.
- Loading/reconstruction supplies the saved EntityId to `create(id)`. Duplicate live or staged IDs reject the batch. An ID destroyed in an earlier batch may be restored; destroying and recreating the same ID in one batch is rejected.
- `EntityHandle` contains a 64-bit World lifetime token, a 32-bit slot, and a 64-bit generation. Resolution checks all three plus slot liveness. Deletion invalidates the handle before component cleanup. Reuse increments generation; exhausted generations retire the slot, and exhausted lifetime tokens throw rather than wrap.
- Worlds are noncopyable/nonmovable and use fresh process-lifetime tokens even at a reused memory address. Reconstructing the same persistent IDs in another World does not revive old handles. World tokens, generations, pool order, and RTTI type keys are never persistent identifiers.
- World owns each component value. Components may own resources through RAII, including move-only values. Additions are owned by their command buffer until publication. Removing an entity/component or destroying a World releases its values. A failed/discarded buffer releases its staged values when destroyed, without touching live values.

## Mutation and borrowing

World and each command buffer have one owner thread; they are not internally synchronized. Thread-safe token generation does not make World access thread-safe. World must outlive all callbacks. Component moves/destructors must not re-enter the World or perform fallible lifecycle work; future script activation/teardown is a separate subsystem phase.

Create/destroy/add/remove/replace, transform edits, and reparent operations are recorded in `WorldCommands` and take effect only through an explicit `commit` at an application-controlled boundary. A `PendingEntity` addresses an earlier create in the same buffer; it is not a live handle. Buffers are movable and may safely outlive the World, but cannot commit into another lifetime. Success consumes the buffer; attempting to append to a consumed or moved-from buffer throws `std::logic_error`.

`WorldCommands::replace<T>` atomically replaces an existing component in enqueue order without changing entity identity or component pool order. Transform replacement uses `set_transform`; other native replacements are trusted values. Use the [property validation API](properties.md) for authoring, imports, or scripting input.

`WorldCommands::staged(first)` lists the staged commands from an index on, in order: each one's kind, target, component type, and new parent, without its value. It lets a caller check a batch before committing it; play sessions use it to refuse transform writes to [physics bodies](physics.md#requests-during-a-tick) (#1017). `for_each_staged(first, visit)` walks the same entries without copying the batch; play sessions use it to keep [pose history](play.md#between-ticks) (#1016).

`WorldCommands::truncate(size)` drops the commands staged after the first `size` (from `size()`), as if they were never made; pending entities from dropped creates must not be used again. The [script host](scripting.md#errors) uses it to discard a failed hook call's commands (#1018).

Commit validates commands in enqueue order. Missing components, duplicate additions/IDs, commands after destruction, stale/foreign handles, and invalid pending targets return a `WorldError` and failing index. No prefix of a rejected batch is applied. Remove followed by add replaces a component. Creating then destroying a pending entity is allowed; its result handle is already invalid when commit returns. Failed buffers retain their staging values and may be discarded or retried after a transient `busy` result.

After validation, commit allocates storage/map nodes before publication. Allocation/construction failures propagate as exceptions while logical world contents remain unchanged (capacity may grow). Components must meet the `Component` concept: unqualified object type with nonthrowing move construction, move assignment, and destruction. This permits packed relocation and a publication phase without allocating or invoking throwing component operations. Fallible value construction/copying happens while staging, before live state changes.

`with`, component iteration, and entity enumeration hold RAII borrow guards. Nested callbacks are allowed; `commit` returns `busy` until every guard exits, including exception unwinding. References are valid only within their callback and must not escape into another system, stored pointer, or job. Transform queries always expose const values; use validated `set_transform` commands. Other component field edits through mutable callbacks are immediate and are **not** transactional or automatically rolled back if the callback throws. Property validation and undo commands arrive in #994/#1000.

```cpp
auto removals = world.commands();
world.for_each<maya::MeshRendererComponent>([&](auto entity, const auto& mesh) {
    if (!mesh.visible) removals.destroy(entity);
});
const auto removed = world.commit(removals); // after iteration, at the owner's boundary
```

No automatic frame/tick commit exists yet. The future scheduler chooses the boundary and merges producer commands in its declared order. This storage transaction covers entity/component and hierarchy operations; it does not claim rollback of arbitrary script, I/O, or GPU side effects.

## Storage decision and limits

World uses a slot table, an intrusive free-slot chain, a packed list of live slots, and a persistent-ID hash map. Each component type has a sparse slot-to-dense index and packed vectors of values/owner slots. Lookups are O(1) by runtime handle/component; persistent-ID lookup is average O(1). Swap-removal is O(1) per pool, and entity destruction visits the registered pools. Component/entity enumeration order can change after removal and is not a simulation ordering guarantee; consumers requiring stable order must select one explicitly.

This small sparse-set implementation keeps storage independent of an external ECS API. Queries resolve pools once and visit the smallest candidate pool rather than scanning every entity. Vector and hash-table capacity grows geometrically, and `World::reserve(entities)` makes room ahead, so a commit that passes the last size does not rehash every persistent ID (#1064 found an exact reserve rehashing 60,000 IDs on each 256-entity commit). A commit stages only touched entities/components and does not copy the whole World; its scratch tables are sized once per batch. Each pool's sparse indices extend to the World's slot high-water mark when it receives additions. Empty pools and vector capacities are retained until World destruction; paging, trimming, archetypes, parallel access, and storage tuning require measured workloads before adoption. The 10,000-entity correctness test is not a production performance budget.

## Initial component schemas

| Component | Stored data / next integration |
| --- | --- |
| `NameComponent` | String value; not identity. |
| `TransformComponent` | Local translation in metres, quaternion x/y/z/w, positive scale defaulting to one. World owns hierarchy links and derived matrices; see [spatial operations](spatial.md). |
| `MeshRendererComponent` | Typed mesh/material AssetId references and visibility. References do not load or pin assets; [#993 leases](assets.md) supply residency/ownership. |
| `CameraComponent` | Vertical FOV in radians, near/far clip in metres. Aspect belongs to a rendered view; no window/input ownership. |
| `LightComponent` | Kind, linear RGB, intensity (directional lux, point/spot candela since #1034), local-light range, spot cone full angles in radians, enabled flag, and shadow settings: whether it casts them, bias and normal bias in shadow-map texels, and a directional light's shadow distance ([lights](renderer.md#lights), [shadows](renderer.md#shadows)). |

Entities start with no implicit components. The component schemas have usable defaults and are ordinary component values, not GPU bindings. This layer validates identity and structural lifecycle; #992 adds transform validation and validated camera calculations. Shared property validation is #994, and the [renderer](renderer.md) reads mesh renderers, cameras, and directional lights since #998. Mutable fields are not yet a validated inspector or scripting API.

## Worlds and cells

[#1064](https://work.rezee.app/kash/issues/1064) divides large worlds into cells that stream, in the format and sizes [#1060 decided](architecture/world-scale-decision.md#cells-a-128-m-grid-cooked-to-binary). A world is not a second World implementation: cells load into the same World, entities, components, and physics as a scene does.

### Worlds on disk

A world is a `.world` file ([world_io.hpp](../include/maya/scene/world_io.hpp), in MayaScene) and a folder of scenes beside it:

```text
levels/w1.world
  maya-world 1
  cell_size 128
  persistent "w1/persistent.scene"
  cell 32 -64 "w1/cells/32_-64.scene" 412
  cell 33 -64 "w1/cells/33_-64.scene" 380
levels/w1/persistent.scene
levels/w1/cells/32_-64.scene ...
```

- **The grid** is uniform, 128 m in x and z; a cell is a column, `cell_of(position)` = the floor of x and z over the cell size, as two 32-bit integers.
- **The persistent part** is always loaded: what the whole world needs. `partition_world` puts a root entity and everything below it there when any of them is a camera, a directional light, an environment, or physics settings.
- **Cells.** Every other root goes, with its whole subtree, to the cell of its translation, so a hierarchy never crosses cells. A cell is listed with its entity count, and only occupied cells are listed.
- **`save_world`** validates the scene, then writes the persistent scene and each cell's scene (ordinary [scene text](scene.md)), each atomically, removes the scenes of cells no longer occupied, and writes the world file last. `load_world` reads everything for tests and tools; play streams instead.
- **Cooked cells.** When a cell is loaded, its scene text is cooked to a [packed binary](scene.md#cooked-cells) in the project's cook cache, keyed by the text's digest and the build's schemas, so later loads decode it (#1060 measured 0.25 µs an entity against 3.4 µs to parse). A package holds its cells only cooked (`.cell`).

### Staged entities

A cell must appear to systems all at once, but committing a big one takes longer than a frame's budget. So the World holds **stage groups** ([commands.hpp](../include/maya/world/commands.hpp)):
- `WorldCommands::create_staged(id, group)` creates an entity that is committed but invisible: `find`, `alive`, `has`, `with`, `for_each`, `for_each_entity`, `children`, `world_matrix`, and `size` do not see it. Later batches of the same activation still target it by handle, so a cell goes in over several frames. Its ID is taken meanwhile.
- `World::publish(group)` makes the whole group visible in one step (it allocates first, then flips each entity), and returns them.
- `World::stage(entities, group)` hides visible entities in one step, as an unloading cell does; they must be whole hierarchies, or it refuses with `stage_mismatch` and changes nothing.
- `World::discard(group, limit)` destroys a group's entities, at most `limit` at a time, so a big cell leaves over several frames, unseen.
- Hierarchies stay inside a group: reparenting between groups, or between a staged and a visible entity, is `stage_mismatch`.
- `component_count` counts staged entities' components too.

Two journals let owners avoid scanning: commits return every entity they destroyed (`WorldCommitResult::destroyed`); `record_transform_changes` lists entities whose transforms changed (physics re-places only static bodies below them); and `record_edits` lists entities published from a stage group whose components changed or that were destroyed, each with that group (`WorldEdit`), so the streamer finds the cell from the group, with no index of every entity.

### Streaming

`WorldStreamer` ([world_streamer.hpp](../include/maya/streaming/world_streamer.hpp), in MayaStreaming) streams a world's cells into a play session's World:

| State | Means |
| --- | --- |
| unloaded | Nothing of it is held. |
| loading | A background job reads (and cooks) its scene. |
| ready | Its content is decoded and held, out of the World. |
| activating | Its entities are going into the World, staged, a frame's budget at a time. |
| active | Published: visible to systems, extraction, and queries, with its bodies. |
| deactivating | Hidden in one step, out of physics in one batch, and being destroyed a budget at a time. |
| failed | Its load or activation failed; nothing of it is in the World. It is tried again once it leaves the load radius and comes back. |

- **Sources** are world positions: the camera, the player. A cell loads when a source is within the load radius of its square, and activates within the activation radius; each goes back down only beyond its radius plus the hysteresis, so a source on a boundary does not thrash. The radii are project settings (`stream_load`, `stream_activate`, `stream_hysteresis`; 640, 384, and 64 m by default, W1's).
- **A frame** (`update`, on the owner thread between ticks): finished loads are applied, the edit journal is read, and each cell's distance and wanted state are decided. Then the work runs within the frame's budget (1 ms and 4,096 entities by default): cells going out first, farthest first, then the nearest cells coming in.
- **The budget is kept by measuring.** Each step's cost is measured as it runs (an entity committed, an entity discarded, a cell finished, a cell hidden), and a step starts only if its measured cost fits the time left, planned to 0.9 ms. A dearer measure counts at once and a cheaper one a tenth of the way, so the plan errs long. Commits take at most 128 entities, so one misjudged step overruns little. The frame's first step always runs, so streaming moves forward even in a frame that began late. Hiding a cell (its changes kept, `stage`, `remove_bodies`) and finishing one (`publish`, `authored_physics`, `create_bodies`) are single steps: their cost grows with the cell's entities and bodies (about 0.4 ms each for the stream benchmark's 2,000-entity, 100-body cells; bodies cost about 4 µs each), so a cell far bigger than that would overrun a frame.
- **Content is arranged off the owner thread.** The load job orders a cell's entities parents first and records each parent's index, so activation looks nothing up by ID, and a parent is committed before its children whatever order the file lists them in. Content no longer needed is freed by a job: freeing a cell's thousands of values took the owner thread up to 4.5 ms.
- **`settle` reserves the World** for the most the sources can hold active (the cells within reach of the activation radius and hysteresis, at the world's mean cell size), so streaming does not grow the World's tables mid-play.
- **Loads** run on the job system's background tier, at most 8 at once, and at most 64 MiB of cooked cells in flight (estimated at 256 bytes an entity until loaded). Past the cells' [resident budget](assets.md#residency) (`resident_bytes`, from the project's `resident_cells`, 64 MiB by default), only cells that must activate load: those within the load radius alone wait, and `settle` does not wait for them (#1063). Each carries the cell's generation: a load given up (the source left) is cancelled, and one that finishes anyway is discarded when its completion arrives. Destroying the streamer cancels its jobs and waits for them; the World is never touched after.
- **Loading screens.** `settle` brings every cell to what the sources want now, waiting for loads and ignoring the frame budget: the player calls it before its first frame.
- **Instruments** (`StreamingStats`): cells by state; loads started, finished, failed, and cancelled; completions discarded; activations, deactivations, and cells kept active; the last and longest frame's streaming time; bytes loading and loaded; active entities; and cells holding play's changes.

### Cross-cell references

References to entities are by persistent ID (a script's entity values, an animation's clip targets by name path), so they are weak: one into an unloaded cell is unresolved (`find` returns nothing), never dangling, and resolves again, to a fresh handle, when the cell is active again. Nothing about a reference keeps its cell loaded; the streamer never looks at them. Play can join a cell's entities to others' hierarchies (reparenting across cells): such a cell cannot be hidden whole, so it is kept active (`pinned`), and the streamer reports it.

### State across unloads

When an active cell goes out, the entities the edit journal named are compared with the cooked cell: destroyed ones, and the components of changed ones that keep state (`ComponentDescriptor::keeps_state`; every component today), are kept as the cell's delta (`CellDelta`). When it comes back, the delta is applied before it activates: destroyed entities and everything below them stay gone, and changed ones get their components back. Authored data is never changed by play: the delta lives in the streamer, so a new play session starts from the authored world. Entities scripts create during play belong to no cell, and stay loaded.

### Physics

A cell's bodies join Jolt in one batch when it is published (`authored_physics` over its entities, then `PhysicsWorld::create_bodies`), and leave in one batch when it is hidden (`PhysicsWorld::remove_bodies`). `PhysicsWorld::commit` removes the bodies of the entities the batch destroyed (`WorldCommitResult::destroyed`) instead of checking every body, and `prepare` re-places only the static bodies below entities the transform journal named, skipping the moving bodies' own write-backs, instead of checking every static body: per-tick cost no longer grows with the number of bodies ([measurements](#measurements)). Removed body records are compacted in place once they outnumber live ones, re-indexing only the records that move. Constraints do not exist yet; mesh and height-field shapes come with #1068.

### Play and the player

The [player](play.md#the-player) runs a `.world` as it runs a scene: its persistent part plays as the scene, the streamer settles the cells around the camera behind the loading screen, and every frame streams around the camera before the tick. Recording and replaying a streamed world are refused for now: streaming is not deterministic with respect to ticks. The editor's Play of a world comes with #1067, when the editor can open one. A [package](projects.md#packages) holds a world's file, its persistent scene, and its cells cooked.

### Tests

[streaming_tests.cpp](../tests/streaming_tests.cpp) (CTest `maya_streaming`, cpu), [world_tests.cpp](../tests/world_tests.cpp) ("[stages]"), and [package_tests.cpp](../tests/package_tests.cpp) ("[streaming]"):
- the world format, partitioning, and read errors; the packed binary codec round trip and refusing damage and other schemas;
- a 5 × 5 world crossed and crossed back by a source: settled at each step, the World holds exactly the persistent part and the active cells, as a whole-world load would, and cells within and beyond the radii are as they should be;
- activation committing at most the frame's entities, invisible until the cell is complete, with its bodies in one batch;
- a cell listing 100 three-level trees grandchildren first, activating over many commits with every hierarchy and pose whole, and a destroyed root's children and grandchildren staying gone after an unload;
- delayed, failed (and retried), and cancelled loads, a world closed with loads in flight, and 400 frames straddling a cell boundary (no deactivations, then everything unloads, no staged entities left, and every body created removed);
- a weak reference from the persistent camera into a cell: resolved, unresolved without keeping the cell, and resolved again;
- play's changes (a moved prop, a destroyed one with its child) surviving an unload, and a new session starting authored;
- a ball resting on its cell's ground, its bodies leaving and returning with the cell, at the same rest;
- stage groups: invisibility, later batches targeting staged entities, `stage_mismatch` (refused staging changes nothing), publish, discard and reusing IDs, the destroyed list, the journals, and edits naming their group;
- a world packaged with cooked cells streams exactly as from the project, and the packaged player streams it around its camera.

### Measurements

Release, M4 Pro, 9 October 2026, with other applications running (the system had 3.1 GB in swap). Two runs of the [stream benchmark](performance.md#stream) (16 × 16 cells of 2,000 entities, 512,000 in all, crossed 10 times at 60 m/s with W1's radii, 20,480 frames each):

| | Run 1 | Run 2 |
| --- | --- | --- |
| Streaming a frame: median, P95, P99 | 0.031, 0.903, 0.919 ms | 0.030, 0.903, 0.916 ms |
| Frames over the 1 ms budget | 42 (0.2 %) | 41 (0.2 %) |
| Longest streaming frame | 2.29 ms | 2.90 ms |
| Whole frame (streaming and the tick), P99 | 1.19 ms | 1.19 ms |
| Activations, deactivations, loads | 886, 860, 1,238 | 886, 860, 1,238 |
| Footprint after each crossing | 347–416 MiB, flat after the first | 345–355 MiB |

Every crossing ends with 52,026 entities active and 14.9 MB of cooked cells loaded; no load was cancelled or failed. The frames over budget are the 0.2 % whose measured costs misjudged a step or whose thread the system delayed; before steps were planned from measured costs, the same benchmark had P99 3.4 ms, a quarter of frames over budget, and a longest frame of 14 ms (a 256-entity commit rehashing every persistent ID, the owner thread freeing whole cells, and unbudgeted hides).

P1 against the commit before #1064 (Release, two runs each, default workers and none):

| Per tick | 5,000 bodies before | after | 20,000 bodies before | after |
| --- | --- | --- | --- | --- |
| Body commit | 0.009 ms | 0.000 ms | 0.036 ms | 0.000 ms |
| Preparation | 0.036 ms | 0.025 ms | 0.080 ms | 0.147 ms |
| Whole tick (default workers) | 6.09 ms | 6.09 ms | 18.56 ms | 20.96 ms |

The commit no longer grows with the bodies. Preparation now grows with the bodies that moved (about 9,000 active at 20,000) rather than with the static bodies, which grow with a streamed world; the 20,000-body tick's difference is in Jolt's step and the queries, which #1064 does not touch (run to run noise on a loaded machine; with no workers, 46.16 and 46.48 ms).

## Verification

```bash
cmake --build build --target maya_world_tests
ctest --test-dir build -L world --output-on-failure
```

The World tests link only MayaWorld and Catch2, so they need no window/GPU. Coverage includes ID reconstruction, wrong/dead/reused handles, repeated World lifetimes, atomic rejection, pending-buffer ownership, RAII cleanup, mutation exclusion, callback exceptions, multi-component queries, and 10,000 entities across repeated growth/removal/reuse cycles. Use the normal sanitizer CMake options; keep sanitizer results separate from performance measurements.

Historical #991 validation on 23 September 2026: all application targets built in a fresh Release configuration with no compiler diagnostics. The final World suite passed 13 cases / 80,314 assertions in Release and UBSan. The existing CPU/CLI checks and all five Metal/lifecycle/application smoke checks passed. Clang static analysis of the two new implementation files reported no findings. A separate bounded CPU microbenchmark exercised 1,000/10,000/100,000 entities and 10,000 incremental commits; it is not the versioned renderer/physics benchmark planned in #1004.

The ASan/UBSan test binary built, but timed out before emitting test output. A newly compiled empty-main ASan/UBSan program also timed out. Both were terminated. ASan validation remains blocked by the local runtime/toolchain startup problem; the UBSan result does not establish an ASan pass.
