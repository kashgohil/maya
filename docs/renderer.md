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
| `textures` | One `AssetLease<TextureAsset>` per distinct texture the instances' materials use (since #1033). |
| `environment` | The scene's [environment](#environments), leased, with its intensity, rotation, and whether the sky is drawn (since #1035); empty when the ambient light is used. |
| `instances` | Entity ID, mesh index, world matrix, normal matrix, and a copied material, with its maps as texture indices, for each drawable entity. |
| `lights` | Enabled directional lights in EntityId order, at most `max_directional_lights` (4). |
| `ambient` | Linear ambient color, from `RenderExtractOptions`. |
| `diagnostics`, `stats` | Why entities were skipped or substituted (capped at 64), with counts that are never capped. |

A snapshot holds no World handles and no pointers into component storage. Its entity IDs are for picking and diagnostics; resolve them in a World again before use. The snapshot owns leases on the exact mesh and texture versions it draws. Deleting entities, evicting or reloading assets, or destroying the registry or the World after extraction does not change the snapshot. Those changes appear in the next extraction. Release the snapshot once its frames are encoded. The graphics device keeps the native buffers until the GPU completes those frames ([deferred retirement](rhi.md)). Materials are copied by value, so the snapshot does not keep material leases.

Extraction acquires assets through the registry. Assets that are not resident load synchronously at that point, so a scene's first extraction pays for its loads. Each asset is acquired once per extraction, so every instance draws the same version. Failed assets are not retried on later extractions; call `AssetRegistry::reload` after fixing the source.

| Situation | Result |
| --- | --- |
| `visible` is false, or no mesh is assigned | Not drawn; counted in `stats.hidden`. No diagnostic. |
| Mesh missing, failed, not in the catalog, or from an ended device session | Draw skipped; `missing_mesh` names the entity, the asset, and the registry's reason. |
| Material missing or failed | Drawn with `fallback_material()` (magenta); `missing_material`. |
| No material assigned | Drawn with default `MaterialAsset` factors (white, not metallic, fully rough). |
| A material's map missing, failed, or of the wrong role | Drawn with the placeholder in that slot; `missing_texture` or `texture_role`, once per material ([materials](#materials)). |
| No transform, or a degenerate world matrix | Draw skipped; `missing_transform` or `invalid_transform`. |
| Point or spot light | Ignored with `unsupported_light`; only directional lights are rendered so far. |
| Environment missing or failed | The ambient light lights the scene, with no sky; `missing_environment`. |
| More than one environment component | The one with the lowest EntityId is used; the others report `environment_limit`. |
| More than four directional lights | The four with the lowest EntityIds are used; the rest report `light_limit`. |

The sample logs diagnostics only when their number changes, not every frame.

## Views and targets

