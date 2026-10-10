# Procedural content: generators and terrain

[#1068](https://work.rezee.app/kash/issues/1068) adds generator assets and the first thing made from them, height-field terrain, as [#1060's record](architecture/world-scale-decision.md#terrain-cdlod-over-cell-tiles) chose. `MayaProcedural` (`Maya::Procedural`, CPU only, linking MayaJobs) holds the noise and the height tiles ([noise.hpp](../include/maya/procedural/noise.hpp), [height_field.hpp](../include/maya/procedural/height_field.hpp)); MayaAssets links it and holds the asset ([generator.hpp](../include/maya/assets/generator.hpp), [generator_file.hpp](../include/maya/assets/generator_file.hpp)); MayaRenderer draws terrain ([terrain.hpp](../include/maya/renderer/terrain.hpp)), MayaPhysics collides with it ([height fields](physics.md#height-fields)), and MayaStreaming streams it ([terrain_streamer.hpp](../include/maya/streaming/terrain_streamer.hpp)).

On 10 October 2026 the project owner split the issue in two. #1068 is the generator asset, the noise height field, and terrain. [#1093](https://work.rezee.app/kash/issues/1093) is scatter, authored overrides of generated items (kept in the cell's scene), the editor's preview, regenerate, cancel, and bake (keeping item IDs), and the `maya.generator` component that names a generator on its own. Until then a terrain names its generator directly, and there is no `maya.generator` component.

## Generator assets

A generator is a catalog asset of kind `generator` ([assets](assets.md#catalog-and-material-files)), a `GeneratorAsset`: what it makes, the version of the algorithm that makes it, its seed, its parameters, and its region. Its output is never authored: it is generated where it is used, cooked into the cook cache, and generated again only when one of these changes.

| ID / name | Type | Range | Means |
| --- | --- | --- | --- |
| 1 `kind` | choice: `heightfield.noise` | | What it makes. Scatter (`scatter`) comes with #1093. |
| 2 `seed` | integer | 0 to 2,147,483,520 | Chooses the output. |
| 3 `octaves` | integer | 1 to 12 | Layers of noise, each half the wavelength and half the height of the one before. |
| 4 `wavelength` | scalar (m) | 1 to 100,000 | The largest features' size. |
| 5 `amplitude` | scalar (m) | 0 to 10,000 | How far they rise and fall from the base. |
| 6 `warp` | scalar (m) | 0 to 10,000 | How far noise bends them sideways, so ridges wander. |
| 7 `base` | scalar (m) | −10,000 to 10,000 | Added to every height. |
| 8 `region_min_x`, 9 `region_min_z` | integer (cells) | −1,024 to 1,023 | The region's first 128 m cell along x and z. |
| 10 `region_max_x`, 11 `region_max_z` | integer (cells) | −1,024 to 1,023 | Its last, included. |

The region is a rectangle of [world cells](world.md#worlds-on-disk), about 131 km either way of the origin at most; the default is cells 0 to 3, a 512 m square. `generator_properties()`, `generator_property`, `read_property(generator, id)`, `edit_properties(generator, edits, context)`, and `validate_generator` work as the [material properties](properties.md#generators) do: on a copy, checking the whole result, so a region whose last cell comes before its first is refused (against property 10 or 11), whichever corner was edited.

### Generator files

A generator file holds one generator ([generator_file.cpp](../src/maya/assets/generator_file.cpp)):

```text
maya-generator 1
algorithm 1
kind heightfield.noise
seed 1068
octaves 5
wavelength 260
amplitude 24
warp 30
base 0
region_min_x -2
region_min_z -2
region_max_x 1
region_max_z 1
```

- **The format** is `maya-generator 1`, then `algorithm <n>`, then one line per property, keyed by its name; a property without a line has its default, and `#` lines are comments. Numbers are written in their shortest exact form.
- **The algorithm version** is the version of the kind's algorithm the output came from (`heightfield_noise_version`, 1). A version this build does not generate is refused, naming both versions, so output never changes silently: a change to the noise that changes heights makes version 2.
- **Refusals** name the line: another format version, a second `algorithm` or property line, an unknown property, a value of the wrong kind (`'octaves' needs one whole number`), and a value out of range or an inverted region, which are checked together after reading, against the line of the property they are about. A file with no `algorithm` line is refused.
- `write_generator_file` writes every property in schema order; `save_generator_file` replaces the file atomically. The sample's is [hills.generator](../samples/basic_scene/assets/terrain/hills.generator).

## Noise

`noise_height(noise, x, z)` is Maya's own seeded gradient noise, with no library, so heights never change with a library's version ([height_field.cpp](../src/maya/procedural/height_field.cpp)):

- **The lattice.** Each integer lattice point's gradient is an angle hashed from the seed and the point with SplitMix64's finalizer. A point's noise is the four corners' gradients dotted with its offsets from them, blended by the quintic 6t⁵ − 15t⁴ + 10t³.
- **fBm.** `octaves` layers, the first at `wavelength`, each next at twice the frequency and half the amplitude, each with its own seed (seed + octave).
- **Warp.** The position is first moved by `warp` metres along x and z, each by an fBm of its own seed, so features bend and ridges wander. The height is `base + amplitude × fBm(warped position)`.
- **Determinism.** It is evaluated at whole metres (64-bit integers), in doubles for positions and floats for values, with no state: the same inputs give the same bits on every thread and run. As the record allows, results are not promised identical across compilers or platforms; they are cooked on the machine that uses them.

## Height fields

`generate_height_tile(noise, index, context, parallel)` makes one cell's tile; `TileIndex` is the cell, floor(position / 128) in x and z.

- **Tiles** hold 129 × 129 heights (`tile_samples`) a metre apart, rows along x, from the cell's low corner to its far edge inclusive. A tile's last row and column are its neighbours' first, generated from the same points, so shared edges are equal, heights and normals alike.
- **Normals** come from central differences over a one-sample apron around the tile, so they continue across its edges. Each is stored as two bytes, its octahedral projection onto x and z, 8 bits each (`oct_encode`).
- **Jobs.** With `parallel`, rows are split across the job system's background workers (`parallel_for`); the bytes are the same either way. A cancelled context returns nothing.
- **The overview** (`generate_height_overview`) covers the whole region at 16 m (`overview_spacing`): (cells × 8 + 1) samples a side, each equal to the tiles' sample at the same point, with normals the same way. It is what terrain beyond the loaded tiles is drawn from.
- **Cooked bytes.** `encode_height_tile` writes "HMT1", the index, the range, the heights, and the normals; `encode_height_overview` writes "HMV1", the region, the range, then the same. The readers refuse a wrong size, a damaged header, or a height that is not finite or outside its range.

### On the GPU

`pack_heights` gives the renderer two 32-bit words per sample: the height's float bits, then the normal. #1060's record proposed 16-bit heights over each tile's range, but two tiles with different ranges decode their shared edge to different heights, which cracks; whole floats keep shared edges exact. A tile is 133,128 bytes on the device; with 16-bit heights beside the same normals it would be 66,564.

## Cooking

Tiles and overviews are cooked into the project's [cook cache](assets.md#cook-cache), under keys from [terrain_streamer.hpp](../include/maya/streaming/terrain_streamer.hpp):

| Kind | Key: besides the kind and its version | Version |
| --- | --- | --- |
| `height-tile` | the noise's digest (the algorithm version, seed, octaves, wavelength, amplitude, warp, and base) and the tile | `height_tile_cook_version` 1 |
| `height-overview` | the noise's digest and the region | `height_overview_cook_version` 1 |

A tile's key leaves out the region, so widening a region keeps the tiles already cooked. Pruning keeps every key a cataloged generator could produce (`append_generator_cook_keys`: its overview and every tile of its region, through `reachable_cook_keys`), so a pruned cache stays warm. A [package](#packages) holds no cook cache.

## Terrain

A `maya.terrain` component ([properties](properties.md#discovery-and-identity), [components.hpp](../include/maya/world/components.hpp)) makes an entity a terrain: heights from a height-field generator, drawn and collided with per cell, and up to four material layers.

| ID / name | Type | Range |
| --- | --- | --- |
| 1 `heights` | generator reference | |
| 2 `layer0` | material reference: the ground, everywhere the other layers are not | |
| 3 `layer1`, 6 `layer2`, 9 `layer3` | material reference: over the layers before it | |
| 4 `layer1_slope`, 7 `layer2_slope`, 10 `layer3_slope` | vector2 (degrees from level) | 0 to 90; default 90 90 |
| 5 `layer1_height`, 8 `layer2_height`, 11 `layer3_height` | vector2 (m) | −10,000 to 10,000; default −10,000 −10,000 |
| 12 `texture_size` | scalar (m): the ground distance each layer's maps repeat over | 0.01 to 1,000; default 4 |

The terrain is placed by its generator's region, in world coordinates, not by its entity's transform. In a [world](world.md#worlds-on-disk) a terrain belongs to the persistent part. The sample's [terrain scene](../samples/basic_scene/assets/terrain.scene) is a 4 × 4-cell terrain of grass and rock, rock on slopes from 20° to 32°, with a camera, a shadowed sun, and a boulder that falls onto it in Play.

### The terrain material

Each layer names an ordinary [material](renderer.md#materials) (base color, metallic-roughness, normal, and occlusion maps and factors; emissive and alpha are not used). Layer 0 covers everything; each later layer fades in over the ones before by its slope ramp times its height ramp, each from `.x` (none of it) to `.y` (all of it), so a ramp whose `.y` is below its `.x` fades the layer out instead, and equal ends are a step. Slope comes from the heights' normals. Maps are mapped across x and z, repeating every `texture_size` metres, with a tangent frame along +x on the surface. The blended surface is then shaded exactly as a mesh's is ([renderer](renderer.md#terrain)).

### Drawing: CDLOD

The renderer draws terrain with CDLOD, as the record chose ([select_terrain_patches](../src/maya/renderer/terrain.cpp)):

- **The quadtree** is aligned to the world: roots are 4,096 m squares over the region, and level L's nodes are patches of 32 × 32 quads, 32 · 2^L metres a side, from level 0 (32 m, 1 m quads) to level 7. Levels 0 to 2 read a tile (a level-2 patch is one cell); level 3 and up read the overview. A patch whose tile is not loaded reads the overview.
- **Ranges.** Level L is drawn out to `terrain_range(L)` = 128 · 2^L metres, four times its size (level 0 to 128 m, level 2, the tiles' last, to 512 m); a node closer than the next finer level's range splits. In its last quarter beyond the previous level's range (`terrain_morph_start`), each vertex morphs toward the next coarser grid, reaching it exactly at the end.
- **One distance.** Selection and the vertex shader measure the same distance: horizontal to the patch (the vertex), plus one height for the whole terrain, how far the eye is above or below the terrain's band of heights (`terrain_height_gap`). With these ranges a patch never borders one two levels coarser, the finer side of a level boundary has finished morphing, and the coarser has not begun: levels meet without cracks, skirts, or stitching. The CDLOD test checks this from 60 random eyes in and above an 11 × 7-cell region.
- **Tile to overview.** A level-2 patch reading a tile blends its heights (and normals) toward the overview's as it morphs, since the level beyond reads the overview, and is exactly the overview's when fully morphed.
- **Exact placement.** Patches are placed in whole metres from the region's low corner (the anchor), so neighbouring patches compute equal positions for their shared vertices and no pinholes show between them.
- **Draws.** Patches are grouped by what they read: one instanced draw per tile, then one for the overview's, from a shared 33 × 33-vertex grid.

Tiles are read within 874 m (`terrain_tile_reach`: level 2's range and the diagonal of the node above it), which is why tiles load to 896 m ([streaming](#streaming)).

**Deviations from the record.** It proposed morphing over the last 30% of each range and patches from 128 m down to 32 m within each cell. Here morphing is over the last 25%, ranges are four times a patch's size, and the quadtree runs to 4,096 m patches over the overview, so the levels' ranges hold with the height-gap distance. Heights on the GPU are floats ([above](#on-the-gpu)).

### Collision

Each loaded tile within the activation radius is a static Jolt `HeightFieldShape` (block size 4, 8 bits a sample, as the record chose) owned by the terrain entity ([height fields](physics.md#height-fields)): bodies rest on it, and rays, shape casts, and overlaps hit it and name the terrain. Rays land within a centimetre of the sampled surface.

## Streaming

`TerrainStreamer` keeps the tiles of every terrain in a World near the places it is seen from (`set_sources`: the camera, the player), as `WorldStreamer` keeps cells ([world](world.md#streaming)), and with the same `StreamingSettings` (the project's radii, hysteresis, and job tier):

| State | Means |
| --- | --- |
| loading | A background job reads the tile from the cook cache or generates it (and writes it there). |
| loaded | Its heights are held; drawn once its packed heights are on the device. |
| active | Also collided with: its height field is in the physics world. |

- **Radii.** Tiles load and are drawn within `tile_radius` (896 m, so the finest levels always have them) or the cells' loading radius if farther, nearest first, at most `tiles_in_flight` (16) at once; they activate within the activation radius (384 m by default), and each goes back only beyond its radius plus the hysteresis. Beyond the tiles, the terrain is drawn from its overview, generated once on a job and kept; a terrain is drawn only once its overview is.
- **A frame** (`update`, on the owner thread between ticks) applies finished jobs, then uploads tiles' heights and activates tiles, nearest first, within `frame_budget` (0.5 ms) and `frame_tiles` (16), and deactivates and unloads beyond the radii, removing height fields in one batch.
- **Height fields** are built on the tile's job only when a physics world will take them (`collide`, and a physics world passed to `update`): the editor's authored scene draws terrain but never builds shapes. A tile that loaded before a physics world came is built on the owner thread when it activates, within the budget, and joining the physics world counts as an activation, so `settle` waits for every one.
- **Changes.** A changed `heights` reference, a reloaded generator, or an edited one (a new version in the registry) drops the terrain's tiles and starts again; its layers are read as they are each frame.
- **Loading screens.** `settle` updates, waiting for jobs and ignoring the frame budget, until nothing more would change around the sources. `clear(physics)` takes everything out, height fields first. Destroying the streamer cancels its jobs and waits for them.
- `terrains()` gives what the renderer and picking read; `height_at(x, z)` the loaded height between samples, on the triangles the finest level draws. `stats()` counts terrains, tiles by state, tiles drawn, generated, and read from the cache, activations, uploads, height fields, CPU and GPU bytes held, and the last and longest update and tile job. Tiles are not yet a [residency](assets.md#residency) category: the streamer's own counts are their bytes.

Terrain streams by its own radius rather than as part of each cell's activation, because the finest levels need tiles out to 874 m, beyond the cells' 640 m loading radius.

## Editing

In the editor ([editor](editor.md#generators)):

- **Drawing.** Terrain in the open scene is drawn around the editor camera, in the Scene and Game views, through the editor's cook cache. It is drawn in the shadow maps and the [debug views](renderer.md#debug-views); the `lod` view tints each patch by its level.
- **Picking.** A click on the ground selects the terrain, and the pivot finds it (a ray marched over the loaded tiles to a centimetre).
- **Generators** are edited in the Inspector: folded under a selected terrain's component, or on their own when clicked in the Assets panel's GENERATORS column. Each change is one undo step in the scene's history (a drag is one), published to the registry, so the terrain regenerates at once; ⌘S writes the file. Undo, discarding, reloading, and watched files work as for [materials](editor.md#materials).
- **Play** draws and collides with the terrain around the play camera, settled before the first tick; Stop takes the height fields out with the play session's physics.
- **Recording and replay** are refused for scenes with terrain, in the editor and the player: tiles join physics frame by frame as they stream, not tick by tick, so a recording could not replay.

## Packages

A [package](projects.md#packages) holds a generator as its authored file, never its output: the packaged player generates tiles and the overview where it runs (it has no cook cache), as the record chose, since results are not promised identical across machines.

## Measurements

Release on the M4 Pro reference machine, 10 October 2026, thermal state nominal, W1's noise (6 octaves, warp 120 m), 64 tiles. Observations, not budgets:

| | Measured | #1060's record |
| --- | --- | --- |
| A tile generated on one thread | mean 4.62 ms, median 4.19 ms | 4.2 ms |
| A tile with rows across the job system | 0.64 ms | |
| Its height field built (`prepare_height_field`, on a job) | 0.21 ms | 0.20 ms |
| Its height field added, removed (owner thread) | about 1 µs, under 1 µs | |
| Its height field's size (Jolt's `GetStats().mSizeBytes`, as the record measured) | 34,151 bytes (33.4 KiB) | 33 KiB |
| Everything Jolt allocates for it (the shape object, its active-edge bits, allocator rounding) | 49,376 bytes | |
| A tile cooked, read back | 0.004 ms, 0.012 ms | |
| A tile's heights and normals held | 99,846 bytes (heights 66,564, normals 33,282); cooked 99,866 | |
| A tile on the device | 133,128 bytes | |
| The overview of 4 × 4, 16 × 16, 64 × 64 cells | 0.17 ms (6.5 KB), 1.0 ms (100 KB), 13.6 ms (1.58 MB) | |

Activation per tile is the upload (133 KB) and adding the height field, far within the 0.5 ms frame budget; the record's 1 ms a frame was #1064's activation budget. The height field matches the record's size measured the same way; what Jolt allocates in all for it is half as much again.

The [terrain benchmark](performance.md#manifests) ([terrain.benchmark](../benchmarks/terrain.benchmark), the sample's terrain scene at 1920 × 1080, 3 runs of 3,000 frames):

- **Warm** (cook cache filled): frame 0.715 ms; 16 tiles drawn and 16 collided with; about 50 patches in 12 draws and 173 shadow patches per view; longest terrain update 0.44 ms; the first frame, with loading, 8.2 ms; 1,566 KiB held and 2,088 KiB on the device.
- **Cold** (cache emptied): the first frame 97 ms, generating the 16 tiles behind the loading screen, the longest tile job 88 ms while all 16 ran at once on the background tier.

## Tests

- [procedural_tests.cpp](../tests/procedural_tests.cpp) (`maya_procedural_tests`, labels `cpu;procedural`): noise depending only on the seed, parameters, and position; tiles identical on one thread and with rows across the job system, byte for byte, with neighbours' shared rows and columns of heights and normals equal; a cancelled tile; the overview equal to tiles at every 16th metre; cooked tiles and overviews read back exactly and damaged ones refused; packing keeping every height's bits; generator files round-tripping every property and refusing what this build cannot generate; and generator properties edited atomically.
- [terrain_tests.cpp](../tests/terrain_tests.cpp) (in `maya_streaming_tests`, tag `[terrain]`): tiles loading within the loading radius and drawn, activating within the activation radius, joining the physics world as height fields of the terrain entity, leaving it as the source moves away and when the terrain is removed, joining a physics world given after they loaded, coming from the cook cache the second time with the same bytes and changing with their generator, a bounded number activated a frame, nearest first, and a streamer destroyed while tiles generate; and (`[cdlod]`) patches covering an 11 × 7-cell region once from 60 random eyes, neighbours a level apart at most, the finer side of every boundary fully morphed and the coarser not begun.
- [physics_tests.cpp](../tests/physics_tests.cpp) (`[terrain]`): rays landing within a centimetre of the samples and naming the owner, a swept sphere stopping on the ground, a body coming to rest on it, removal in a batch, and refused descriptions.
- [terrain_gpu_tests.cpp](../tests/terrain_gpu_tests.cpp) (in `maya_tests`, `[rhi][terrain]`, Metal): a 6 × 6-cell terrain at W1's place cleared to magenta, seen from 3 m to 220 m above the ground at three offsets: no magenta pixel anywhere (no cracks between tiles or levels), grass showing, under 5% of pixels dark (normals and shading work), and terrain drawn into the shadow maps; the same terrain in every debug view from `luminance` to `texels`, with no holes and an image of its own; and (`[visual]`) four views compared with the owner-approved references in `tests/references/terrain/` (from the ground, across the hills, and from 300 m above, where the tiles meet the overview, and that view's `lod` view), within the references' usual tolerance (each channel within 6 on 99.5% of pixels; `MAYA_BLESS_REFERENCES=1` renders them again, for approval).
- [editor_terrain_tests.cpp](../tests/editor_terrain_tests.cpp) (`[editor][terrain]`): the sample's terrain drawn in the editor with no height fields, picked by a click, and collided with in Play (the boulder rests on it), its height fields gone at Stop; and a generator edited in the Inspector regenerating the terrain, undone in the scene's history, and saved with ⌘S.
- [benchmark_tests.cpp](../tests/benchmark_tests.cpp) (`[benchmark][terrain]`): the terrain manifest's scene workload streaming the terrain and counting it.
- [scene_tests.cpp](../tests/scene_tests.cpp) loads the terrain scene; [package_tests.cpp](../tests/package_tests.cpp) packages generator files as authored text.
