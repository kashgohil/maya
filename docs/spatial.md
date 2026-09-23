# Transform hierarchy and camera data

[#992](https://work.rezee.app/kash/issues/992) adds spatial operations to the CPU-only [World](world.md). Public entry points are [world.hpp](../include/maya/world/world.hpp), [commands.hpp](../include/maya/world/commands.hpp), and [spatial.hpp](../include/maya/world/spatial.hpp). No camera calculation needs a device, window, input singleton, or editor.

## Author and edit

Entities start without components. Both ends of a parenting relationship must have a `TransformComponent`; an unparented transform is a root. Hierarchy links and world matrices are World-owned derived storage, not editable components. Transform queries always expose **const** references, including queries on a mutable World. Other component types retain #991's scoped mutable access.

```cpp
maya::World world;
auto commands = world.commands();
const auto rig = commands.create();
const auto eye = commands.create();
commands.add(rig, maya::TransformComponent{{10, 0, 0}});
commands.add(eye, maya::TransformComponent{{0, 2, 5}});
commands.add(eye, maya::CameraComponent{});
commands.reparent(eye, rig, maya::ReparentPolicy::keep_local);
const auto result = world.commit(commands);
if (!result) return; // report error and command_index

const auto camera_entity = result.created[eye.index];
auto edit = world.commands();
edit.set_transform(result.created[rig.index], {{20, 0, 0}});
const auto edited = world.commit(edit);
if (!edited) return;
const auto matrices = world.camera(camera_entity, 16.0f / 9.0f);
if (matrices) {
    // Copy view, projection and view_projection into the view's rendering data.
}
```

All spatial commands share WorldCommands' enqueue order, scoped-borrow exclusion, allocation preparation, and atomic publication. A pending target must have been created earlier in the same buffer. A failed operation publishes none of the batch, including prior component changes. `set_transform` requires an existing transform and replaces its entire local TRS. It can be queued during a query but committed only after the borrow ends.

`parent(entity)` returns an optional handle. A root, invalid entity, or entity without a transform returns nullopt; use `alive`/`has` when the distinction matters. `children(entity)` returns a copy of direct child handles; invalid entities return an empty list. Child order is unspecified and must not become serialized/editor ordering. Handles are World-lifetime checked; relationships are internal slot links, never references to another World.

## Parenting and destruction

- `keep_local` preserves local TRS and lets world placement change.
- `keep_world` uses the pose at that point in the command sequence, calculates inverse(new parent world) × old world, and decomposes it to positive-scale TRS. Later commands still apply normally. Required local shear, reflection, numerical degeneracy, or an unrepresentable inverse returns `unrepresentable_transform` without modifying the World. Selecting the current parent is a no-op.
- `nullopt` as parent detaches to the root under the selected policy. Self-parenting, descendant cycles, stale/foreign targets, and missing transforms fail explicitly.
- Destroying a parent destroys its complete subtree, releases all component-owned resources, removes persistent-ID resolution, and invalidates runtime handles. Detach/reparent a child **before** the destruction command to preserve it. Referring to a deleted descendant later in the batch rejects the entire batch.
- Removing a transform while it has a parent or children returns `hierarchy_in_use`. Detach children and the entity first, or destroy the subtree. Remove/add replacement is allowed after these relationships are cleared.

## Coordinates, validation, and precision

The [runtime contracts](architecture/runtime-world-contracts.md) remain authoritative: metres, radians, right-handed +X right/+Y up/-Z forward, column-major matrices and column vectors. Local = T × R × S; world = parent world × local. Full affine world matrices retain inherited shear, with no per-frame TRS decomposition.

Adding or setting a transform validates finite translation/rotation/scale, strictly positive scale, and a representable nonsingular local matrix/inverse. Finite nonzero quaternions are normalized using double intermediates, including very small and very large magnitudes; zero/nonfinite quaternions fail. No value is clamped into range. Authored invalid data returns `invalid_transform`.

`world_matrix(entity)` returns a matrix copy or nullopt for a missing/invalid transform or a derived pose whose matrix/inverse cannot be represented. Finite local values can still overflow or become numerically degenerate when composed through a hierarchy. Such a derived pose is cached as invalid and propagates invalidity to descendants; a subsequent local edit can restore it. Rendering/extraction must skip/report invalid poses, never reuse a previous matrix. Keep-world reparenting rejects an invalid required pose. There is no approved maximum world extent or promise that arbitrary finite float inputs compose successfully.

Named numerical rules in `spatial.hpp`:

| Rule | Value / meaning |
| --- | --- |
| `spatial_tolerance` | 1e-5: dimensionless bound on pairwise normalized basis dot products; per-column TRS reconstruction error relative to column length; absolute deviation of a scale from one for a camera pose. |
| `spatial_singularity_tolerance` | 1e-8: reject when abs(linear determinant) ≤ tolerance × product of basis-column lengths. This measures angular degeneracy independently of overall scale. |
| Float representability | Double intermediate results must fit finite float storage. An inverse outside that range is rejected. No absolute scale clamp is imposed. |

Spatial math expects affine matrices with an exact bottom row (0,0,0,1). `local_matrix` composes already-validated TRS; call `validated_transform` before using arbitrary external data. `inverse_affine`, `compose_affine`, and `decompose_transform` return optional results. Float storage still limits precision near extreme magnitudes. Small errors inside the named tolerances are accepted as rounding, not as support for authored camera scale/shear.

## Camera data and controller boundary

`CameraComponent` contains vertical FOV in radians and near/far distances; aspect belongs to each view. `camera_matrices(camera, rigid_world_pose, aspect)` is a pure calculation returning view, projection, and projection × view. `World::camera(entity, aspect)` gets the entity's pose and additionally checks every transform in its ancestry for unit scale. A scaled ancestor cannot be hidden by an inverse-scaled child.

Camera calculations reject nonfinite data, FOV outside (0, π), aspect ≤ 0, near ≤ 0, far ≤ near, scale/shear/reflection, or unrepresentable matrices. Camera field edits remain mutable pending #994; validation runs whenever camera matrices are requested. The view is the inverse rigid pose. Projection maps view-space -near to depth 0 and -far to 1; the camera looks along local -Z. A zero-size viewport should skip its camera/render calculation.

Input controllers are producers of local transform commands. They do not own camera matrices, capture a window, or change camera projection through these APIs. The old `maya::Camera` remains the legacy sample's degree-based free-flight input adapter until renderer/sample migration in #998; new World camera data is independent of it. Editor navigation, input-controller redesign, render extraction, normal inverse-transpose handling, and physics restrictions belong to their respective follow-ups.

## Storage and cost

World keeps parent/first-child/previous-sibling/next-sibling slot links and a derived-matrix cache per slot, plus reusable traversal scratch storage. Capacities follow the slot high-water mark and remain until World destruction. Transactions stage only touched nodes, required ancestors, and deleted subtrees; they do not clone every component or the whole World. Cycle checks and keep-world calculations walk ancestry, so constructing a chain in parent-first order can have quadratic total validation cost. This is an explicit current limit, not a production scene-loading benchmark.

Publishing transform/reparent edits marks affected descendants dirty. Already-dirty subtrees are skipped. Matrix queries evaluate dirty ancestors before descendants and reuse clean cached matrices; a clean query is constant time. Hierarchy queries, dirty marking, and destruction are iterative, so depth does not consume recursive call stack. Subtree deletion visits registered component pools for each deleted entity. World remains single-owner-thread; const matrix queries mutate the cache and are not concurrent read APIs.

## Verification

The World test target includes [spatial_tests.cpp](../tests/spatial_tests.cpp) and links only MayaWorld and Catch2. Tests cover nested rotation/nonuniform scale and shear, both parenting policies, transactional rejection, sibling relinks, stale/foreign/pending targets, transform replacement/removal, subtree RAII cleanup, normalization and tolerance boundaries, deep hierarchy deletion, numeric failure/recovery, camera depth/view calculation, and cancelling ancestor scales. A deterministic forest-edit test compares repeated updates/reparents with an independent position model.

Run `ctest --test-dir build -L world --output-on-failure` after building `maya_world_tests`. Sanitizer and benchmark results are recorded separately; correctness and CPU microbenchmarks do not establish shipping game capacity.

Validation on 23 September 2026: all application targets built in Release without diagnostics. All eight CPU/CLI CTest entries passed; the final World suite passed 27 cases / 147,640 assertions in Release and UBSan. Clang static analysis of world.cpp and spatial.cpp reported no findings. The test link contains only MayaWorld and Catch2. A bounded CPU benchmark exercised 1,000/10,000/100,000 entities through hierarchy creation, ten root edits with full matrix queries, cached queries, and subtree deletion; it is not the #1004 performance baseline.

The ASan/UBSan build succeeded, but the test executable timed out after 15 seconds before any output. A freshly compiled independent empty-main ASan/UBSan executable also timed out after 10 seconds. Both were terminated. The local sanitizer startup limitation persists; this is not an ASan pass.
