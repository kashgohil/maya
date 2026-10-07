# Skeletal animation

Imported glTF models play their animation clips and deform their skinned meshes (#1038). Clips are sampled on the CPU in the fixed tick, which writes joints' transforms. Meshes are skinned on the GPU, four joints per vertex. The choices come from the [rendering and content decision](architecture/rendering-content-decision.md) (#1030).

Joints are **entities**. An imported model's joints are the entities its nodes import as, so a joint can carry children (a sword in a hand), be selected and inspected, and is shown by the World like any entity. A clip moves joints by writing their `TransformComponent`s through the tick's commands. Interpolation between ticks, recording and replay, and Stop's return to the authored scene therefore work as for any other moving entity.

## Skins and clips

Two asset kinds ([animation.hpp](../include/maya/assets/animation.hpp)) hold what glTF calls skins and animations:

- **`SkinAsset`**: the joints a skinned mesh's vertices name, as [name paths](#binding), and each joint's inverse bind matrix (from the mesh's space to the joint's, in the bind pose).
- **`AnimationAsset`**: a clip's name, its duration (its last key), and its channels. A channel names its target by path and holds one property (translation, rotation, or scale), its sampler (step, linear, or cubic spline), and its keys.

An import catalogs them as parts of the glTF file, as it does meshes and textures ([import](import.md#catalog-entries)):

```text
skin 6d617961 1a2f "models/fox.glb#skin/0"
animation 6d617961 1a30 "models/fox.glb#animation/1"
```

The import file names them by the file's names, so reimporting keeps their IDs. A model's skinned mesh entities get a `maya.skin`, and its root a `maya.animation` that plays its first clip, looping. The others are in the catalog to choose from. Morph-target weight channels are not read; they are left out with a warning.

Cooking reads a skin or clip from its glTF file, cached in the [cook cache](assets.md#cook-cache) (`skin_cook_version`, `animation_cook_version`). Packages hold them cooked (`.cooked-skin`, `.cooked-animation`), and the packaged player reads only those ([packages](projects.md#packages)). Imported meshes cook their joints and weights too (`imported_mesh_cook_version` 2): a second vertex stream of four joint indices and four weights per vertex (`SkinVertex`, [vertex.hpp](../include/maya/rhi/vertex.hpp)), weights scaled to sum to 1.

## Samplers

glTF's three samplers are implemented directly (`sample_channel`), without an animation library:

- **Step** holds each key until the next.
- **Linear** interpolates translations and scales, and slerps rotations along the shorter arc.
- **Cubic spline** follows glTF's Hermite curve, with each key's in- and out-tangents scaled by the time to the neighbouring key.

Every sampler holds its first key before it and its last key after it. Rotations come out normalized. A channel whose keys are out of order, whose value count does not match its keys, or whose values are not finite is refused when the file is read (`check_channel`), and the import reports it with its JSON path.

## Binding

Skins and clips name joints by **name path**: the names of the entities from a root down to the joint, joined with `/`, such as `Armature/Hips/Spine` ([name_path.hpp](../include/maya/world/name_path.hpp)). A `/` or `\` in a name is escaped with `\`. When several siblings share a name, the first in child order matches. An import names nodes uniquely among their siblings (a node's name, else its mesh's, else `Node <index>`, with ` 2`, ` 3`, ... for repeats), so its paths always resolve.

- A **clip's** paths start below the entity whose `maya.animation` plays it: an imported model's root.
- A **skin's** paths start below the nearest of the skinned entity's ancestors (or the entity itself) where every joint resolves: for an import, its root.

Binding by name makes a clip work for any model that names its joints the same way, wherever it is in the scene. **Renaming or moving a joint unbinds it**, and this is reported:

- In play, a clip's channel whose target is missing is reported once and skipped; the rest of the clip plays (`'Fox' has no entity at 'root/_rootJoint/b_Root_00', which its clip 'Run' moves (renamed or removed?); those channels are skipped`).
- A skin whose joints do not all resolve draws its mesh unskinned, as authored, with the `unbound_skin` [render diagnostic](renderer.md#render-snapshots). The editor shows it in Diagnostics.

Bindings are resolved again only when the World's `names_revision()` changes. It counts commits that create, destroy, or reparent entities, or that add, replace, or remove a `NameComponent`, so a clip's own transform writes never cause a rebind. The animation system keeps its clips' bindings this way, and extraction keeps skins' in a `SkinBindingCache` its caller holds between frames (`RenderExtractOptions::skins`; the player, the editor, and the benchmark keep one). Without one, every extraction searches for the joints again, which for 100 CesiumMen costs about 3 ms a frame.

## Components

| Component | Property | |
| --- | --- | --- |
| `maya.skin` (13) | `skin` | The skin asset whose joints deform this entity's mesh. |
| `maya.animation` (14) | `clip` | The clip it plays on the entities below it. |
| | `playing` | Advances the clip each tick; otherwise its current pose is held. Default on. |
| | `loop` | Starts again from the clip's other end; otherwise it holds its last pose. Default on. |
| | `speed` | Clip seconds per second of play, −100 to 100; negative plays backwards. Default 1. |
| | `start` | Seconds into the clip where play begins, and begins again when the clip or this changes. |

Both are edited in the Inspector ([editor](editor.md#animation)), saved in scenes like any component ([scene](scene.md)), and validated against the catalog: a skin reference must name a skin, and a clip reference a clip.

## Playing

The **animation system** (`animation_system`, [simulation/animation.hpp](../include/maya/simulation/animation.hpp)) is the last of `play_systems`: fly control, spin, scripts, then animation. Each tick it reads every `maya.animation` as the tick began, in EntityId order. For each one, it samples the clip at its time and writes the transforms of the entities its channels name. Then a playing clip's time advances by the tick times its speed. Time accumulates in double precision, so a clip's end falls on the tick its length and speed say.

- **The first tick** poses the clip's start.
- **A script's change shows one tick later.** The system reads components as the tick began, and a script's writes commit at the tick's end. So a clip set by a script in tick *n* is first sampled in tick *n + 1*. The #1030 record expected the same tick. This was changed for #1038 so that animation reads the same committed state as every system; replays repeat it exactly either way.
- **A new clip or start time** starts again from `start`, and its joints [jump](#presentation) there.
- **What a clip does not name keeps its pose**, including what an earlier clip left. A clip's channel sets only its own property (translation, rotation, or scale).
- **A zero scale**, which glTF uses to hide parts, is held at 10⁻⁶ (`min_animated_scale`): the World keeps only transforms it can invert, and what it scales is too small to see. A negative scale cannot be shown; the entity keeps its last pose, and that is reported once.
- **A clip that cannot be loaded** is reported once, and its entity does not move.
- Joints may carry static colliders, which follow them. Moving bodies are always roots ([physics](physics.md)), and a joint is below its animated entity, so a clip never moves a body that physics owns.

The editor and the player run the same systems on the same scene, so **a clip plays identically in both**, tick for tick. The poses feed `PlaySession::state_hash`, so a recorded session that changes clips replays exactly ([recording and replay](play.md#recording-and-replay)). **Stop** drops the play World, so animated entities return to their authored poses with the rest of the scene. Outside play, joints show their authored poses: the import's node transforms.

## Presentation

Animated joints are shown between ticks like any moving entity ([between ticks](play.md#between-ticks)), so motion is smooth at any display rate. At 120 Hz each frame shows half a tick more of the clip. When a clip changes or starts again, the system lists the joints it moved in `TickContext::jumps`, and presentation shows them (and what they carry) at their new pose rather than between the old pose and the new one, as it does for teleports.

## Skinning

A skinned instance's vertices are placed in world space by its joints: each vertex's position is the sum of its four joints' skin matrices (joint world matrix × inverse bind matrix), weighted, applied to its bind-pose position. As glTF says, the skinned mesh entity's own transform is not applied. Normals use the blended matrix's cofactors.

- **Extraction** binds each `maya.skin` entity's skin and computes the skin matrices from the joints' *shown* poses, so skinning follows presentation. Instances of one skin under one ancestor (an imported mesh's primitives) share them. `RenderSnapshot::joints` holds them all, and each instance names its range (`first_joint`, `joint_count`).
- **The renderer** uploads the frame's joints once (buffer 6, `float4x4` each), read by every pass. Each instance's `DrawConstants::skin` holds its range. Meshes with joint weights draw with the skinned pipelines (`litSkinnedVertex`, `shadowSkinnedVertex`); an instance whose skin did not bind uses its own transform there. Joint indices past the skin are clamped to its last joint, so a mesh and skin that disagree cannot read outside the palette.
- **Culling** uses a sphere around the mesh's bounds carried by every joint's skin matrix. This encloses every skinned vertex, since each is a weighted average of those placements. It is loose (for CesiumMan about 2 m across a 1.5 m figure), but skinned meshes still cull per pass like any other.

Picking uses the mesh's authored geometry at its entity's transform, not the skinned shape.

## Debug view

The **skeleton** view ([skeleton_debug](../include/maya/renderer/render_snapshot.hpp)) draws, for every skin that binds, a line from each joint to its parent joint and each joint's axes (X red, Y green, Z blue), a third of the mean bone long. Joints sit inside their mesh, so these are **x-ray lines** (`DebugDraw::xray_lines`): as bright behind surfaces as in front. Turn it on in the editor's debug menu (eye icon → Animation → Skeletons, kept as a preference), or start the player with `--debug-skeletons`.

## Scripts

Scripts set a `maya.animation`'s properties like any component's ([scripting](scripting.md#what-scripts-may-change)):

```lua
self.entity:set("maya.animation", "speed", 2)
self.entity:set("maya.animation", "clip", walk) -- an asset ID, as get returns it ("6d617961 1a30"), or nil
```

A clip is read as its asset ID's text, and set from one. The ID is checked against the project's catalog (`ScriptSettings::assets`), so a script cannot set a mesh where a clip belongs. A script can keep clips' IDs in its `string` properties.

## Testing

`tests/animation_tests.cpp` checks the samplers (step, linear with the shorter arc, cubic with scaled tangents, held ends, refused channels), clip time, cooked skins and clips, name paths, and the animation system: the first tick, speed, looping, backwards play, start times, step clips, a script's change showing the next tick, scripts reading and setting clips, smooth presentation at 120 Hz with clip changes jumping, renamed joints, zero and negative scales, missing clips, static colliders on joints, and exact replay.

`tests/animation_samples_tests.cpp` uses Khronos's SimpleSkin, RiggedSimple, RiggedFigure, CesiumMan, Fox, RecursiveSkeletons, and InterpolationTest (from `tools/fetch_render_samples.sh`). It imports each, plays every clip held at six times, and compares the results with an **independent evaluator**. The evaluator reads the file with cgltf alone and computes the specification's samplers, node hierarchy, and joint matrices in double precision. Every node's entity must match its world matrix, and every skinned instance's joints must match a skin's, within float precision of the matrices multiplied. The file also checks the import's catalog parts, components, and stable IDs, and checks skinning in the renderer: the skinned pipelines in the lit and shadow passes, the palette, bounds that hold every CPU-skinned vertex, and an unbound skin's report.

`tests/animation_gpu_tests.cpp` renders CesiumMan and Fox at chosen times of their clips on Metal, and with the skeleton view, against approved references in `tests/references/animation`. `tests/editor_animation_tests.cpp` plays an imported model in the editor and checks it against the player's path, Stop's return to the authored poses, the Inspector fields and Assets panel, and the skeleton view. Packaging is checked in `package_tests.cpp`: a packaged clip plays as the project's does, tick for tick.

## Cost

Measured by the `animation` benchmark workload ([performance](performance.md#animation)): `benchmarks/a1_animation.benchmark` plays 100 CesiumMen (19 joints each, 1,900 joints a tick) under a sun casting four cascades. `a1_unskinned.benchmark` is the same scene drawn unskinned, so the difference per pass is skinning's cost. In a Release build on an Apple M4 Pro, 1920×1080:

| Per frame (means of two invocations of three runs, 3,000 frames each) | Skinned | Unskinned | Skinning's share |
| --- | --- | --- | --- |
| Animation system, per tick (P99) | 0.197 ms (0.22 ms) | 0.226 ms (0.25 ms) | — (both play every clip) |
| Extraction | 0.143 ms (P99 0.18 ms) | 0.009 ms | 0.134 ms: skin matrices for 1,900 joints, and bounds |
| Encoding | 0.024 ms | 0.027 ms | none |
| GPU `view` pass | 0.53 ms | 0.50 ms | 0.025 ms |
| GPU `sun shadows` pass (4 cascades) | 0.84 ms | 0.78 ms | 0.065 ms |
| Frame, CPU | 0.91 ms | 0.84 ms | 0.07 ms |

- **Sampling** is about 2 µs per character a tick: 57 channels and 19 joint writes each. Joints that hold a pose (a paused or ended clip) are not written again.
- **Finding joints** is kept between ticks and frames ([binding](#binding)). Before extraction kept it (`SkinBindingCache`), searching for the joints by name every frame cost 2.98 ms here.
- **Skinning on the GPU** adds about 7% to the passes that draw these 2 million triangles a frame (the view and four cascades), far below the 3.3 ms CPU skinning would cost ([decision](architecture/rendering-content-decision.md#animation-sample-on-the-cpu-skin-on-the-gpu)).
- **Nothing else changed.** I1 at 10,000 and 100,000 instances, alternating with a Release build of the commit before #1038 (`07ae5c6`): extraction 0.51 and 5.0 ms in both, encoding +0.01 ms (draw constants grew from 112 to 128 bytes), GPU time unchanged within its spread.

These are observations, not budgets.

## Limits

- One clip per animated entity, with no blending or cross-fading between clips: a change jumps.
- Morph targets (blend shapes) and `KHR_animation_pointer` are not read.
- Four joints per vertex (`JOINTS_0` and `WEIGHTS_0`); further sets are not read.
- Skinned bounds are loose, and picking does not follow the skinned shape.
