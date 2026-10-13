# Procedural content: generators, terrain, and scatter

[#1068](https://work.rezee.app/kash/issues/1068) adds generator assets and the first thing made from them, height-field terrain, as [#1060's record](architecture/world-scale-decision.md#terrain-cdlod-over-cell-tiles) chose. `MayaProcedural` (`Maya::Procedural`, CPU only, linking MayaJobs) holds the noise and the height tiles ([noise.hpp](../include/maya/procedural/noise.hpp), [height_field.hpp](../include/maya/procedural/height_field.hpp)); MayaAssets links it and holds the asset ([generator.hpp](../include/maya/assets/generator.hpp), [generator_file.hpp](../include/maya/assets/generator_file.hpp)); MayaRenderer draws terrain ([terrain.hpp](../include/maya/renderer/terrain.hpp)), MayaPhysics collides with it ([height fields](physics.md#height-fields)), and MayaStreaming streams it ([terrain_streamer.hpp](../include/maya/streaming/terrain_streamer.hpp)). [#1093](https://work.rezee.app/kash/issues/1093) adds [scatter](#scatter): items placed over a terrain, as entities ([scatter.hpp](../include/maya/procedural/scatter.hpp), [scatter_streamer.hpp](../include/maya/streaming/scatter_streamer.hpp)).

On 10 October 2026 the project owner split the issue in two. #1068 is the generator asset, the noise height field, and terrain. [#1093](https://work.rezee.app/kash/issues/1093) is scatter, authored overrides of generated items (kept in the cell's scene), the editor's preview, regenerate, cancel, and bake (keeping item IDs), and the `maya.generator` component that names a generator on its own. A terrain still names its generator directly; a scatter generator runs where a `maya.generator` component names it.

## Generator assets

A generator is a catalog asset of kind `generator` ([assets](assets.md#catalog-and-material-files)), a `GeneratorAsset`: what it makes, the version of the algorithm that makes it, its seed, its parameters, and its region. Its output is never authored: it is generated where it is used, cooked into the cook cache, and generated again only when one of these changes.

| ID / name | Type | Range | Means |
| --- | --- | --- | --- |
| 1 `kind` | choice: `heightfield.noise`, `scatter` | | What it makes: heights, or [scatter](#scatter)'s items. |
| 2 `seed` | integer | 0 to 2,147,483,520 | Chooses the output. |
| 3 `octaves` | integer | 1 to 12 | Layers of noise, each half the wavelength and half the height of the one before. |
| 4 `wavelength` | scalar (m) | 1 to 100,000 | The largest features' size. |
| 5 `amplitude` | scalar (m) | 0 to 10,000 | How far they rise and fall from the base. |
| 6 `warp` | scalar (m) | 0 to 10,000 | How far noise bends them sideways, so ridges wander. |
| 7 `base` | scalar (m) | −10,000 to 10,000 | Added to every height; for scatter, the ground's height where it names no heights. |
| 8 `region_min_x`, 9 `region_min_z` | integer (cells) | −1,024 to 1,023 | The region's first 128 m cell along x and z. |
| 10 `region_max_x`, 11 `region_max_z` | integer (cells) | −1,024 to 1,023 | Its last, included. |

Kind, seed, base, and the region belong to both kinds; octaves, wavelength, amplitude, and warp to heights alone, and properties 12 to 36 to [scatter](#the-scatter-generator) alone (`generator_properties(kind)`, `generator_property_applies`).

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
- **The algorithm version** is the version of the kind's algorithm the output came from (`heightfield_noise_version` 1, `scatter_version` 1). A version this build does not generate is refused, naming both versions, so output never changes silently: a change to the noise that changes heights makes version 2.
- **Its kind's properties.** A file holds only its kind's properties: a line for another kind's is refused (`'octaves' is not a property of scatter generators`), not ignored. Asset references (scatter's heights, meshes, and materials) are `none` or the asset ID's two hexadecimal words; booleans are `true` or `false`. They are checked against the catalog where the generator is used, as a material's textures are, not when the file is read: since #1069 they are encoded as asset IDs (`PropertyEncoding::persistent_asset_id`), so the Inspector refuses one the catalog lacks or of the wrong kind, and [packaging](#packages) finds them.
- **Refusals** name the line: another format version, a second `algorithm` or property line, an unknown property, a value of the wrong kind (`'octaves' needs one whole number`), and a value out of range or an inverted region, which are checked together after reading, against the line of the property they are about. A file with no `algorithm` line is refused.
- `write_generator_file` writes its kind's properties in schema order; `save_generator_file` replaces the file atomically. The sample's are [hills.generator](../samples/basic_scene/assets/terrain/hills.generator) and [rocks.generator](../samples/basic_scene/assets/terrain/rocks.generator) ([scatter](#the-sample)).

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
| `scatter-cell` | [scatter's](#cooking-and-the-streamer) parameters, the generator's ID, the cell's height tile key, and the exclusions that reach the cell | `scatter_cook_version` 1 |

A tile's key leaves out the region, so widening a region keeps the tiles already cooked. Pruning keeps every key a cataloged height generator could produce (`append_generator_cook_keys`: its overview and every tile of its region, through `reachable_cook_keys`; nothing for a scatter generator), so a pruned cache stays warm. Scatter's cells are not kept: their keys depend on the exclusions, which only the scenes know, and a pruned cell is made again in hundredths of a millisecond ([costs](#costs)). A [package](#packages) holds no cook cache.

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
- **Changes.** A changed `heights` reference, a reloaded generator, or an edited one (a new version in the registry) drops the terrain's tiles and starts again; its layers are read as they are each frame. Since #1093 the terrain is drawn as it was (not collided with) until the new one is ready, its overview and every wanted tile loaded and on the device, so it never disappears for a frame; `regenerating` counts the terrains drawn this way.
- **Waiting to start.** A tile wanted while `tiles_in_flight` others generate waits for room. One the sources leave behind before it starts is dropped once it is past the loading radius, even within the hysteresis; before #1069 it was kept loading with no job, so the streamer never settled.
- **Under a cell** (#1069). `collides(low, size)` says whether bodies can stand on the terrain over a square of ground (a world's cell, of any size) now: every tile under it that a colliding terrain the streamer has found collides with there (within the activation radius) holds its height field in the physics world; farther tiles are not collided with, so they are not waited for, and a terrain whose generator is still loading is waited for everywhere. `set_held(predicate)` names tiles whose height fields stay past the radii: those under a cell still in the World. `link_terrain(cells, terrain)` sets both ([world](world.md#streaming)): a world's cells wait for the ground under them, and the ground under a cell stays until it goes, so the player and Play settle and update the terrain before the cells. In the editor, Play's terrain is updated in `update_play`, before the cells and the tick, around the play camera (the editor camera when there is none), as the player's does, and around the editor camera too while the Scene view shows it, so the cells near the play camera are never held while the Scene view looks elsewhere; the viewport updates terrain only for the authored scene.
- **Loading screens.** `settle` updates, waiting for jobs and ignoring the frame budget, until nothing more would change around the sources. `clear(physics)` takes everything out, height fields first. Destroying the streamer cancels its jobs and waits for them.
- `terrains()` gives what the renderer and picking read; `height_at(x, z)` the loaded height between samples, on the triangles the finest level draws. `stats()` counts terrains, tiles by state, tiles drawn, generated, and read from the cache, activations, uploads, height fields, CPU and GPU bytes held, and the last and longest update and tile job. Tiles are not yet a [residency](assets.md#residency) category: the streamer's own counts are their bytes.

Terrain streams by its own radius rather than as part of each cell's activation, because the finest levels need tiles out to 874 m, beyond the cells' 640 m loading radius.

## Scatter

Scatter (#1093) places items (rocks, bushes, trees) over a terrain, cell by cell, as [#1060's record](architecture/world-scale-decision.md#generators-seeded-cooked-per-cell-stable-ids) chose: one candidate a square, rules that only remove candidates, and IDs that never depend on the rules. Its items are entities, made where they are used and never saved; what is authored about them is kept as [overrides](#overrides). On 11 October 2026 the project owner decided four questions: an override is the whole entity, marked; an override whose item is no longer made is kept; exclusion volumes are entities; and scatter stands on terrain only.

### The scatter generator

A generator of kind `scatter` ([above](#generator-assets)), with its seed, base, and region, and these ([properties](properties.md#generators)):

| ID / name | Type | Range | Means |
| --- | --- | --- | --- |
| 12 `heights` | generator reference | | A `heightfield.noise` generator: the terrain the items stand on. None, or a cell outside its region, is a flat ground at `base`. |
| 13 `spacing` | scalar (m) | 0.125 to 64; default 4 | The squares' size: a power of two that divides the 128 m cell (`scatter_spacing_valid`). |
| 14 `density` | scalar | 0 to 1; default 0.6 | The fraction of candidates kept, before the other rules. |
| 15 `max_slope` | scalar (deg) | 0 to 90; default 30 | The steepest ground an item stands on. |
| 16 `min_height`, 17 `max_height` | scalar (m) | −10,000 to 10,000 | The band of ground heights items stand in. |
| 18 `min_scale`, 19 `max_scale` | scalar | 0.01 to 100; default 0.8 and 1.2 | Each item's uniform scale, between them. |
| 20 `random_yaw` | boolean | default true | Each item turned about +y by a hashed angle. |
| 21–24 `mesh0`…`mesh3` | mesh reference | | Up to four variants. |
| 25–28 `material0`…`material3` | material reference | | Each variant's material. |
| 29–32 `weight0`…`weight3` | scalar | 0 to 1,000; default 1 | How often each is chosen, relative to the others; a variant without a mesh is never chosen, and a generator with no meshes places nothing. |
| 33–36 `collider0`…`collider3` | scalar (m) | 0 to 1,000 | A sphere collider's radius before the item's scale; 0 for none. |

The lowest height above the highest, or the smallest scale above the largest, is refused against property 17 or 19; a spacing that does not divide the cell, against 13. `scatter_rules(generator)` gives the rules scatter runs ([generator.hpp](../include/maya/assets/generator.hpp)). A `maya.generator` component ([properties](properties.md#discovery-and-identity)) runs a scatter generator in a scene or world; a second entity naming the same generator is ignored, and reported.

### Placement

`scatter_cell(generator, rules, cell, tile, exclusions, context)` makes one cell's items, in square order ([scatter.cpp](../src/maya/procedural/scatter.cpp)):

- **Squares** a spacing wide tile the world, aligned to it: the cell's square (sx, sz) is the world's (cell × squares + sx, cell × squares + sz), so cells share no square and need nothing from their neighbours.
- **A candidate** in each square comes from one hash of the seed and the square. Its point is at (g + 0.25 + 0.5 r) × spacing along x and z, in the middle half of the square, so two items are at least half a spacing apart.
- **Rules**, in order: the density (the hash against it), the height band, the slope (only on a height tile), and the [exclusions](#exclusions). The ground's height and slope are read on the triangles terrain draws (`tile_height_at`, `tile_slope_at`; each square split from (0, 0) to (1, 1)), so items stand on the drawn surface; `TerrainStreamer::height_at` reads the same.
- **What survives** picks its variant by weight, its yaw (a hashed turn, or none), and its scale, each from the same hash.
- **Terrain only**, as the owner decided: items stand on a height-field generator's tiles or a flat base height. Other surfaces (meshes, authored ground) are a follow-up.

**Identity.** An item's ID is `scatter_item_id(generator, gx, gz)`: the generator's asset ID and the square's place in the world, never the rules or the seed. Rules only remove candidates, so changing a rule keeps the items that survive with their IDs and places; a new seed moves the points and keeps the IDs. Neighbouring cells, and different generators, never share an ID.

### Exclusions

A `maya.scatter_exclusion` component ([properties](properties.md#discovery-and-identity)) makes an entity a volume that scatter leaves clear, as the owner decided: a box (its half extents, in the entity's frame, so its rotation and scale apply) or a sphere (its radius times the frame's largest axis scale), at the entity's world pose. It clears one generator, or every one when it names none. Exclusions are authored entities like any other: in a [world](world.md) they are in the persistent part, so every cell sees them, loaded or not. A cell is reached where the volume's bounding sphere meets its square, and only those exclusions are in its key: moving, resizing, adding, or removing one regenerates the generator's cells, and those it does not reach come back unchanged from the cook cache.

### Items

`scatter_entity` makes an item's entity, with the item's ID ([scatter_streamer.cpp](../src/maya/streaming/scatter_streamer.cpp)):

- `maya.name`: the generator file's name ("rocks");
- `maya.transform`: the item's position, its yaw about +y, and its uniform scale;
- `maya.mesh_renderer`: its variant's mesh and material;
- `maya.collider`: a sphere of the variant's radius, when it has one, static in a physics world;
- `maya.generated`: the marker naming the generator ([overrides](#overrides)).

### Cooking and the streamer

Each cell's items are cooked into the [cook cache](assets.md#cook-cache) as `scatter-cell` entries (`scatter_cook_version` 1; "SCT1", then 49 bytes an item; `decode_scatter_items` refuses malformed bytes). The key (`scatter_cell_cook_key`) covers the generator's ID and scatter parameters, the cell's height tile key, and the exclusions that reach the cell, so a cell is made again only when one of these changes; a cell's job reads its height tile from the cache too, or generates it there, as terrain does.

`ScatterStreamer` keeps the items of every scatter generator in a World near its sources (`set_sources`), as [terrain](#streaming) keeps tiles:

- **Cells.** A generator's cells within the activation radius of a source are made on background jobs, nearest first, at most `cells_in_flight` (8) at once, and stay until beyond the radius plus the hysteresis (`StreamingSettings`).
- **A frame** (`update`) finds the World's generators and exclusions, applies finished jobs, then puts items in and takes them out within `frame_budget` (1 ms). It plans from the costs it measures: staging in chunks of `chunk_items` (256), revealing, removing. However small the budget, the frame's first step is allowed, so it always progresses. A cell's items are staged invisible over as many frames as they take and revealed together (`StageGroup`), so a cell appears all at once.
- **Ready.** `set_ready(cell)` names the cells whose authored content is in (in the player, Play, and the editor, a world's active cells, and any cell the world does not have; every cell in a plain scene). A cell's items go in only once it is ready, and go out as soon as it is not, so an override and the item it stands for are never in at once.
- **Changes.** A changed generator, heights, or exclusions remakes every cell, cancelling superseded jobs. A cell's items stay until its new ones are made; then the old ones are taken out and the new ones put in over the next frames (survivors with the same IDs), so a large cell can be empty for a few frames between. `stats().regenerating` counts cells showing an older version.
- **Loading screens.** `settle` updates until the cells around the sources are made and in. `clear(sink)` takes every item out; destroying the streamer cancels its jobs and leaves its items.
- **Reports** (`take_reports`, each once): a generator that cannot load, heights that are not a height-field generator, two entities running one generator, a sink's errors, and orphans.
- **Stats** (`stats()`): generators; cells loading and active; items in; cells generated and read from the cache; regenerations and cancelled jobs; items skipped and orphans in the cells in; cells regenerating and waiting (0 once settled); and the last and longest frame, cell job, and a cell's time going in. `reset_peaks` clears the longest.

**Sinks** put items where they go (`ScatterSink`: stage, reveal, discard, remove, exists). `WorldScatterSink` puts them into a World and, on reveal, their colliders into its physics world as static bodies ([physics](physics.md)); removing takes the bodies out with the items. The editor's `EditorScatterSink` puts them into the open scene as transient entities ([generated content](editor.md#generated-content)) and removes only items still transient.

### Overrides

An item is authored by authoring its whole entity, as the owner decided: the entity keeps the item's ID and its `maya.generated` marker, and is saved with the scene (or its cell, in a world) like any other. Wherever an entity with an item's ID is already in, the streamer does not make that item (`skipped`):

- **An override** is an item edited: moved, scaled, given another mesh. It is drawn and collided with as authored.
- **A removed item** is a marker with `removed` set (keeping the item's name and transform, nothing drawn): the item is not made there.
- **A baked item** is an ordinary entity with the item's ID and no marker: the generator skips it too, and it is no longer the generator's.
- **An orphan** is an override (or a removed marker) of a generator whose item that cell no longer makes, after a rule, the seed, or an exclusion changed. It is kept as authored, as the owner decided, and reported once; `orphans()` lists them.

The streamer tracks the items it made (`made`). `release(id)` gives one up (it was edited into an override, or baked), `adopt(id)` takes one back (the edit was undone, and its cell still makes it), and `restore(id)` puts a cell's items in again (a removed marker was deleted).

### The player, Play, and the benchmark

- **The player** scatters a scene's generators after its terrain, into its World and physics (`WorldScatterSink`), waiting for each cell's authored content in a world, settled before the first frame ("[Player] scattered N items in M cells"), updated each frame around the camera, and cleared at stop. A packaged player has no cook cache and generates every cell. Recording and replay are refused for a scene with generators ("recording and replaying a scene with generators are not supported yet"): items join physics as they stream, not tick by tick.
- **Play** in the editor does the same from the scene as edited, its overrides included: a world's items settle after its cells, a plain scene's before the first tick; recording and replay are refused the same way; Stop takes the items and colliders out before the cells.
- **The benchmark** streams a scene's generators as the player does, waits for them in its streamed loads, and records `scatter_items`, `scatter_cells`, `scatter_generated`, `scatter_cached`, and the longest frame, cell job, and activation ([performance](performance.md#results)).

### The sample

[terrain.scene](../samples/basic_scene/assets/terrain.scene) runs [rocks.generator](../samples/basic_scene/assets/terrain/rocks.generator) from the entity "Rocks", over the hills' terrain: an 8 m spacing, density 0.3, slopes to 28°, scales 0.6 to 1.6, three spheres (collider 1 m) for every cube (0.6 m), in the rock material. "Clearing", a 20 m sphere at the origin, keeps the middle clear.

### Costs

Release on the M4 Pro reference machine, 11 October 2026, W1's rules (4 m spacing, density 0.6, 30°, a 20 m clearing), 64 cells. Observations, not budgets:

| | Measured |
| --- | --- |
| `scatter_cell` from a height tile | mean 0.031 ms, median 0.029 ms, max 0.071 ms |
| Items per cell | mean 549, max 620 (the record: about 528) |
| A chunk of 256 staged into a World, the worst per cell | mean 0.159 ms, max 0.356 ms |
| A cell revealed | 0.003 ms |
| A cell removed | mean 0.075 ms, max 0.096 ms |

A cell goes in well within #1064's 1 ms a frame. Height tiles are shared with terrain through the cook cache; one not cached is generated in 4.2 ms, as #1068 [measured](#measurements). The record's 0.48 ms a cell included sampling noise for each item.

## Editing

In the editor ([editor](editor.md#generators)):

- **Drawing.** Terrain in the open scene is drawn around the editor camera, in the Scene and Game views, through the editor's cook cache. It is drawn in the shadow maps and the [debug views](renderer.md#debug-views); the `lod` view tints each patch by its level.
- **Picking.** A click on the ground selects the terrain, and the pivot finds it (a ray marched over the loaded tiles to a centimetre).
- **Generators** are edited in the Inspector: folded under a selected terrain's component, or on their own when clicked in the Assets panel's GENERATORS column. Each change is one undo step in the scene's history (a drag is one), published to the registry, so the terrain regenerates at once; ⌘S writes the file. Undo, discarding, reloading, and watched files work as for [materials](editor.md#materials).
- **Play** draws and collides with the terrain around the play camera, settled before the first tick; Stop takes the height fields out with the play session's physics.
- **Scatter** is edited the same way: its items are a preview in the scene, outside its history, and are edited, removed, and baked as [generated content](editor.md#generated-content).
- **Recording and replay** are refused for scenes with terrain (and, since #1093, with generators), in the editor and the player: tiles join physics frame by frame as they stream, not tick by tick, so a recording could not replay.

## Packages

A [package](projects.md#packages) holds a generator as its authored file, never its output: the packaged player generates tiles, the overview, and scatter's cells where it runs (it has no cook cache), as the record chose, since results are not promised identical across machines. It holds what a generator names too, followed from the scenes as their references are: a scatter generator's heights, meshes, and materials, and those materials' textures (#1069; before, a mesh only a generator named was left out, and the packaged player drew none of its items).

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

- [procedural_tests.cpp](../tests/procedural_tests.cpp) (`maya_procedural_tests`, labels `cpu;procedural`): noise depending only on the seed, parameters, and position; tiles identical on one thread and with rows across the job system, byte for byte, with neighbours' shared rows and columns of heights and normals equal; a cancelled tile; the overview equal to tiles at every 16th metre; cooked tiles and overviews read back exactly and damaged ones refused; packing keeping every height's bits; generator files round-tripping every property and refusing what this build cannot generate; and generator properties edited atomically; and (`[scatter]`) a cell's items the same on jobs in parallel, byte for byte; every rule holding (in the cell, on the drawn ground, at least half a spacing apart, within the slope, outside a sphere and a rotated box, a weightless variant never chosen, scales in range), and a flat ground without heights; rules only removing items, the survivors keeping their IDs and places; a new seed keeping the IDs; no ID shared by neighbouring cells or by two generators; cooked items read back exactly and damaged ones refused; spacings checked; a scatter generator file holding its kind's properties alone, reading back exactly, and giving its rules; and (#1069) its heights, meshes, and materials checked against the catalog when validated, and not when its file is read.
- [terrain_tests.cpp](../tests/terrain_tests.cpp) (in `maya_streaming_tests`, tag `[terrain]`): tiles loading within the loading radius and drawn, activating within the activation radius, joining the physics world as height fields of the terrain entity, leaving it as the source moves away and when the terrain is removed, joining a physics world given after they loaded, coming from the cook cache the second time with the same bytes and changing with their generator, a bounded number activated a frame, nearest first, a streamer destroyed while tiles generate, (#1093) a regenerating terrain drawn as it was until its new tiles are ready, and (#1069) tiles left behind before they started dropped; a world's cell waiting, loaded, until the terrain under it collides, settling the cells not waiting on it, and the height field under it kept while it is in after the source leaves; and a 256 m cell waiting for each of the four tiles under it; and (`[cdlod]`) patches covering an 11 × 7-cell region once from 60 random eyes, neighbours a level apart at most, the finer side of every boundary fully morphed and the coarser not begun.
- [scatter_tests.cpp](../tests/scatter_tests.cpp) (in `maya_streaming_tests`, tag `[scatter]`): items streaming in as entities around a source, the same ones from the cook cache, and leaving as it moves away; items an override, a removed marker, or a baked entity stands for not made, and overrides of items no longer made reported once as orphans; a changed generator remaking its items, the survivors keeping their IDs, superseded jobs cancelled; exclusions clearing items and remaking what they reach when moved; items waiting for their cell's authored content, going when it goes, and colliding in a physics world; and progress under any frame budget, each cell shown at once.
- [physics_tests.cpp](../tests/physics_tests.cpp) (`[terrain]`): rays landing within a centimetre of the samples and naming the owner, a swept sphere stopping on the ground, a body coming to rest on it, removal in a batch, and refused descriptions.
- [terrain_gpu_tests.cpp](../tests/terrain_gpu_tests.cpp) (in `maya_tests`, `[rhi][terrain]`, Metal): a 6 × 6-cell terrain at W1's place cleared to magenta, seen from 3 m to 220 m above the ground at three offsets: no magenta pixel anywhere (no cracks between tiles or levels), grass showing, under 5% of pixels dark (normals and shading work), and terrain drawn into the shadow maps; the same terrain in every debug view from `luminance` to `texels`, with no holes and an image of its own; and (`[visual]`) four views compared with the owner-approved references in `tests/references/terrain/` (from the ground, across the hills, and from 300 m above, where the tiles meet the overview, and that view's `lod` view), and (#1093, `[scatter]`) the hills with cubes scattered over them by a scatter generator (`terrain-scatter.png`, approved on 11 October 2026), within the references' usual tolerance (each channel within 6 on 99.5% of pixels; `MAYA_BLESS_REFERENCES=1` renders them again, for approval).
- [editor_terrain_tests.cpp](../tests/editor_terrain_tests.cpp) (`[editor][terrain]`): the sample's terrain drawn in the editor with no height fields, picked by a click, and collided with in Play (the boulder rests on it), its height fields gone at Stop; and a generator edited in the Inspector regenerating the terrain, undone in the scene's history, and saved with ⌘S.
- [editor_scatter_tests.cpp](../tests/editor_scatter_tests.cpp) (`[editor][scatter]`): see [the editor's tests](editor.md#tests).
- [benchmark_tests.cpp](../tests/benchmark_tests.cpp) (`[benchmark][terrain]`): the terrain manifest's scene workload streaming the terrain and its rocks, and counting them.
- [scene_tests.cpp](../tests/scene_tests.cpp) loads the terrain scene; [package_tests.cpp](../tests/package_tests.cpp) packages generator files as authored text, with (#1069) the meshes, materials, and heights only a scatter generator names, and W1 ([acceptance](acceptance.md#w1)), whose views stream and draw from its package as from its sources.
