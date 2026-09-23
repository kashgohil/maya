# Component properties and validated edits

[Issue #994](https://work.rezee.app/kash/issues/994) adds shared runtime property schemas for future inspectors, scene persistence, and scripting. The [schema API](../include/maya/properties/schema.hpp) lives in `MayaWorld` / `Maya::World`, with no editor, graphics, window, or asset-loading dependency. The [asset validation adapter](../include/maya/assets/property_context.hpp) belongs to `MayaAssets` / `Maya::Assets`.

## Discovery and identity

`component_schemas()` returns immutable descriptors with stable addresses; `component_schema` and `property_schema` look up IDs or stable names. Every property has its exact value type, a default derived from its C++ component's default construction, numeric bounds and inclusivity, units, a display label/presentation hint, and a persistence encoding. Light kinds expose named choices. Descriptions record contextual constraints such as intensity units and cone relationships. All 17 initial properties are editable and persistent; derived matrices, hierarchy links, runtime handles, and asset residency are not properties in these schemas.

| Component ID / stable name | Property IDs / stable names |
| --- | --- |
| 1 / `maya.name` | 1 `value` |
| 2 / `maya.transform` | 1 `translation`, 2 `rotation`, 3 `scale` |
| 3 / `maya.mesh_renderer` | 1 `mesh`, 2 `material`, 3 `visible` |
| 4 / `maya.camera` | 1 `vertical_fov`, 2 `near_clip`, 3 `far_clip` |
| 5 / `maya.light` | 1 `kind`, 2 `color`, 3 `intensity`, 4 `range`, 5 `inner_cone`, 6 `outer_cone`, 7 `enabled` |

A persistent property identity is the pair `(ComponentId, PropertyId)`. Property IDs are scoped to their component; zero is reserved. Display labels, table order, RTTI, C++ layout, variant indices, and pointer addresses never identify saved data. Lookup returns null for unknown identities. The initial built-in schema set is deliberately closed; adding another component requires a typed variant alternative, schema bindings, default/snapshot dispatch, and validation tests. There is no dynamic plugin registration or script VM yet.

## Reading and editing

`ComponentValue` owns a typed snapshot; `PropertyValue` owns a typed field value. `default_component`, `read_component`, and `read_property` return copies. No component pointer escapes a World query. Member bindings derive the value type, default, and accessor from the same C++ member; no raw offsets or unchecked casts are used.

```cpp
#include "maya/properties/schema.hpp"
#include <array>

// `world` and `entity` already identify an entity with CameraComponent.
const auto edits = std::array{
    maya::PropertyEdit{2, 2.0f},     // near_clip
    maya::PropertyEdit{3, 500.0f},   // far_clip
};
const auto result = maya::edit_properties(
    world, entity, maya::ComponentId::camera, edits);
if (!result) {
    // Report result.message, result.property, and (if relevant) result.world_error.
}
```

Edits are applied to a private copy. Type checks and duplicate/unknown-ID checks run before complete-component validation. Only the final values are validated, so near/far or inner/outer cone pairs can change together without an invalid intermediate state. A failure leaves the original component unchanged. `validate_component` uses the same path with no field changes; it can normalize a detached component before creation/import. Allocation and resolver exceptions propagate before publication and leave the source unchanged. `PropertyResult::property == 0` means a component-level or World-level error.

World edits publish one replacement in an atomic command batch. `WorldCommands::replace<T>` also supports native command users and move-only components: the target must already have T at that point in the batch; failed later commands roll back the whole batch. It retains entity identity and component pool order. Transform replacement delegates to `set_transform`, preserving hierarchy and invalidating descendant matrix caches. Replacement does not remove/re-add a component or reserve additional component storage. Other transaction staging can allocate.

All access obeys the World's single-owner-thread rule. A property edit during `with`/`for_each` can read a snapshot but fails publication with `WorldError::busy`. Stale or foreign entities and missing components fail explicitly. Resolver callbacks must be read-only and must not mutate or re-enter the World or asset services.

The detached API lets a future scene loader validate an entire unpublished scene and then construct a command batch. The immediate World overload edits one component at a time; multi-entity authoring transactions and undo belong to #1000. Native `add`, `replace`, and mutable non-transform query access remain trusted low-level APIs, and do not automatically run property/catalog validation. Inspectors, importers, and scripting must use this shared boundary rather than writing component fields directly. No property system can validate asset identities without a project catalog.

## Validation rules

| Data | Accepted values and normalization |
| --- | --- |
| Name | Owned string, including empty; future file codecs must validate their own text encoding. |
| Translation | Finite metres; local matrix and inverse must remain representable. |
| Rotation | Finite nonzero quaternion; normalized with the same helper used by World transform commands. |
| Scale | Finite, strictly positive per axis, plus existing [spatial numerical limits](spatial.md). No reflection or zero scale. |
| Camera | Finite FOV strictly between 0 and pi radians; finite near clip > 0, far clip > near clip. Aspect and camera pose are view-level constraints. |
| Light color / intensity | Finite nonnegative linear RGB (HDR above one allowed), finite nonnegative intensity. Directional intensity is lux; point/spot intensity is lumens. |
| Light range / cones | Finite range > 0; full-angle cones in radians with 0 <= inner <= outer < pi and outer > 0. Validate inactive fields too, so switching light kind does not reveal invalid values. |
| Light kind / flags | Only named directional/point/spot values; flags require exact bool values. |
| Asset references | Empty means unassigned. Nonempty ID must exist in the supplied catalog and have the correct mesh/material kind. |

Numbers are rejected instead of clamped. Numeric ranges in descriptors drive generic validation; complete-component rules handle relationships and transform normalization. Rotation is the only normalization. Camera property validity does not guarantee a representable projection at every aspect/pose/extreme scale; `camera_matrices` retains its numerical checks. Local transform validity similarly does not guarantee that composing an arbitrarily deep hierarchy will stay representable.

Pass `asset_property_context(registry)` when editing components with nonempty references. The returned context borrows the registry, which must outlive it. Validation does not load an asset, pin a resource, or require a source file to exist. Registered unloaded/failed assets retain valid identity; loading diagnostics and [fallback policies](assets.md) remain the registry/renderer responsibility. A missing catalog entry, a wrong asset kind, and an omitted resolver are distinct errors. Validation checks the entire final component, including unchanged reference fields; clearing all references needs no resolver. It is not a check that a GPU mesh remains resident.

## Persistence and schema evolution

All five schemas start at version 1. The scene codec itself is #995; these rules define its input contract:

- Persist component identity/version and property identity using the stable IDs or names above. Choose one canonical representation in the codec, and reject conflicting ID/name pairs if both are supplied. Never serialize object memory or variant ordinals.
- Encode floats/vectors as finite scalar values, quaternion as normalized `(x,y,z,w)`, boolean as boolean, string as text, and light kind using the stable choice names. Asset properties encode only the 128-bit persistent ID (zero/unassigned explicitly), never a path, lease, pointer, registry token, or runtime handle.
- Never reuse a retired component/property ID or stable name. A display-label change does not change identity. Renaming a persistence key requires an explicit migration/alias; reordering the table does not.
- Increment the component schema version when persistent shape, units, default semantics, or validation meaning changes. Additive properties need explicit migration defaults for older versions. A changed default must not silently reinterpret old omitted fields.
- Type/unit changes require an explicit migration; do not reinterpret bytes or coerce strings/numbers. Keep migrations keyed by source and destination versions, then validate the complete migrated component with these same APIs.
- Unknown properties, duplicate fields, unknown enum names, or newer unsupported versions must produce diagnostics before publication. A future preservation mode must retain unknown data explicitly; silently dropping it is not allowed. Migration failures must not partially mutate the live World.

Presentation hints are descriptive data, not widget code. An inspector can display degrees while converting to radians at this boundary, or show Euler controls while storing a quaternion. Such conversions must not change persistence units.

## Validation and limits

The CPU-only `maya_property_tests` covers discovery/defaults, stable lookup, typed reads/writes, batch rollback, numeric boundaries/nonfinite values, quaternion normalization, camera and cone relations, catalog identity/kind without loading, resolver exceptions, World borrows/identity/hierarchy/cache invalidation, and ordered/move-only component replacement. Existing World, assets, runtime, and desktop tests provide regression coverage. Validation on 23 September 2026:

- Release build of all targets completes without compiler diagnostics; all 15 CTest entries pass (10 CPU/CLI and five GPU/smoke).
- Property tests: 12 cases / 351 assertions pass in Release and UBSan. The World suite (27 cases / 147,640 assertions) and asset suite (14 cases / 307 assertions) also pass under UBSan.
- Clang static analysis reports no findings in schema.cpp, property_context.cpp, or the updated world.cpp. Property tests link only MayaWorld, MayaAssets, and Catch2; a separate metadata/edit benchmark links only MayaWorld.
- ASan/UBSan builds successfully, but the property executable times out before any output after 15 seconds and is terminated. The startup limitation recorded in previous issues persists; ASan is not recorded as passing.
- A bounded Release CPU microbenchmark exercised 1,000 / 10,000 / 100,000 detached camera edit/read pairs and edits across equally sized Worlds. At 100,000, one local run took about 2.49 ms for detached operations and 24.86 ms for individual World publications. Setup was outside the timed regions. These are local checks, not representative game workloads or #1004 capacity evidence.
- All 38 local documentation links across the changed guides resolve; whitespace and diff checks pass.

Schema/property lookup is a bounded scan of five components and at most seven properties. Editing validates the complete small component; duplicate detection is quadratic in the edit count (at most seven distinct supported fields). World publication uses existing transaction staging; it is an authoring/validation boundary, not a per-frame simulation interface. No production scene-capacity claim follows from these tests. Asset graph editing, material asset factor schemas, custom script components, the inspector, file codecs, and render extraction remain separate work.
