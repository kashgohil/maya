# World storage and component lifecycle

[Issue #991](https://work.rezee.app/kash/issues/991) implements the identity/storage part of the [architecture contracts](architecture/README.md). `MayaWorld` (`Maya::World`) is a CPU-only C++20 library with no GLFW, renderer, Metal, or editor dependency. `MayaRuntime` links it publicly. Include [world.hpp](../include/maya/world/world.hpp) and [components.hpp](../include/maya/world/components.hpp).

The existing `Scene` remains a legacy sample drawing helper. It is not the new world model. The sample loads its mesh through the [#993 asset registry](assets.md); World render extraction remains #998. [Hierarchy/camera calculations](spatial.md) are implemented by #992. [Shared property metadata and validated edits](properties.md) are implemented by #994. Serialization (#995) and a physics/script scheduler remain follow-up work.

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

- `EntityId` and `AssetId` are distinct 128-bit value types with `high`/`low` words. Zero is invalid. Generated IDs use a randomly seeded per-thread generator; uniqueness is still validated at commit. These are not cryptographic capabilities. File encoding remains #995's responsibility; do not serialize C++ object memory.
- Loading/reconstruction supplies the saved EntityId to `create(id)`. Duplicate live or staged IDs reject the batch. An ID destroyed in an earlier batch may be restored; destroying and recreating the same ID in one batch is rejected.
- `EntityHandle` contains a 64-bit World lifetime token, a 32-bit slot, and a 64-bit generation. Resolution checks all three plus slot liveness. Deletion invalidates the handle before component cleanup. Reuse increments generation; exhausted generations retire the slot, and exhausted lifetime tokens throw rather than wrap.
- Worlds are noncopyable/nonmovable and use fresh process-lifetime tokens even at a reused memory address. Reconstructing the same persistent IDs in another World does not revive old handles. World tokens, generations, pool order, and RTTI type keys are never persistent identifiers.
- World owns each component value. Components may own resources through RAII, including move-only values. Additions are owned by their command buffer until publication. Removing an entity/component or destroying a World releases its values. A failed/discarded buffer releases its staged values when destroyed, without touching live values.

## Mutation and borrowing

World and each command buffer have one owner thread; they are not internally synchronized. Thread-safe token generation does not make World access thread-safe. World must outlive all callbacks. Component moves/destructors must not re-enter the World or perform fallible lifecycle work; future script activation/teardown is a separate subsystem phase.

Create/destroy/add/remove/replace, transform edits, and reparent operations are recorded in `WorldCommands` and take effect only through an explicit `commit` at an application-controlled boundary. A `PendingEntity` addresses an earlier create in the same buffer; it is not a live handle. Buffers are movable and may safely outlive the World, but cannot commit into another lifetime. Success consumes the buffer; attempting to append to a consumed or moved-from buffer throws `std::logic_error`.

`WorldCommands::replace<T>` atomically replaces an existing component in enqueue order without changing entity identity or component pool order. Transform replacement uses `set_transform`; other native replacements are trusted values. Use the [property validation API](properties.md) for authoring, imports, or scripting input.

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

This small sparse-set implementation keeps storage independent of an external ECS API. Queries resolve pools once and visit the smallest candidate pool rather than scanning every entity. Vector capacity grows geometrically; a commit stages only touched entities/components and does not copy the whole World. Each pool's sparse indices extend to the World's slot high-water mark when it receives additions. Empty pools and vector capacities are retained until World destruction; paging, trimming, archetypes, parallel access, and storage tuning require measured workloads before adoption. The 10,000-entity correctness test is not a production performance budget.

## Initial component schemas

| Component | Stored data / next integration |
| --- | --- |
| `NameComponent` | String value; not identity. |
| `TransformComponent` | Local translation in metres, quaternion x/y/z/w, positive scale defaulting to one. World owns hierarchy links and derived matrices; see [spatial operations](spatial.md). |
| `MeshRendererComponent` | Typed mesh/material AssetId references and visibility. References do not load or pin assets; [#993 leases](assets.md) supply residency/ownership. |
| `CameraComponent` | Vertical FOV in radians, near/far clip in metres. Aspect belongs to a rendered view; no window/input ownership. |
| `LightComponent` | Kind, linear RGB, intensity (directional lux, point/spot lumens), local-light range, spot cone full angles in radians, enabled flag. Renderer interpretation follows later. |

Entities start with no implicit components. The five schemas have usable defaults and are ordinary component values, not GPU bindings. This layer validates identity and structural lifecycle; #992 adds transform validation and validated camera calculations. Shared property validation is #994, and renderer use is #998. Mutable fields are not yet a validated inspector or scripting API.

## Verification

```bash
cmake --build build --target maya_world_tests
ctest --test-dir build -L world --output-on-failure
```

The World tests link only MayaWorld and Catch2, so they need no window/GPU. Coverage includes ID reconstruction, wrong/dead/reused handles, repeated World lifetimes, atomic rejection, pending-buffer ownership, RAII cleanup, mutation exclusion, callback exceptions, multi-component queries, and 10,000 entities across repeated growth/removal/reuse cycles. Use the normal sanitizer CMake options; keep sanitizer results separate from performance measurements.

Historical #991 validation on 23 September 2026: all application targets built in a fresh Release configuration with no compiler diagnostics. The final World suite passed 13 cases / 80,314 assertions in Release and UBSan. The existing CPU/CLI checks and all five Metal/lifecycle/application smoke checks passed. Clang static analysis of the two new implementation files reported no findings. A separate bounded CPU microbenchmark exercised 1,000/10,000/100,000 entities and 10,000 incremental commits; it is not the versioned renderer/physics benchmark planned in #1004.

The ASan/UBSan test binary built, but timed out before emitting test output. A newly compiled empty-main ASan/UBSan program also timed out. Both were terminated. ASan validation remains blocked by the local runtime/toolchain startup problem; the UBSan result does not establish an ASan pass.
