# Renderer: extraction, views, and offscreen targets

[Issue #998](https://work.rezee.app/kash/issues/998) adds `MayaRenderer` (`Maya::Renderer`). It links MayaWorld, MayaAssets, and MayaRHI, and MayaRuntime links it publicly. It replaces the legacy `Scene` and `Material` types, which issued draws from their own object list. A World now only describes what exists. The renderer reads it once per frame into an immutable snapshot, then turns camera views of that snapshot into textures.

```text
World ──extract_render_snapshot──▶ RenderSnapshot ─┐
      ──extract_render_view / make_render_view──▶ RenderView ─┼─▶ Renderer::render ─▶ RenderTarget (color + depth)
                                                     │                                      │
                                                     └──────────── Renderer::present ◀──────┘──▶ window surface or editor panel
```

## Rendering a World

```cpp
#include "maya/renderer/renderer.hpp"

// Once per session: the shader source and the owners of GPU state.
auto renderer = maya::Renderer(device, maya::FileSystem::read_text("resources/shaders/metal/renderer.metal"));
auto view_target = maya::RenderTarget(device, {maya::Format::rgba8_unorm, false, "player view"});

// Each frame, after World updates and inside begin_frame/end_frame:
const auto surface = device.acquire_surface();
if (!surface) return; // hidden or minimized: skip the view, not the simulation
const auto [width, height] = std::pair{surface.target.width, surface.target.height};
if (auto error = view_target.resize(width, height)) { /* report */ }
const auto view = maya::extract_render_view(world, camera_entity, width, height);
const auto snapshot = maya::extract_render_snapshot(world, assets);
if (auto error = renderer.render(snapshot, *view, view_target)) { /* report */ }
if (auto error = renderer.present(view_target, surface.target.texture, {0, 0, width, height})) { /* report */ }
```

The [player](../apps/player/player_application.cpp) uses exactly this path: it renders its play World's first camera entity at the window's size (see [play](play.md#the-player)). The [editor](editor.md) renders the same kind of World with its own camera, which is tool state rather than an entity, into a target sized to its viewport panel. Its UI then draws that target as an image instead of calling `present`.

## Render snapshots

`extract_render_snapshot(world, registry, options)` reads the World without writing to it and returns a `RenderSnapshot`:

| Field | Contents |
| --- | --- |
| `world` | The source `World::token()`, so snapshots from authoring and play Worlds stay distinguishable. |
| `meshes` | One `AssetLease<MeshAsset>` per distinct mesh asset. Every instance of a mesh shares its lease and GPU buffers. |
| `instances` | Entity ID, mesh index, world matrix, normal matrix, and copied material factors for each drawable entity. |
| `lights` | Enabled directional lights in EntityId order, at most `max_directional_lights` (4). |
| `ambient` | Linear ambient color, from `RenderExtractOptions`. |
| `diagnostics`, `stats` | Why entities were skipped or substituted (capped at 64), with counts that are never capped. |

A snapshot holds no World handles and no pointers into component storage. Its entity IDs are for picking and diagnostics; resolve them in a World again before use. The snapshot owns leases on the exact mesh versions it draws. Deleting entities, evicting or reloading assets, or destroying the registry or the World after extraction does not change the snapshot. Those changes appear in the next extraction. Release the snapshot once its frames are encoded. The graphics device keeps the native buffers until the GPU completes those frames ([deferred retirement](rhi.md)). Material factors are copied by value, so the snapshot does not keep material leases.

Extraction acquires assets through the registry. Assets that are not resident load synchronously at that point, so a scene's first extraction pays for its loads. Each asset is acquired once per extraction, so every instance draws the same version. Failed assets are not retried on later extractions; call `AssetRegistry::reload` after fixing the source.

| Situation | Result |
| --- | --- |
| `visible` is false, or no mesh is assigned | Not drawn; counted in `stats.hidden`. No diagnostic. |
| Mesh missing, failed, not in the catalog, or from an ended device session | Draw skipped; `missing_mesh` names the entity, the asset, and the registry's reason. |
| Material missing or failed | Drawn with `fallback_material()` (magenta); `missing_material`. |
| No material assigned | Drawn with default `MaterialAsset` factors (white, not metallic, fully rough). |
| No transform, or a degenerate world matrix | Draw skipped; `missing_transform` or `invalid_transform`. |
| Point or spot light | Ignored with `unsupported_light`; only directional lights are rendered so far. |
| More than four directional lights | The four with the lowest EntityIds are used; the rest report `light_limit`. |

The sample logs diagnostics only when their number changes, not every frame.

## Views and targets

A `RenderView` is a camera description in framebuffer pixels: size, `CameraMatrices`, camera position, and clear color. `extract_render_view(world, camera, width, height)` builds one from a camera entity through `World::camera`. `make_render_view(camera, pose, width, height)` builds one from camera data and a rigid pose, for cameras that are not entities. Both take the aspect ratio from the view's own size and return `nullopt` for a zero size or an invalid camera ([camera rules](spatial.md)). Several views can render the same snapshot in one frame, each into its own target. They do not need separate Worlds or separate extraction.

**Poses between ticks** (#1016). `RenderExtractOptions::poses` and the last argument of `extract_render_view` take a `PresentationPoses`: world matrices shown in place of the World's for some entities, such as a play session's between ticks ([play](play.md#between-ticks)). Entities not in it draw at the World's pose; without it, extraction is as before. The player and the editor's Play pass `PlaySession::presentation()`.

`RenderTarget` owns one view's color texture (render target, sampled, and optionally readback) and its `depth32_float` depth texture. Nothing is allocated until `resize`. Resizing to the current size does nothing, so steady frames never reallocate; `allocations()` counts real allocations. A new size creates both textures before destroying the old ones. The device retires the old ones after their frames complete, and a failed resize keeps the previous textures. Zero sizes are rejected; skip the view instead. After a device session ends, `valid()` is false until the next `resize`. A view's size is independent of any window: offscreen captures, thumbnails, and editor panels choose their own sizes.

## Passes

`Renderer::render(snapshot, view, target)` runs inside a frame with no pass open. It uploads one `ViewConstants` block, opens a pass that clears the target's color to the view's clear color and its depth to 1, and binds the lit pipeline. Then, for each instance, it uploads that instance's `DrawConstants` to its own slice of frame upload memory and draws the shared mesh. It checks that the view size matches the target, that the target is live, and that every instance refers to a mesh the snapshot holds. It returns the first device error, such as exhausted upload memory. The remaining instances are skipped, the pass is still closed, and the frame can still end.

`Renderer::present(target, destination, area, background)` opens a pass on `destination`, clears it to `background`, and draws the target's color texture scaled into `area`. The area is a `PixelRect` in the destination's pixels, with its origin at the top left. The destination is usually the acquired window surface, but any render-target texture works. The player presents to the whole surface; the same call can present into any rectangle, such as a panel. When the area and the view have the same size, presentation copies the view's pixels exactly (bilinear sampling at texel centers).

Pipelines are created on first use for each target format (lit, with `depth32_float`, back-face culling, and counter-clockwise front faces; and the two debug pipelines, only once a snapshot has debug lines) and each destination format (present). A shader compile failure is cached and returned without recompiling every frame. When the device starts a new session, the renderer drops its old handles and recreates what it needs. `stats()` counts views, draws, presents, and debug draws, outlines, and lines.

## Debug lines

[Issue #1022](https://work.rezee.app/kash/issues/1022) adds a pass for debug lines and outlines, used by the [physics debug views](physics.md#debug-views). It is an ordinary part of `render`, so it works in any view: the editor's Scene and Game views, the player, and offscreen targets.

- **Data.** `DebugDraw` ([debug_draw.hpp](../include/maya/world/debug_draw.hpp), in `MayaWorld`) holds world-space `lines` (from, to, color) and `shapes`: box, sphere, and capsule outlines, each a world matrix, a size, and a color. A capsule's matrix is rigid and its size carries the radius and the half height, so its caps stay round. Helpers add crosses, arrows, and each outline. Colors are RGBA as the view stores them, with alpha blending.
- **Extraction.** `RenderExtractOptions::debug` is copied into `RenderSnapshot::debug`, so the snapshot stays self-contained. Without it the snapshot's `DebugDraw` is empty.
- **Off costs nothing.** An empty `DebugDraw` creates no pipelines, uploads nothing, and draws nothing.
- **Drawing.** Lines are uploaded once, and outlines once per kind. Each outline is an instance of a unit wire template generated in the vertex shader: a box's 12 edges, a sphere's three great circles, and a capsule's two rings, four sides, and arcs over each cap. Every segment becomes a screen-space quad `RenderView::debug_line_width` pixels wide (default 1.5; the editor doubles it at 2× scale), with a pixel of soft edge, clipped at the near plane.
- **In front and behind.** Each batch is drawn twice against the scene's depth, without writing it: at full opacity where it is in front (`less_equal`), then at 30% where the scene hides it (`greater`). Lines are pulled a thousandth of their distance toward the camera, so outlines lying on surfaces win.
- **Level of detail.** Sphere and capsule circles have 8, 16, or 32 segments, chosen from the outline's radius on screen (under 6 pixels, under 20, or more: `debug_segments_for`), so segments stay a few pixels long. The pass is bound by vertex work, so distant outlines cost little.
- **Layout.** `DebugConstants` (viewport size, line width, opacity, kind, and segments) is in [shader_constants.hpp](../include/maya/renderer/shader_constants.hpp) with the other layouts.

Cost, Release on the M4 Pro reference machine (thermal state nominal), at 1920 × 1080 with 3-pixel lines: boxes, spheres, and capsules in equal numbers, seen from about 100 m (`maya_editor_tests "Physics debug pass cost*"`, hidden):

| Colliders | GPU time added by the pass | Encoding added |
| --- | --- | --- |
| 1,000 | 0.38–0.46 ms | < 0.01 ms |
| 10,000 | 1.6 ms | 0.13 ms |

The GPU times are the device's own timestamps for the frame, with and without the pass. Before level of detail, every circle had 32 segments and the same views cost 1.75 ms and 7.6 ms. These are observations, not budgets.

## Lighting and materials

[renderer.metal](../resources/shaders/metal/renderer.metal) keeps the initial Blinn-Phong model, but every input is now data:

| Input | Source |
| --- | --- |
| Camera | View-projection and position from the view. |
| Lights | Each directional light shines along its entity's local −Z; `direction_to_light` is its world +Z. Radiance is `color × intensity`. No exposure is applied yet, so intensity acts as a linear multiplier rather than lux. |
| Ambient | `RenderExtractOptions::ambient`, default (0.06, 0.07, 0.09). |
| Base color | `MaterialAsset::base_color` multiplied by the vertex color. |
| Metallic | Removes the diffuse term and tints the highlight: specular color = mix(0.04, base, metallic). |
| Roughness | Highlight exponent = 2 / roughness⁴ − 2, clamped to [1, 2048], with roughness clamped to [0.05, 1]. |

This is not physically based shading. HDR, exposure, tone mapping, shadows, environment lighting, and textures in materials are later work (DOC-58 milestone 3). `MaterialAsset` has no texture reference yet, so the lit shader samples no textures. The sample's old checkerboard texture was removed with the legacy `Material`.

Normals are transformed by the inverse transpose of the world matrix's linear part. Extraction computes it in double precision from the cofactor matrix and scales it to stay representable; the shader renormalizes. Normals therefore stay perpendicular to surfaces under nonuniform scale, including scale inherited through the hierarchy. Transforms have strictly positive scale, so no world matrix is a reflection and triangle winding never flips. Extraction rejects a nonpositive or degenerate determinant as `invalid_transform`.

The shader's `Vertex` uses `float3`, which occupies 16 bytes in Metal and matches the padded C++ [Vertex](../include/maya/rhi/vertex.hpp). The legacy shader used `packed_float3`, which put the normal at byte offset 12 instead of 16. As a result, every earlier lit draw read its normals from the wrong bytes. The layouts of `ViewConstants`, `DrawConstants`, and `PresentConstants` are defined in [shader_constants.hpp](../include/maya/renderer/shader_constants.hpp), with static assertions. Buffer index 0 holds vertices, 1 holds per-draw constants, and 2 holds per-view constants.

## Sample content

The [basic scene](../samples/basic_scene/assets/basic.scene) is an ordinary [scene file](scene.md): a camera, a directional sun, the pyramid, and one [cube mesh](../samples/basic_scene/assets/cube.obj) drawn three times (a red cube, a blue metal cube, and a ground slab scaled 6 × 0.1 × 6). Four [material files](../samples/basic_scene/assets/materials) supply their factors. The sample's fly controller writes the camera entity's transform and animates the pyramid through World commands. The renderer only reads the result. The pyramid's side normals were also corrected; three of its four side faces had been using another face's normal.

## Tests

- [renderer_tests.cpp](../tests/renderer_tests.cpp) (`maya_renderer_tests`, labels `cpu;renderer`) uses a null device that mirrors buffer contents and records the constants bound at every draw. It covers mesh sharing, copied transforms and materials, and each missing-asset rule. It checks normal matrices under nonuniform scale in a hierarchy and light selection and limits. It checks that snapshot ownership survives entity deletion, eviction, and registry/World destruction, with deferred retirement. It also covers two views of one snapshot, target reallocation and retirement, upload exhaustion, presentation rectangles, and pipeline recreation across sessions.
- [renderer_gpu_tests.cpp](../tests/renderer_gpu_tests.cpp) (in `maya_tests`, tag `[rhi]`, run under Metal API validation) reads pixels back. It checks per-instance colors from one shared mesh and a skipped missing mesh. It checks diffuse lighting of a slanted quad scaled 1 × 1 × 4, which only the inverse-transpose normal passes. It checks identical output presented into a player-sized window and an editor viewport rectangle, rendering at six sizes with target reuse and retirement, and ten rounds of deleting the drawn entity and evicting its mesh while its frame is still in flight.
- [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) drives the real player and editor applications through window resizes, including a zero-sized one.
- **Debug lines** (#1022), in renderer_tests.cpp: an empty `DebugDraw` creates no pipelines and draws nothing; lines, boxes, and capsules are uploaded once per kind, with their matrices, sizes, and colors, and drawn in front and then behind at their opacities; outlines take 8, 16, or 32 segments by their size on screen; and the helpers make the lines they promise. Reference images of every physics debug category are compared on Metal ([physics](physics.md#debug-views)).

#998 validation on 24 September 2026:

- A fresh Release build of every target reports no diagnostics from Maya sources. All 18 CTest entries pass (13 CPU/CLI, 5 GPU/smoke), repeated three times. The GPU entries run with Metal API validation.
- The renderer CPU suite (11 cases / 311 assertions), the GPU renderer cases (5 / 370), and the desktop suite (5 / 804) pass. The GPU renderer cases passed 20 repeated Release runs under Metal API validation. All 18 CTest entries also pass under UBSan.
- Mutation checks: transforming normals by the model matrix in the shader fails the nonuniform-scale case; restoring the legacy `packed_float3` vertex layout fails all five GPU renderer cases. A transposed normal matrix, disabled mesh sharing, and a removed light limit each fail the CPU suite.
- Clang static analysis of the three renderer sources, the basic scene, and the editor application reports no findings. `maya_renderer_tests` links only MayaRenderer, MayaAssets, MayaWorld, MayaRHI, and Catch2: no Metal, window, or editor code.
- A Release CPU benchmark on the null backend, with every transform dirtied between frames, measured extraction at about 70 ns and encoding at about 30 ns per instance. 100,000 instances of 16 shared meshes took 6.8 ms to extract and 3.0 ms to encode. This measures CPU bookkeeping, not Metal encoding or GPU time, and it is not #1004 evidence.
- Rendered output was also inspected in images from the player camera and the editor camera.
- The ASan renderer executable builds but produced no output within 15 seconds and was terminated: the startup limitation recorded since #991. ASan is not recorded as passing.
