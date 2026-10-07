# World-scale stack

Decided 7 October 2026 for [ISSUE-1060](https://work.rezee.app/kash/issues/1060), the first issue of the [world scale milestone](https://work.rezee.app/kash/issues/1059), under [DOC-58](https://work.rezee.app/kash/docs/58). The measurements are from prototypes in this build, on the M4 Pro reference machine (10 performance and 4 efficiency cores) in a Release build, with the thermal state nominal.

| Decision | Choice | Instead of |
| --- | --- | --- |
| Job system | **A Maya-owned pool in two tiers**: frame workers at user-interactive quality of service, background workers at utility; Jolt runs on the frame tier through a `JPH::JobSystem` adapter | enkiTS 1.12, Taskflow 4.1.0, Grand Central Dispatch; one tier with reserved workers |
| Coordinates | **Double-precision world translations**; rotation, scale, mesh data, and everything on the GPU stay float; **Jolt built with `JPH_DOUBLE_PRECISION`**; **camera-relative rendering**. Chosen by the project owner | Float positions within ±4 km; float positions with origin rebasing |
| Cells | **A uniform 128 m grid**; a world asset holds the grid, an always-loaded persistent part, and one cell scene per occupied cell, authored as scene text and **cooked to a packed binary** | A nested grid; one partitioned world file |
| Activation | **1 ms of owner-thread commits a frame** (about 2,000 entities), the cell published to systems in one final commit; each cell's bodies added and removed **in Jolt batches** | Whole cells in one frame |
| Spatial index | **A loose grid** of 32 m cells within each streaming cell, with a list for large objects | A dynamic bounding-volume tree; a loose octree |
| Mesh LOD | **meshoptimizer v1.3 at cook time**: chains of halving levels, simplified with normals and texture coordinates, **permissive across attribute seams** and pruning tiny parts; levels chosen by projected error under a pixel, with hysteresis | glTF `MSFT_lod` (a later importer seam; no content uses it) |
| Terrain | **Height tiles of 129 × 129 samples (1 m) per cell**, drawn with **CDLOD**, colliding through **Jolt height fields**; a 16 m overview for terrain beyond the loading radius | Geomipmapping; 257 samples (0.5 m) |
| Generators | **A generator asset** (kind, version, seed, parameters, region), cooked per cell; **item IDs from the generator and the item's candidate square**, so changed rules only remove or add items; Maya's own seeded gradient noise | A noise library; IDs by item order |
| The W1 reference world | **A 4 km × 4 km rocky highland** placed 5.8–11.6 km from the origin, built from noise terrain, three Poly Haven scatter sets, and R1's models, with the S1 traversal and a soak cycle, **approved by the project owner** | Trees (2.3–17 million triangles each at the source) |

## The prototypes

The decisions were made from prototypes built behind `MAYA_BUILD_PROTOTYPES` (off by default), in [prototypes/world](../../prototypes/world). With `BUILD_TESTING`, CTest runs them under the `prototype` label, one at a time, as each times work on every core. Each prints its measurements, reports what the decision relies on as `UNEXPECTED`, and fails on it. Mutation checks confirmed that: scatter IDs that depend on the rules, a loose grid without slack, and a background tier at the frame tier's quality of service each fail their prototype.

| Target (CTest) | What it does |
| --- | --- |
| `maya_jobs_prototype` (`maya_prototype_jobs`) | Runs four job systems through one adapter: spawning 200,000 empty jobs, a parallel-for over 1,000,000 items, 96 coarse jobs, urgent jobs behind 1,200 background ones, cancelling 4,000 jobs, and Jolt stepping a 5,000-box pile, alone and while background work fills every worker. |
| `maya_precision_prototype`, `maya_precision_prototype_double` | The same source against single- and double-precision Jolt: float spacing, rendering error (world-space and camera-relative), picking error, and physics at origin offsets from 0 to 10 km; rebasing cost; step cost. |
| `maya_cells_prototype` | Cells of 500–8,000 entities in Maya's scene text and in a packed binary; activating them into a World of 100,000 in slices; commits against World size; Jolt batch adds and removes. |
| `maya_visibility_prototype` | Brute force, a loose grid, a dynamic tree, and a loose octree over 100,000 moving boxes, with seven timed views and 1,100 correctness volumes; meshoptimizer LOD chains for R1's models and W1's scatter. |
| `maya_terrain_prototype` | Noise height tiles (time, determinism, seams), Jolt height fields (build, memory, rays, rest), scatter (spacing, identity under changed rules), and a 4 km terrain on Metal with geomipmapping and CDLOD. |

The visibility prototype reads models from [fetch_render_samples.sh](../../tools/fetch_render_samples.sh) and [fetch_world_samples.sh](../../tools/fetch_world_samples.sh) where they were fetched, and skips its LOD half otherwise. The double-precision Jolt is the engine's Jolt sources, definitions, and options compiled a second time; the two are never linked into one executable.

## Jobs: a two-tier pool

| Candidate | Spawn (ns a job) | Parallel-for (× one thread) | Urgent job start, P50 / P99 (ms) | Jolt step, alone / with background work (ms) |
| --- | --- | --- | --- | --- |
| Maya pool, one tier | 2,538 | 9.5 | 0.17 / 1.91 | 1.33 / 3.75 |
| Maya pool, 4 workers reserved for frame work | 2,544 | 9.2 | 0.01 / 0.03 | 1.36 / 2.42 |
| **Maya pool, two tiers** | 2,396 | 9.4 | 0.00 / 0.01 | **1.32 / 1.30** |
| enkiTS 1.12 | 10,822 | 8.5 | 0.02 / 0.41 | 1.76 / 4.10 |
| Taskflow 4.1.0 | 157 | 9.9 | 66 / 158 | 1.33 / 5.28 |
| GCD | 91 | 10.1 | 0.00 / 0.02 | 2.05 / 2.23 |
| Jolt's own pool (Maya's work in a second pool) | — | — | — | 1.27 / 2.99 |

