# Maya architecture contracts

Recorded 23 September 2026 for [ISSUE-990](https://work.rezee.app/kash/issues/990), under [DOC-58 — Production Game Engine Direction and Architecture](https://work.rezee.app/kash/docs/58) and the [author-save-run milestone](https://work.rezee.app/kash/issues/988).

These are implementation contracts for the next issues, ready for review. They describe intended behavior, not features already present. Benchmark profiles and numerical targets are explicitly proposals. A change to an identity, timing, or persistence contract must update these records and describe its compatibility impact before dependent code changes.

| Record | Decisions |
| --- | --- |
| [Runtime and world](runtime-world-contracts.md) | Dependencies, ownership, identity, coordinates, transforms, assets, and streaming boundaries. |
| [Scheduling and rendering](scheduling-contracts.md) | Clocks, mutation boundaries, scripts/physics/animation order, transform authority, and immutable rendering input. |
| [Performance baseline](performance-baseline.md) | Proposed hardware and repeatable workloads, measurement protocol, and budget decisions. |

## Current implementation and migration

The source audit below is against commit `0b5eb8e`. Subsequent implementation is recorded separately: [#991 World storage](../world.md) adds World, typed asset-reference values, and the MayaWorld target; [#992 spatial operations](../spatial.md) adds validated local transforms, hierarchy transactions, derived world matrices, and independent camera calculations. RenderSnapshot and the other future services remain contracts rather than existing APIs.

| Evidence in the repository | Current behavior | Next implementation boundary |
| --- | --- | --- |
| [CMakeLists.txt](../../CMakeLists.txt), [Engine](../../src/maya/core/engine.cpp) | Runtime/editor/player/sample split; application destroyed before device shutdown. | Preserve #989 ownership and dependency rules. Logical runtime modules below can initially share MayaRuntime. |
| [Scene](../../include/maya/core/scene.hpp), [Material](../../include/maya/core/material.hpp) | Flat drawable list, owning mesh vector, raw mesh/texture references. | #991 world/identity, #993 shared assets, #994 properties, #995 persistence. |
| [Matrix](../../include/maya/math/matrix.hpp), [Quaternion](../../include/maya/math/quaternion.hpp), [Camera](../../src/maya/core/camera.cpp) | Column vectors, right-handed transforms, zero-to-one depth. Camera mixes data/input and accepts degrees. | #992 separates camera data/controller and validates transform/camera inputs. Convert the legacy degree API at the boundary. |
| [Desktop host](../../src/maya/platform/desktop_application.cpp), [Application](../../include/maya/core/application.hpp) | One update per render; smoke mode supplies 1/60 seconds. | A fixed-step scheduler and script/physics hooks are later simulation work. Smoke mode is not that scheduler. |
| [Scene rendering](../../src/maya/core/scene.cpp), [shader](../../resources/shaders/metal/triangle.metal) | Direct draws reuse one uniform region; normals use the model's upper 3x3. | #996 explicit RHI, #997 independent draw/frame data, #998 extraction and correct normal transforms. Nonuniform-scale lighting is not yet correct. |
| [RHI handles](../../include/maya/rhi/resource.hpp), [Metal device](../../src/maya/rhi/metal/metal_device.mm) | Integer resource IDs and device-wide cleanup; no individual retirement API. | #996/#997 validated resource lifetime and GPU completion tracking. |

Storage layout, an ECS library, a physics library, a scripting language, an editor UI library, and a general job system are not selected by this issue. In particular, validated handles and component access rules must survive whichever storage strategy #991 selects.

## Scenario review

This is a design walkthrough, not a claim that the future systems passed integration tests. Each row traces a trigger through the contracts and names the follow-up evidence required.

| Scenario | Required outcome under these contracts | Implementation evidence to add |
| --- | --- | --- |
| Editor saves, enters play, deletes an entity, then stops | Clone authored data into a new World instance; preserve persistent IDs but remap runtime handles. Deletion in play cannot resolve into or mutate the authoring World. Stop discards play state. | #991 cross-world/stale-handle tests; #995 ID round trip; #1003 repeated play/reset. |
| Player opens the same scene without editor libraries | The shared loader resolves the same IDs and properties. Input and simulation use the same schedule; no editor selection/undo data is required to run. | #995 atomic load failure; #1003 player load; retain #989 independent build checks. |
| A collision callback deletes a body while another system iterates | Callback queues destruction for the next tick boundary. The current step/event buffer completes; destruction invalidates its handle before storage reuse. | Future physics integration: deletion in contact callbacks, deferred commands, stale event recipients. |
| A script and physics both try to move a dynamic body | Script supplies force, impulse, or an explicit teleport command. Physics remains the pose writer; interpolated render poses never feed back into it. | Future simulation: ownership validation, teleport history reset, stable pause/step. |
| A cell unloads while another cell references it and an async load completes late | References become unresolved without pinning the cell. Old handles remain invalid on reload. A mismatched request generation discards the completion. | Future streaming: cancellation/unload/reload stress; #991 handle reuse; #993 resource leases. |
| A mesh reloads or its last entity is deleted while a submitted frame uses it | New extraction acquires the new resource version. Existing snapshots/submissions retain the old allocation until CPU encoding and GPU use finish. | #993 sharing/reload tests, #996/#997 delayed-completion retirement tests. |
| An editor frame stalls, the viewport disappears, or capture runs slower than real time | Interactive catch-up is bounded and lost wall time is reported. Simulation does not depend on drawable acquisition. Capture advances exact requested time without dropping simulation ticks. | Future scheduler/capture: clock traces, overload, zero-size view, pause/resume tests. |
| A parent has rotated nonuniform scale and a child is reparented | World affine matrices preserve inherited shear. A keep-world reparent requiring unrepresentable local shear is rejected atomically. Physics rejects unsupported ancestry. | #992 hierarchy/reparent tests; #998 normal transform and winding checks. |

## Open product decisions

These remain open rather than being resolved by convenient defaults in the prototype.

| Decision | Evidence needed / decision point |
| --- | --- |
| Shipping platforms and minimum hardware | Select actual representative game content and supported device matrix before promising a minimum spec. macOS/Metal is the implementation base only. |
| Required world extent and precision | Measure camera, picking, contact, and joint error at increasing distances. Decide double precision, cell-relative coordinates, or origin rebasing before the streaming format and physics integration are fixed. |
| Frame, memory, load, and iteration budgets | Run the proposed workloads through #1004; allocate budgets with headroom on named hardware. No production numbers have been approved. |
| Game genres, content density, and physics workload | Choose a representative game slice, active/sleeping body mix, query/constraint count, and traversal speed before scale acceptance. |
| Simulation frequency and overload policy for shipping | Validate the proposed 60 Hz profile against physics stability, input latency, and CPU cost. Multiplayer/rollback requirements may alter timing and determinism needs. |
| Cinematic capture scope and repeatability | Define output format/color, shutter sampling, frame rates, and replay/cache fidelity before promising reproducible exports. |

## Validation of this record

- All four #990 acceptance areas are covered by the three linked records and the scenario review above.
- Existing source and tests support the retained matrix and session conventions; proposed world, scheduling, asset, and streaming behavior is explicitly separated from current code.
- Performance numbers below are workload inputs or candidate targets, with no benchmark result inferred from smoke tests.
- Validate repository links and run the existing matrix/quaternion/engine tests when reviewing compatibility. Runtime implementation and new tests belong to the follow-up issues.

Validation on 23 September 2026: `./build/maya_tests '[matrix],[quaternion],[engine]'` passed **28 test cases / 322 assertions**. All **20 local Markdown links** across these records and their README/GEMINI entry points resolve. This verifies retained conventions and documentation references; it is not execution evidence for the proposed subsystems or workloads.