A `RenderView` is a camera description in framebuffer pixels: size, `CameraMatrices`, camera position, clear color, and (since #1032) the camera's exposure as a scale, its tone mapping, and an [exposure view](#exposure-views). `extract_render_view(world, camera, width, height)` builds one from a camera entity through `World::camera`. `make_render_view(camera, pose, width, height)` builds one from camera data and a rigid pose, for cameras that are not entities. Both take the aspect ratio from the view's own size, and the exposure and tone mapping from the camera's component, and return `nullopt` for a zero size or an invalid camera ([camera rules](spatial.md)). Several views can render the same snapshot in one frame, each into its own target. They do not need separate Worlds or separate extraction.

**Poses between ticks** (#1016). `RenderExtractOptions::poses` and the last argument of `extract_render_view` take a `PresentationPoses`: world matrices shown in place of the World's for some entities, such as a play session's between ticks ([play](play.md#between-ticks)). Entities not in it draw at the World's pose; without it, extraction is as before. The player and the editor's Play pass `PlaySession::presentation()`.

`RenderTarget` owns one view's textures: the HDR scene color the scene is drawn into (`scene_color()`, `rgba16_float` by default: `RenderTargetDesc::scene_format`), its `depth32_float` depth, and the color texture it is tone-mapped into (`color()`: render target, sampled, and optionally readback; `rgba8_unorm` holding sRGB-encoded values). Nothing is allocated until `resize`. Resizing to the current size does nothing, so steady frames never reallocate; `allocations()` counts real allocations. A new size creates all three textures before destroying the old ones. The device retires the old ones after their frames complete, and a failed resize keeps the previous textures. Zero sizes are rejected; skip the view instead. After a device session ends, `valid()` is false until the next `resize`. A view's size is independent of any window: offscreen captures, thumbnails, and editor panels choose their own sizes.

## Passes

`Renderer::render(snapshot, view, target)` runs inside a frame with no pass open, in up to three named passes (each timed on the GPU, [measuring](performance.md#what-is-measured)):

1. **`view`.** It uploads one `ViewConstants` block and opens a pass that clears the target's HDR scene color to the view's clear color and its depth to 1. Then, for each instance (opaque and masked ones first, then the [environment's sky](#environments) where nothing was drawn, then blended ones back to front), it binds the lit pipeline its [material](#materials) needs and the material's maps, uploads that instance's `DrawConstants` to its own slice of frame upload memory, and draws the shared mesh. Depth is stored only when debug lines will test against it.
2. **`tone map`.** One triangle over the target's color reads each pixel's scene light, scales it by the view's exposure, and writes it [tone-mapped and sRGB-encoded](#exposure-and-tone-mapping).
3. **`debug lines`**, only when the snapshot has any: [debug lines](#debug-lines) over the tone-mapped color, against the scene's depth.

It checks that the view size matches the target, that the target is live, and that every instance refers to meshes and textures the snapshot holds. It returns the first device error, such as exhausted upload memory. The remaining work is skipped, each open pass is still closed, and the frame can still end. The clear color is scene light like any other, before exposure: the views' default 0.1 grey shows as about 35% grey through AgX at EV100 0.

`Renderer::present(target, destination, area, background)` opens a pass on `destination`, clears it to `background`, and draws the target's color texture scaled into `area`. The area is a `PixelRect` in the destination's pixels, with its origin at the top left. The destination is usually the acquired window surface, but any render-target texture works. The player presents to the whole surface; the same call can present into any rectangle, such as a panel. When the area and the view have the same size, presentation copies the view's pixels exactly (bilinear sampling at texel centers).

Pipelines are created on first use for each target format (the four lit pipelines, for the scene format, with `depth32_float` and counter-clockwise front faces, each once a material needs it; the sky, once a snapshot draws one; tone map, for the color format; and the two debug pipelines, only once a snapshot has debug lines) and each destination format (present). A shader compile failure is cached and returned without recompiling every frame. When the device starts a new session, the renderer drops its old handles, and its texture placeholder, and recreates what it needs. `stats()` counts views, draws, presents, and debug draws, outlines, and lines.

## Debug lines

[Issue #1022](https://work.rezee.app/kash/issues/1022) adds a pass for debug lines and outlines, used by the [physics debug views](physics.md#debug-views). It is an ordinary part of `render`, so it works in any view: the editor's Scene and Game views, the player, and offscreen targets. Since #1032 it draws after tone mapping, into the view's color, so lines keep exactly the colors they were given.

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

## Exposure and tone mapping

[Issue #1032](https://work.rezee.app/kash/issues/1032) renders through an HDR pipeline, as the [rendering and content record](architecture/rendering-content-decision.md#color-rgba16f-ev100-and-agx) chose. The lit pass writes scene light, unclamped, into the target's `rgba16_float` scene color; the tone-map pass turns it into what the display shows.

- **Exposure** is camera data: `CameraComponent::exposure` in EV100 (default 0, from −10 to 24). Scene light is scaled by `exposure_scale(ev100)` = 1 / (1.2 × 2^EV100), the photometric saturation-based exposure at ISO 100. Each step of EV100 halves the light shown; EV100 log2(1/1.2) ≈ −0.26 would show scene light as it is. Exposure is fixed; metered (automatic) exposure is a later option.
- **Tone mapping** is `CameraComponent::tone_mapping`: **AgX** by default, or **Khronos PBR Neutral**. AgX (the common approximation of Troy Sobotka's AgX, default look) rolls highlights off smoothly toward white with stable hues, and desaturates saturated colors as they brighten: a pure red surface shows as pinkish red. PBR Neutral keeps base colors as authored up to about 0.76 and then compresses them; use it for material and product views.
- **Output.** The result is sRGB-encoded into the view's `rgba8_unorm` color, which every consumer (the editor's viewport, the player's window, offscreen readback) shows as it is.
- **Scenes from before #1032** load with EV100 0 and AgX ([migration](scene.md#versions-and-migration)). EV100 0 shows scene light a quarter stop darker than the old renderer, which wrote linear values to the display unencoded. Old images were darker in the midtones and more saturated; the V1 references were re-blessed once for this ([acceptance](acceptance.md#regression-scenes)).
- **One path.** The editor's Scene view (the editor camera's own exposure and tone mapping, in the Inspector), its Game view and the player (the scene camera's), and offscreen views all render through `Renderer::render`. A view from a camera entity and one from the same camera data and pose give identical images.

**Cost** (Release on the M4 Pro reference machine, thermal state nominal; 1920 × 1080, 3,000 sampled frames, three runs; GPU time per pass from #1026's timestamps). Observations, not budgets:

| Manifest | `tone map` | `view` | GPU per frame | CPU per frame |
| --- | --- | --- | --- | --- |
| `sample` | 0.073–0.074 ms | 0.050–0.052 ms | 0.21 ms (0.044 ms before #1032) | 0.11 ms (0.04 ms before) |
| `i1_10k` | 0.144–0.147 ms | 0.43 ms | 0.60 ms (0.77–0.91 ms before) | 2.30–2.33 ms (2.12–2.14 ms before) |

- **The tone-map pass** costs 0.07–0.15 ms at 1080p, in line with the #1030 prototype's 0.26 ms for AgX. The light `sample` scene runs at low GPU clocks, so its pass reads slower than under load.
- **The CPU** pays for encoding one more pass and its constants: about 0.07 ms in `sample`, and within the variation between sessions in `i1_10k`.
- **GPU time** cannot be compared directly across sessions: it depends on clocks the machine chooses. In `i1_10k`, the frame's GPU time fell, and no pass is slower than before.
- **Memory.** Each view adds an `rgba16_float` scene color: 15.8 MiB at 1920 × 1080.
- **Overhead.** The matched runs without CPU scopes and pass timing differ by +4.4% (CPU) and +1.4% (GPU) in `sample` and −0.4% and −0.1% in `i1_10k`, within the variation between runs.

### Exposure views

`RenderView::exposure_view` replaces the tone-mapped image with a diagnostic view of the exposed scene light, without sRGB encoding. The editor offers them in the viewport's eye menu ([editor](editor.md#physics-debug-views)).

| View | What a pixel shows |
| --- | --- |
| `luminance` | Exposed luminance (Rec. 709) in grey by stops from middle grey (0.18): black at −8 stops, 50% grey at middle grey, white at +8. |
| `false_color` | A band per range of stops from middle grey: dark violet below −6, blue −6 to −4, cyan −4 to −2, green −2 to −0.5, grey within half a stop of middle grey, yellow +0.5 to +2, orange +2 to +4, red +4 to +6, and pink beyond +6, where AgX has reached white. |

The views ignore the tone mapper. They help set exposure: a well-exposed subject is mostly grey to yellow in false color.

## Materials

[Issue #1033](https://work.rezee.app/kash/issues/1033) replaces the Blinn-Phong factors with glTF's metallic-roughness model, as the [rendering and content record](architecture/rendering-content-decision.md#shading-and-lights) chose. [renderer.metal](../resources/shaders/metal/renderer.metal)'s `litFragment` and the CPU reference [tests/support/shading.hpp](../tests/support/shading.hpp) are the same model:

- **Specular:** GGX distribution with α = roughness², height-correlated Smith visibility, and Schlick Fresnel with F0 = mix(0.04, base color, metallic). Roughness is clamped to at least 0.045, so a mirror under a directional light stays finite.
- **Diffuse:** Lambert, base color × (1 − metallic) / π, weighted by 1 − F, as glTF's appendix B writes it.
- **Lights.** A directional light's `color × intensity` is the illuminance E (lux) on a surface facing it; the surface sends (diffuse + specular) × E × N·L toward the eye. A white diffuse surface facing a light therefore shows E / π, about E × 0.96 / π with Fresnel. The default intensity is π, so a default light still shows such a surface at scene light 1.
- **Light from the surroundings** is the scene's [environment](#environments) when it has one (since #1035), else `RenderExtractOptions::ambient`, a uniform environment of that radiance. The specular takes its share E_spec = F0 × scale + bias from the split-sum table, and diffuse takes what is left: (c_diff × (1 − E_spec) × diffuse light + E_spec × specular light) × occlusion. A white dielectric in a uniform environment shows exactly its light (the white furnace). Until #1035 the share came from Karis's analytic fit of the table, which reads up to 2.5% low for smooth metals.

| Input | From |
| --- | --- |
| Base color | `base_color` (linear RGBA) × the base color map (sRGB, decoded by the sampler) × the vertex color. |
| Metallic, roughness | The factors × the metallic-roughness map's blue and green. |
| Normal | The normal map in tangent space: x in red, green, and blue and y in alpha, as [textures](assets.md#textures) store normals; z is rebuilt, then x and y scaled by `normal_scale`. +Y points up the texture. |
| Occlusion | 1 + `occlusion_strength` × (the occlusion map's red − 1), on ambient light only. |
| Emissive | `emissive` × `emissive_strength` × the emissive map, added after lighting. |
| Texture coordinates | Every map is read at the vertex's texture coordinates transformed by the material's `uv_scale`, `uv_rotation`, and `uv_offset` (#1036, `KHR_texture_transform`; [properties](properties.md#materials)): the CPU passes the rotation and scale as a 2×2 matrix (`material_uv_transform`) and the offset, in `MaterialConstants` (96 bytes). |
| Alpha | Base color alpha. `opaque` ignores it; `mask` discards fragments below `alpha_cutoff`; `blend` blends over what is behind. |
| Double-sided | Drawn without culling; seen from behind, the normal, tangent, and bitangent all turn around. |

**Tangents.** `Vertex` carries a tangent (xyz along increasing u, w the bitangent's sign) and grew from 64 to 80 bytes. Where a mesh has none, as every OBJ, the loader generates them with the reference MikkTSpace ([tangents.hpp](../include/maya/core/tangents.hpp)), per triangle corner, and then shares the corners that agree. Texture v grows downward, as in glTF, so MikkTSpace is given t = 1 − v: its sign is then glTF's, and cross(normal, tangent) × w points up the texture. A mirrored half of a model gets w = −1. The shader moves tangents by the model matrix (they lie in the surface) and normals by the [normal matrix](#normals), then makes the tangent perpendicular again.

**Energy.** The direct-light model is glTF's, so it has glTF's limits. Its directional albedo (all light from the hemisphere, as the white furnace test measures it) stays at or below 1 for metals at every angle, and for everything where N·V ≥ 0.7. Smooth dielectrics at grazing angles exceed it, because 1 − F(V·H) barely shrinks the diffuse there while the specular grows. Rough surfaces lose energy instead, since there is no multiple scattering:

| White surface | N·V 0.1 | 0.25 | 0.4 | 0.7 | 1.0 |
| --- | --- | --- | --- | --- | --- |
| Dielectric, roughness 0.25 | 1.35 | 1.18 | 1.06 | 1.00 | 1.00 |
| Dielectric, roughness 1 | 0.99 | 0.98 | 0.98 | 0.97 | 0.97 |
| Metal, roughness 0.25 | 0.90 | 0.96 | 0.98 | 0.99 | 1.00 |
| Metal, roughness 1 | 0.76 | 0.60 | 0.50 | 0.38 | 0.31 |

The Sample Viewer comparison that this model must pass belongs to the glTF importer, [#1036](https://work.rezee.app/kash/issues/1036).

**Materials in a snapshot.** Extraction copies each material into its instances (`RenderMaterial`), with the emissive already times its strength. It leases each texture once per snapshot (`RenderSnapshot::textures`), however many materials use it, and records each slot as an index, `no_texture`, or `placeholder_texture`. A material's map that is missing, failed, or of the wrong role for its slot (base color and emissive need color textures, normal needs normal, metallic-roughness and occlusion need data) draws the [placeholder](assets.md#textures) and is reported once per material (`missing_texture`, `texture_role`). Edits, reloads, and [the editor's live edits](editor.md#materials) reach every instance at the next extraction.

**Drawing.** Opaque and masked instances draw first, in snapshot order; blended ones follow, back to front by the distance from the camera to their origin, and test depth without writing it. There are four lit pipelines (opaque or blended, single- or double-sided), each created when a material first needs it. Each draw's transforms go to the vertex stage (`DrawConstants`, buffer 1); its material's factors and flags go to the fragment stage (`MaterialConstants`, buffer 3), with five texture and sampler slots, all uploaded and bound only when they differ from the previous draw's. An empty slot holds the renderer's own placeholder, which a flag tells the shader not to sample. Blending sorts whole instances, so intersecting or overlapping blended surfaces can still sort wrong; order-independent transparency is not planned.

**Cost** (Release on the M4 Pro reference machine, thermal state nominal but the machine otherwise busy; 1920 × 1080, 3,000 sampled frames, three runs each, before and after #1033 back to back; GPU time per pass from #1026's timestamps). Observations, not budgets:

| Manifest | `view` before | `view` after | CPU frame before | CPU frame after |
| --- | --- | --- | --- | --- |
| `sample` | 0.037–0.043 ms | 0.069 ms | 0.089–0.093 ms | 0.118–0.119 ms |
| `i1_10k` | 0.53–0.58 ms | 0.50–0.56 ms | 2.14–2.15 ms | 1.96–1.97 ms |
| `materials` (new) | | 0.077–0.090 ms | | 0.122 ms |

- **The shading pass** costs about 0.03 ms more for the sample's few large surfaces at 1080p: GGX, Smith, and Fresnel per light, and the environment fit, per pixel. With 10,000 small cubes it is within the run-to-run variation (the `tone map` pass, unchanged, read 0.16–0.22 ms in the same runs, so the GPU's clocks moved more than the shading did). The material scene, with every kind of map, a cutout, and blending, costs 0.08–0.09 ms.
- **The CPU** pays less than before. Per-draw constants (the transforms) now go to the vertex stage and material constants to the fragment stage, uploaded only when a draw's material differs from the previous draw's. Binding new constants to the fragment stage makes Metal's driver re-emit that stage's whole argument table, now five textures and five samplers larger. Before the split, that put `i1_10k`'s encoding at 1.73–1.77 ms; with it, encoding is 1.27 ms, against 1.47 ms before #1033. Extraction is 0.03 ms slower, copying the larger materials.

### Version-1 content

Version-1 materials (base color, metallic, roughness) load as before and keep their meaning. The sample scenes' lights were multiplied by π (from 1 to 3.1415927) so they keep their brightness; a scene of your own from before #1033 shows about a third as bright until you do the same. Against the V1 references, rendered by the old model through the same exposure and tone mapping (256 × 144, the old images re-blessed afterwards):

| Image | Mean difference | Pixels over 6 levels | Largest | Mean level, before → after |
| --- | --- | --- | --- | --- |
| `v1/overview` | 1.9 | 4.4% | 18 | 114.5 → 113.0 |
| `v1/overlap` | 2.8 | 10.0% | 21 | 125.4 → 124.0 |
| `v1/path-0000` | 4.4 | 14.2% | 18 | 138.9 → 136.0 |
| `v1/path-0450` | 4.1 | 7.8% | 92 | 131.0 → 129.3 |
| `hdr/pbr-neutral` | 3.0 | 25.9% | 28 | 90.8 → 88.3 |
| `hdr/false-color` | 3.0 | 3.9% | 77 | 113.9 → 112.5 |

Large surfaces are slightly darker (Fresnel takes a little from diffuse) and within 18 levels. The largest differences are new GGX highlights: the blue metal bar's top face catches the sun in `path-0450`, and highlights move a band in false color. The other V1 and HDR images lie between these.

## Environments

[Issue #1035](https://work.rezee.app/kash/issues/1035) lights scenes from their surroundings with image-based lighting, as the [rendering and content record](architecture/rendering-content-decision.md#environment-lighting-cooked-on-the-cpu) chose: an [environment asset](assets.md#environments), cooked on the CPU when it loads, and a scene's `maya.environment` component ([properties](properties.md#discovery-and-identity)).

- **The component.** `EnvironmentComponent` names the environment, its `intensity` (a scale, default 1), its `rotation` about +Y (radians, counter-clockwise seen from above: a quarter turn brings the environment's +X to the world's -Z), and `background` (draw it as the sky, default on). A scene uses one: the one with the lowest EntityId; any other is reported (`environment_limit`). One with no environment assigned lights nothing and says nothing.
- **Diffuse.** The environment's irradiance as nine spherical-harmonic coefficients (l ≤ 2), already convolved with the cosine lobe, so the shader evaluates irradiance(n) and the diffuse light is irradiance / π × intensity. Nine coefficients hold light that varies smoothly with direction exactly; a small bright source such as the sun rings, and negative irradiance is clamped to 0.
- **Specular.** A GGX-prefiltered cube (RGBA16F, 128 texels a side by default, six levels): level L is for perceptual roughness L / (levels − 1), and the shader reads the reflection direction at roughness × (levels − 1). The split-sum table (`brdf_table`, 64 × 64 RGBA16F, N·V across and roughness down, 0 and 1 at the first and last texels' centres, 512 importance samples per entry, height-correlated Smith visibility) gives the scale and bias, built once per process (28 ms in Release) and uploaded once per renderer.
- **Occlusion** darkens both the environment's diffuse and specular light, as it did the ambient light.
- **Replaces the ambient light.** With an environment, `RenderExtractOptions::ambient` is not used. A missing or failed environment falls back to it, and is reported once per extraction (`missing_environment`) with the asset's ID and the registry's reason, so the scene is still lit.
- **The sky.** With `background` on, the view pass draws one triangle at the far plane after opaque and masked surfaces and before blended ones, testing depth (`less_equal`, without writing it), so it shows only where no surface was drawn and blended surfaces show it through them. It reads the environment's background (the source image, RGBA16F) along each pixel's view ray, from the inverse view-projection, at the finest level (the seam where u wraps would otherwise pick a coarse one), times the intensity. Without the sky, the view's clear color shows as before.
- **Mapping.** The background is equirectangular: its centre (u 0.5) lies along −Z, the default view direction; u grows toward +X; v runs from straight up (0) to straight down (1). Cube faces follow Metal's order and orientation (+X, −X, +Y, −Y, +Z, −Z). Both are in [environment_cook.hpp](../include/maya/assets/environment_cook.hpp) (`equirect_uv`, `equirect_direction`, `cube_direction`), and the shader's `equirect_uv` and `to_environment` match them.
- **Bindings.** `ViewConstants` carries the inverse view-projection, the intensity, the rotation's cosine and sine, the cube's last level, two flags (an environment lights the scene; the sky is drawn), and the nine coefficients. Texture slot 5 holds the cube (a 1-texel black cube stands in without an environment), 6 the background, and 7 the split-sum table; sampler slot 5 is the environment's (linear, repeating around the horizon), 7 the table's (linear, clamped). They are bound once per view pass.

**Cost** (Release on the M4 Pro reference machine, thermal state nominal; 1920 × 1080, 3,000 sampled frames, three runs). Building an environment once per version, with its 1024 × 512 source on every core and 256 samples per prefiltered texel:

| Environment | Decode | Build, 128 per face | Build, 256 per face | GPU memory |
| --- | --- | --- | --- | --- |
| `workshop` (indoors) | 7.9 ms | 60 ms | 202 ms | 5.3 MiB background + 1.0 MiB cube (4.0 MiB at 256) |
| `sky` (outdoors) | 5.0 ms | 58 ms | 202 ms | the same |

The `view` pass of the material test scene, with the environment and the sky, with the environment and no sky, and with neither (the ambient light), measured back to back:

| `materials` | `view` | GPU per frame | CPU per frame |
| --- | --- | --- | --- |
| Environment and sky | 0.106–0.131 ms | 0.32–0.33 ms | 0.14 ms |
| Environment, no sky | 0.078–0.091 ms | 0.26 ms | 0.12 ms |
| Ambient light | 0.069–0.079 ms | 0.24 ms | 0.11 ms |

- **Image-based lighting** costs about 0.01 ms in this scene, a cube sample and nine coefficients per pixel of surface.
- **The sky** costs about 0.03 ms at 1080p, where most of the frame is sky; it is drawn inside the `view` pass rather than as a pass of its own, so the difference measures it. A separate pass would store and reload the 16 MiB scene color.
- **Without an environment** the split-sum table replaces the analytic fit: `sample`'s `view` pass read 0.061 ms against 0.059 ms before #1035, and `i1_10k` did not change beyond its run-to-run variation (0.95–0.98 ms against 0.95–0.97 ms).

### Normals

Normals are transformed by the inverse transpose of the world matrix's linear part. Extraction computes it in double precision from the cofactor matrix and scales it to stay representable; the shader renormalizes. Normals therefore stay perpendicular to surfaces under nonuniform scale, including scale inherited through the hierarchy. Transforms have strictly positive scale, so no world matrix is a reflection and triangle winding never flips. Extraction rejects a nonpositive or degenerate determinant as `invalid_transform`.

The shader's `Vertex` uses `float3`, which occupies 16 bytes in Metal and matches the padded C++ [Vertex](../include/maya/rhi/vertex.hpp). The legacy shader used `packed_float3`, which put the normal at byte offset 12 instead of 16. As a result, every earlier lit draw read its normals from the wrong bytes. The layouts of `ViewConstants`, `DrawConstants`, and `PresentConstants` are defined in [shader_constants.hpp](../include/maya/renderer/shader_constants.hpp), with static assertions. Buffer index 0 holds vertices, 1 holds per-draw constants, 2 holds per-view constants, and 3 holds material constants; texture and sampler slots 0 to 4 hold the material's maps, in `MaterialSlot` order.

## Sample content

The [basic scene](../samples/basic_scene/assets/basic.scene) is an ordinary [scene file](scene.md): a camera, a directional sun, the pyramid, and one [cube mesh](../samples/basic_scene/assets/cube.obj) drawn three times (a red cube, a blue metal cube, and a ground slab scaled 6 × 0.1 × 6). Four [material files](../samples/basic_scene/assets/materials) supply their factors; they are still version 1, which loads unchanged. The sample's fly controller writes the camera entity's transform and animates the pyramid through World commands. The renderer only reads the result. The pyramid's side normals were also corrected; three of its four side faces had been using another face's normal.

The [material test scene](../samples/basic_scene/assets/materials.scene) (#1033) shows the model: a [UV sphere](../samples/basic_scene/assets/sphere.obj) in two rows, a red dielectric and a gold metal, each at roughness 0, 0.25, 0.5, 0.75, and 1, and below them six surfaces: a base color map, a normal map, a packed occlusion-roughness-metallic map in tiles, an emissive map, a masked double-sided cutout, and blended glass. Its version-2 materials are in `materials/spheres` and `materials/surfaces`, and its two maps (`orm`, `cutout`, RGBA8 so their texels stay exact) in `textures`. Since #1035 an Environment entity lights it with the [sky environment](assets.md#environments) and draws it as the sky; its references also show it under the workshop.

## Tests

- [renderer_tests.cpp](../tests/renderer_tests.cpp) (`maya_renderer_tests`, labels `cpu;renderer`) uses a null device that mirrors buffer contents and records the constants bound at every draw. It covers mesh sharing, copied transforms and materials, and each missing-asset rule. It checks normal matrices under nonuniform scale in a hierarchy and light selection and limits. It checks that snapshot ownership survives entity deletion, eviction, and registry/World destruction, with deferred retirement. It also covers two views of one snapshot, target reallocation and retirement, upload exhaustion, presentation rectangles, and pipeline recreation across sessions.
- [renderer_gpu_tests.cpp](../tests/renderer_gpu_tests.cpp) (in `maya_tests`, tag `[rhi]`, run under Metal API validation) reads pixels back. It checks per-instance colors from one shared mesh and a skipped missing mesh. It checks diffuse lighting of a slanted quad scaled 1 × 1 × 4, which only the inverse-transpose normal passes. It checks identical output presented into a player-sized window and an editor viewport rectangle, rendering at six sizes with target reuse and retirement, and ten rounds of deleting the drawn entity and evicting its mesh while its frame is still in flight.
- **Materials** (#1033). [material_gpu_tests.cpp](../tests/material_gpu_tests.cpp) (in `maya_tests`, tag `[materials]`) reads the HDR scene color (`rgba16_float`, before exposure) of quads seen head-on and compares it with the CPU reference in [shading.hpp](../tests/support/shading.hpp): every metallic and roughness under several lights and angles (within 1%); the white furnace, where a white dielectric in a uniform environment shows exactly its light; the reference's directional albedo; each map (base color with sRGB decoding, metallic-roughness channels, occlusion by strength on ambient only, emissive by strength); normal-map orientation (+X toward +u, +Y up the texture, on plain and mirrored UVs, and by scale); alpha cutoffs; blending back to front; double-sided back faces; and the placeholder for missing maps and maps of the wrong role. [renderer_tests.cpp](../tests/renderer_tests.cpp) checks pipeline choice, draw order, constants, and the textures bound in each slot; [material_tests.cpp](../tests/material_tests.cpp) the material files, the schema, publishing, and tangents. The [material test scene](#sample-content) has reference images ([acceptance](acceptance.md#regression-scenes)).
- **Environments** (#1035). In material_gpu_tests.cpp (tag `[environments]`): the furnace under a uniform environment for white, red, and metal surfaces at every roughness, head-on and turned, against the CPU reference with the split-sum table (`brdf_scale_bias`); the environment replacing the ambient light and its intensity scaling it; irradiance by direction and the rotation's sense; the mirror reflection of a smooth metal and occlusion darkening it; the sky ahead, turned a quarter, through a blended surface, and off; and the fallback to the ambient light for a missing environment. renderer_tests.cpp checks extraction (one environment, the lowest EntityId's, the limit and missing diagnostics), the view constants and their inverse view-projection, the bound slots, and that the sky draws after opaque surfaces and before blended ones. [environment_tests.cpp](../tests/environment_tests.cpp) checks environment files, HDR decoding, the mappings, irradiance (exact for light linear in direction), the prefiltered cube, determinism across thread counts, the split-sum table against the BRDF's integrated albedo, and loading.
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