- **Background work must not slow frame work.** In a cooperative pool, a worker that has started a 1 ms decode cannot take physics work until the decode ends. One-tier pools slowed Jolt 2.8× under streaming load, and reserving workers helped only partly. Two tiers of threads at different quality of service let macOS preempt the background threads, and Jolt stayed at its idle time. With both tiers at the same quality of service (a mutation check), Jolt slowed to 3.0 ms and urgent jobs waited 2.4 ms: the quality of service is the mechanism.
- **Taskflow has no priorities.** Urgent jobs waited behind the whole background queue (P99 85–185 ms over runs).
- **enkiTS runs tasks inline when a pipe is full.** Its per-thread pipes hold 256 tasks; a burst of submissions from the owner thread ran tasks on the owner thread (all 4,000 cancellable jobs ran before the cancel). A burst of streaming requests would decode on the main thread.
- **GCD** has the cheapest spawn and the right priorities, but Jolt ran 60% slower on it, it is Apple-only, and its pool is shared with the system and not inspectable.
- **The Maya pool's spawn cost** (about 2.4 µs) comes from one lock around both queues; [#1061](https://work.rezee.app/kash/issues/1061) replaces it with lock-free or per-worker queues. The parallel-for and coarse results already match the libraries.

Contracts for [#1061](https://work.rezee.app/kash/issues/1061) and the scheduling contract:
- One job system per process. The frame tier has a worker for each core but one, at `QOS_CLASS_USER_INTERACTIVE`; it runs physics, animation, extraction, and parallel-fors. The background tier has as many workers at `QOS_CLASS_UTILITY`; it runs file reads, decoding, cooking, and generation. On other platforms the tiers become thread priorities.
- Background jobs stay short (a few milliseconds) and check their cancellation token before each step, so cancellation is prompt.
- Jobs never touch the World, never mutate the asset registry, and never encode GPU commands. Their results return through an owner-thread completion queue, drained in the finalize phase under a per-frame budget. Completions carry the owner's lifetime token and a request generation; stale ones are discarded.
- Shutdown cancels both tiers and joins them before the registry, the World, or the device is destroyed.
- Jolt uses a `JPH::JobSystemWithBarrier` adapter on the frame tier (1.32 ms a step against 1.27 ms on its own pool).

## Coordinates: double positions

| Offset | Float spacing | Rendering error, object 5 m / 0.5 m away: float world space | Camera-relative | Picking error | Rolling sphere after 25 m, single / double Jolt | Resting stack, single / double |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | — | 0.000 / 0.001 px | 0.0001 px | 0.002 mm | 0 / 0 mm | rests / rests |
| 1 km | 0.06 mm | 0.025 / 0.22 px | 0.0001 px | 0.03 mm | 0.15 / 0 mm | still moving at 8 s / rests |
| 4 km | 0.24 mm | 0.095 / 0.94 px | 0.0001 px | 0.11 mm | 0.23 / 0 mm | rests / rests |
| 10 km | 0.98 mm | 0.256 / 2.74 px | 0.0001 px | 0.44 mm | 1.43 / 0 mm | rests / rests |

- **Camera-relative rendering is required whatever the stored precision.** In float world space, a hand or tool 0.5 m from the camera is off by almost a pixel at 4 km and 2.7 px at 10 km, and the error changes as the camera moves (shimmer). Subtracting the camera position in double before converting to float keeps every case at 0.0001 px.
- **Double-precision Jolt** matched the origin run everywhere (worst 0.03 mm). Its step took 1.28–1.35 ms against 1.18–1.28 ms in single precision (0–14% more over runs), and a body is 160 bytes instead of 128.
- **Single-precision Jolt** drifted 1.4–1.5 mm per 25 m of rolling at 8–10 km. At 1 km a balanced stack had not come to rest by 8 s; a balanced stack is chaotic, so this is a symptom, not a proportional measure.
- **Rebasing** works for physics (a stack rebased by 4 km every 2 s stayed at rest), but moving every body costs 5.9 ms for 50,000 bodies, and every system that holds a position (scripts, recordings, the editor, animation targets, cached queries) would have to handle the shift.

The project owner chose double-precision positions. Contracts for [#1065](https://work.rezee.app/kash/issues/1065):
- `TransformComponent.translation` becomes double for every entity. Rotation and scale stay float. World transforms carry a double translation and a float rotation and scale.
- Extraction subtracts the view's camera position from each world translation in double, then converts to float. Model matrices on the GPU are camera-relative, and the view matrix has no translation. Shadow cascades, spot maps, culling volumes, and debug lines use the same camera-relative space.
- Jolt is built with `JPH_DOUBLE_PRECISION`; positions cross the physics boundary as doubles and velocities as floats.
- Skinning, joint palettes, and mesh data stay in local float space.
- Scene files gain a version: translations are written so they read back exactly, and earlier files migrate.
- The scripting boundary keeps position precision; Luau's `vector` is float, so #1065 decides how scripts see positions.
- No maximum extent is claimed yet: correctness is checked to 10 km here and across W1 (11.6 km); #1065 extends the sweep before a larger range is promised.

## Cells: a 128 m grid, cooked to binary

| Cell entities | Scene text | Packed binary | Text parse | Binary decode + validate |
| --- | --- | --- | --- | --- |
| 500 | 163 KiB | 55 KiB | 1.4 ms | 0.14 ms |
| 2,000 | 651 KiB | 220 KiB | 6.2 ms | 0.50 ms |
| 8,000 | 2.6 MiB | 0.9 MiB | 29.2 ms | 2.0 ms |

Activating entities into a World of 100,000 costs 0.51–0.57 µs an entity on the owner thread (0.5 ms for 1,000, 2.1 ms for 4,000), and removing them 0.12 µs. A 1,000-entity commit costs the same in a World of 10,000 and of 400,000. Jolt adds 1,000 static colliders to a world of 50,000 in 0.035 ms in a batch (0.063 ms one at a time), and removes them in 0.013 ms (0.026 ms).

Contracts for [#1064](https://work.rezee.app/kash/issues/1064):
- The grid is uniform, 128 m in x and z; a cell is a column. A cell's index is the floor of a position divided by 128, as two 32-bit integers.
- A world asset holds the grid, a persistent scene (cameras, lights, environment, terrain settings, generators), and one cell scene for each occupied cell. Cell scenes are authored as scene text and cooked to a packed binary in the cook cache, so reading a cell costs decoding, not parsing.
- An entity belongs to the cell of its root's translation when the world is saved; a whole subtree moves with its root, and hierarchies across cells are refused.
- Streaming radii are project settings; W1 uses 640 m to load, 384 m to activate, and 64 m of hysteresis.
- Activation spends at most 1 ms a frame on owner-thread commits. A cell larger than one frame's budget is committed over several frames with its entities excluded from systems, extraction, and queries, and is published in one final commit. Each cell's bodies join and leave Jolt in batches.
- The scheduling contract's state machine, tokens, and generations apply as written in [Future streaming boundary](runtime-world-contracts.md#future-streaming-boundary).

## Visibility: a loose grid

| Index | Build (ms) | Update, 10,000 moving (ms) | Frame of queries (ms) |
| --- | --- | --- | --- |
| Brute force | 1.1 | 0.04 | 5.70 |
| **Loose grid, 32 m** | 2.5 | 0.10 | **0.14** |
| Dynamic tree | 42.9 | 0.25 | 0.54 |
| Loose octree | 10.4 | 0.32 | 1.34 |

100,000 boxes over 2 km × 2 km; a frame is the camera (1,500 m and 250 m), four shadow cascades, and a spot light, which see 25,451, 1,581, 12, 221, 1,743, 16,947, and 1 boxes. Every index returned exactly brute force's boxes in these views, in 300 random spot volumes, and in 800 boxes placed across cell lines.

Contracts for [#1066](https://work.rezee.app/kash/issues/1066): each streaming cell holds a 4 × 4 loose grid of 32 m cells, whose bounds are their footprint grown by a slack (the largest small-object half size). Objects larger than the slack go in the streaming cell's list of large objects, and persistent entities in a world list. A cell's index joins and leaves the renderer with the cell. Static entities cost nothing per frame.

## Mesh LOD: meshoptimizer at cook time

Levels simplified each from the one before, permissive across seams; error relative to the mesh's size, and the distance (in mesh sizes) beyond which it is under a pixel at 1080p and 60 degrees:

| Mesh | Triangles | Level 2 (25%) | Level 4 (6.2%) | Cook, all four levels |
| --- | --- | --- | --- | --- |
| FlightHelmet | 94,722 | 0.0044, from 4 | 0.024, from 23 | 40 ms |
| DamagedHelmet | 15,452 | 0.014, from 13 | 0.085, from 79 | 10 ms |
| ABeautifulGame | 574,528 | 0.0003, from 0 | 0.0018, from 2 | 570 ms |
| boulder_01 (W1) | 66,122 | 0.0080, from 7 | 0.033, from 31 | 58 ms |
| rock_moss_set_01 (W1) | 63,127 | 0.0097, from 9 | 0.040, from 37 | 26 ms |
| dead_tree_trunk_02 (W1) | 83,128 | 0.0028, from 3 | 0.012, from 11 | 35 ms |

Without the permissive option, simplification stalled at 80–90% of the triangles on ABeautifulGame, boulder_01, and DamagedHelmet, whose UV and normal seams split vertices into locked borders.

Contracts for #1066: a cooked mesh carries up to six halving levels, simplified with normals (weight 0.5) and texture coordinates (weight 1), `meshopt_SimplifyPermissive | meshopt_SimplifyPrune`; each level stores its error in metres. A view draws the coarsest level whose projected error is under one pixel (a project setting biases it), moving to a finer level above 1 px and to a coarser one only below 0.75 px. Shadow passes use one level coarser. Instances whose projected bounds fall under a few pixels are culled.

## Terrain: CDLOD over cell tiles

| Tile (128 m) | Generate (one thread) | Jolt height field | Ray error | Size |
| --- | --- | --- | --- | --- |
| 65 samples (2 m) | 1.1 ms | 0.07 ms | 17 mm | 9 KiB |
| **129 samples (1 m)** | **4.2 ms** | **0.20 ms** | **9 mm** | **33 KiB** |
| 257 samples (0.5 m) | 16.0 ms | 0.87 ms | 4 mm | 129 KiB |

Tiles are byte-identical whatever the thread count, and neighbours' shared edges are equal. A box dropped on each tile came to rest on it.

A 4 km × 4 km terrain at 2 m on Metal, 1920 × 1080:

| Viewpoint | Geomipmapping: patches, triangles, CPU, GPU | CDLOD: patches, triangles, CPU, GPU |
| --- | --- | --- |
| On the ground | 1,081, 38,600, 0.053 ms, 0.12 ms | 55, 112,640, 0.006 ms, 0.09 ms |
| A hilltop | 2,393, 42,694, 0.089 ms, 0.05 ms | 58, 118,784, 0.007 ms, 0.07 ms |
| 400 m up | 3,298, 19,490, 0.067 ms, 0.06 ms | 24, 49,152, 0.003 ms, 0.06 ms |

Both cost under 0.12 ms of GPU time. CDLOD selects in a tenth of the CPU time or less, draws 20–140 times fewer patches, and morphs between levels, so it needs no stitching or skirts and does not pop.

Contracts for [#1068](https://work.rezee.app/kash/issues/1068): a terrain's heights are cooked per cell as 129 × 129 samples sharing their edge row and column, stored on the GPU as 16-bit values over the tile's height range (steps under 5 mm even across W1's whole relief). Each cell is a CDLOD quadtree of 32 × 32-quad patches from 128 m down to 32 m (1 m quads), morphing over the last 30% of each range. Beyond the loading radius, terrain is drawn from an always-resident overview at 16 m. Normals are taken from the heights per pixel. A terrain material blends up to four layers by slope and height. Collision uses a Jolt `HeightFieldShape` per tile (block size 4, 8 bits a sample), which joins and leaves with the cell.

## Generators: seeded, cooked per cell, stable IDs

One 128 m cell of scatter at 4 m spacing: 1,024 candidates, 528 items, 0.48 ms; the closest pair 2.13 m apart (2 m required). Changing the slope limit from 30° to 20° kept 389 items, the density from 0.6 to 0.3 kept 246, and the clearing radius from 20 m to 40 m kept 390, all with unchanged IDs and positions and no new items. Neighbouring cells produced no shared IDs.

Contracts for #1068:
- A generator asset holds its kind (`heightfield.noise` or `scatter`), its algorithm version, its seed, its parameters, and its region (the world or listed cells).
- Output is cooked per cell into the cook cache, keyed by all of these and by the cook keys of what it reads (scatter reads the height tiles). It is regenerated only when an input changes.
- Scatter places one candidate in each spacing-sized square, at a hashed point at least half the spacing from the square's edges. The item's ID derives from the generator's ID and the square's global coordinates, never from the rules; rules only remove candidates.
- Noise is Maya's own gradient noise on an integer lattice hashed with the seed (fBm with a domain warp), so results do not change with a library version. Results are identical across thread counts; they are cooked on the machine that uses them, not promised identical across compilers or platforms.
- Authored changes to generated items are kept by item ID, and reported when their item no longer exists.

## Pins and licenses

| Dependency | Version | License | Use |
| --- | --- | --- | --- |
| meshoptimizer | v1.3 | MIT | Chosen: LOD at cook time (#1066) |
| Jolt Physics | v5.6.0 with `DOUBLE_PRECISION` | MIT | Chosen: double-precision physics (#1065) |
| enkiTS | v1.12 | zlib | Prototype only |
| Taskflow | v4.1.0 | MIT | Prototype only |
| W1's content | Poly Haven, 2k, by SHA-256 | CC0 | [fetch_world_samples.sh](../../tools/fetch_world_samples.sh), into `build/world-samples`; not committed |

## W1: the reference world

Recipe version 1, approved by the project owner on 7 October 2026.

- **Extent.** 4 km × 4 km at x and z from 4,096 m to 8,192 m, so the world lies 5.8–11.6 km from the origin and exercises the coordinates.
- **Cells.** Uniform 128 m: 32 × 32 = 1,024 cells. Load at 640 m, activate at 384 m, 64 m of hysteresis. Terrain beyond the loading radius from a 16 m overview, always resident.
- **Terrain.** A noise height field (seed 990, 6 octaves, 900 m first wavelength, 140 m amplitude, 120 m warp), 129 samples a cell (1 m), CDLOD. Two layers by slope: `forest_ground_04` and `rock_face_03`.
- **Scatter**, with cooked LOD chains and distance culling:

  | Set | Models | Spacing | Rules |
  | --- | --- | --- | --- |
  | Boulders | `boulder_01`, `namaqualand_boulder_02` | 12 m | slope ≤ 35° |
  | Rocks | `rock_moss_set_01`, `rock_moss_set_02` | 4 m | density 0.4 |
  | Deadwood | `tree_stump_01`, `dead_tree_trunk_02` | 10 m | density 0.3 |

- **Landmarks** (R1's content): a clearing with ABeautifulGame and FlightHelmet; CesiumMan walking a loop across three cells.
- **Light.** A sun, and `kloofendal_48d_partly_cloudy_puresky` at 2k for the environment; shadows on.
- **Cross-cell reference.** A watch camera in one cell that targets CesiumMan in another.
- **Physics.** 400 resting rocks near the path, 20 boulders that roll when their cell activates, and the terrain's height-field colliders.
- **S1, the traversal.** A fixed 3.2 km path over 14,400 ticks: 120 s on foot at 8 m/s, turning back across a cell edge three times, then a 40 s flyover at 60 m/s. 5% of loads are delayed by 500 ms, and two loads are cancelled by turning back.
- **The soak.** 20 cycles of: load, 60 s of S1, regenerate the rocks in 4 cells with a new seed, reset, unload (about 30 minutes).
- **Views.** The clearing, a hillside, a 2 km ridge vista, the flyover, and a cell edge.
- **Visual bar.** Reference images of the five views approved by the project owner; no visible LOD popping above the one-pixel error; no cracks in the terrain; shadows on terrain and scatter.
- **Budgets to measure against** (M4 Pro, 1080p, Release), approved in [#1069](https://work.rezee.app/kash/issues/1069) after its baselines: GPU P95 ≤ 8 ms; CPU work P95 ≤ 4 ms; the longest activation frame no more than 2 ms over a steady frame; resident memory ≤ 1.5 GiB; warm first frame ≤ 3 s.
- **No trees in version 1.** Poly Haven's trees are 2.3–17 million triangles each, made for offline rendering; forests need impostors or foliage LODs, which this milestone leaves out.

| Content | Source | License |
| --- | --- | --- |
| `boulder_01`, `namaqualand_boulder_02`, `rock_moss_set_01`, `rock_moss_set_02`, `tree_stump_01`, `dead_tree_trunk_02` (glTF, 2k textures) | [Poly Haven](https://polyhaven.com/models) | CC0 |
| `forest_ground_04`, `rock_face_03` (2k color, normal, and ARM maps) | [Poly Haven](https://polyhaven.com/textures) | CC0 |
| `kloofendal_48d_partly_cloudy_puresky` (2k HDR) | [Poly Haven](https://polyhaven.com/hdris) | CC0 |
| ABeautifulGame, FlightHelmet, CesiumMan | glTF-Sample-Assets, through [fetch_render_samples.sh](../../tools/fetch_render_samples.sh) | Per model, as for R1 |

S1 and the soak become benchmark manifests with W1 in #1069, following the [workload rules](performance-baseline.md#repeatable-workloads).

## What the milestone's issues take from this

| Issue | Decided here |
| --- | --- |
| [#1061](https://work.rezee.app/kash/issues/1061) jobs | The two-tier pool, its rules, the Jolt adapter, and a queue that removes the single lock |
| [#1062](https://work.rezee.app/kash/issues/1062) loading | Decoding on the background tier; completions with tokens and generations, drained under a budget |
| [#1063](https://work.rezee.app/kash/issues/1063) residency | Cooked cells and height tiles as residency categories |
| [#1064](https://work.rezee.app/kash/issues/1064) cells | The 128 m grid, the world asset, binary cooked cells, the 1 ms activation budget, and Jolt batches |
| [#1065](https://work.rezee.app/kash/issues/1065) precision | Double translations, double-precision Jolt, camera-relative rendering |
| [#1066](https://work.rezee.app/kash/issues/1066) visibility | The loose grid, meshoptimizer LOD chains, and selection by projected error |
| [#1067](https://work.rezee.app/kash/issues/1067) editor | The grid, radii, and cell states it shows |
| [#1068](https://work.rezee.app/kash/issues/1068) terrain and generators | CDLOD over 129-sample tiles, Jolt height fields, the generator asset, and scatter identity |
| [#1069](https://work.rezee.app/kash/issues/1069) acceptance | W1, S1, the soak, and the budgets to measure against |
