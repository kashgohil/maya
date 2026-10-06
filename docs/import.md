# Importing glTF content

[#1036](https://work.rezee.app/kash/issues/1036) imports glTF 2.0 files (`.gltf` with the buffers and images it names, or `.glb`) into a project. An import adds the file's meshes and textures to the catalog as **parts of the file**, writes an **editable material file** per glTF material, and writes a **scene file** holding the file's nodes. Nothing is converted into another format: meshes and textures are read from the source when the registry loads them, and what loading cooks is kept in the [cook cache](assets.md#cook-cache). An **import file** beside the source holds the import's settings and the IDs it gave, so importing again keeps every ID it can match. OBJ files are still loaded as before ([assets](assets.md#loading-and-reload)).

The parser is cgltf v1.15, as the [rendering and content record](architecture/rendering-content-decision.md#gltf-import-cgltf) chose. Its types never leave [gltf.cpp](../src/maya/assets/gltf.cpp); the rest of Maya sees [gltf.hpp](../include/maya/assets/gltf.hpp). The importer is the MayaImport library ([gltf_import.hpp](../include/maya/import/gltf_import.hpp)), which needs no GPU.

```cpp
#include "maya/import/gltf_import.hpp"

const auto result = maya::import_gltf(project, "models/helmet.glb");
if (!result) for (const auto& problem : result.errors) log(maya::gltf_problem_text(problem));
// result.records: the catalog entries; result.scene: "models/helmet.scene"; result.warnings, kept, added, removed, renamed
```

In the editor, a file dropped on the window, or chosen in the Scene menu's **Import model**, is imported ([editor](editor.md#importing-models)).

## glTF

`GltfFile::open` parses the file, loads its buffers, converts what Maya reads, and then runs cgltf's validation. cgltf names only the kind of an error, so Maya checks what it converts itself, and every problem names its **JSON path**, such as `meshes[0].primitives[1].attributes.NORMAL: has 2 components per element, which a NORMAL cannot`. A file Maya refuses is never partly imported.

**Refused:** a file cgltf cannot parse or validate; glTF 1.0 ("convert it to glTF 2.0"); a file that **requires** an extension Maya does not read, by name (`extensionsRequired[0]: requires KHR_draco_mesh_compression, which Maya does not read`); and attributes with the wrong number of components or count, and sparse indices.

**Extensions read:** `KHR_texture_transform`, `KHR_materials_emissive_strength`, `KHR_lights_punctual`, and `KHR_mesh_quantization`. Any other extension the file only *uses* is a warning (`extensionsUsed[0]: KHR_materials_clearcoat is not read; what it adds is left out`).

| glTF | Maya |
| --- | --- |
| Mesh primitive (triangles, strips, fans) | A mesh: positions, normals (flat per triangle where the file has none), `TEXCOORD_0`, `COLOR_0`, and tangents (the file's, or [MikkTSpace's](renderer.md#materials) where it has none or no normals), with identical corners shared. Strips and fans become triangle lists in glTF's winding. Quantized and normalized attributes and sparse accessors are read. |
| Points and lines, primitives without positions | Left out, with a warning. |
| Material | A [material](assets.md#materials): base color, metallic, roughness, normal scale, occlusion strength, emissive and its strength, alpha mode and cutoff, double-sided, and the five maps. A factor the [material schema](properties.md#materials) refuses keeps its default, with a warning. A material with only specular-glossiness values is warned about. |
| Texture and sampler | A texture with glTF's filters and wraps (linear and repeating, with mips, when it has no sampler). Textures used only through `EXT_texture_webp` or `KHR_texture_basisu` are left out, with a warning. |
| `KHR_texture_transform` | The material's `uv_offset`, `uv_rotation`, and `uv_scale` ([properties](properties.md#materials)). Maya has one transform per material, applied to every map: the base color map's, or else the first map's that has one. A material whose maps are transformed differently is warned about. |
| `texCoord` other than 0 | Warned about; set 0 is read. |
| Node | An entity with a name and a transform. A matrix, or a negative scale, is decomposed into translation, rotation, and positive scale; a mirror is dropped (Maya's transforms cannot mirror) and a shear is lost, each with a warning. |
| Perspective camera | A camera: `yfov`, `znear`, and `zfar` (an infinite far plane becomes at least 1,000 m). Orthographic cameras are left out, with a warning. |
| `KHR_lights_punctual` light | A light ([below](#lights)). |
| Scene | The default scene's root nodes, or the first scene's, or every node without a parent. |
| Skins, animations, morph targets | Not imported, with a warning: skinned meshes keep their bind pose, and morphed meshes their base shape. |

glTF's conventions are Maya's: +Y up, −Z forward, metres, counter-clockwise front faces, and texture coordinates with v growing downward. Images are read from buffer views, data URIs, or files at or below the glTF file's folder, never elsewhere.

## Lights

| `KHR_lights_punctual` | Maya |
| --- | --- |
| `directional`, `intensity` in lux | Directional, the same intensity |
| `point` and `spot`, `intensity` in candela | Point or spot, the same intensity: Maya's measure too since #1034 |
| `range` | The same range; an unbounded light (no `range`) reaches as far as it gives 0.01 lux, √(intensity / 0.01) m, clamped to 0.1–10,000 m |
| `innerConeAngle`, `outerConeAngle` (from the axis) | Twice each: Maya's cone angles are full angles |

Lights shine along their node's −Z in both, and cast shadows by default (point lights cast none yet). The renderer draws them in the same units and with glTF's range window and cone falloff ([lights](renderer.md#lights)). Before #1034 point and spot lights imported as lumens and were not drawn; a reimport rewrites them in candela.

## What an import writes

For `models/helmet.glb`, an import writes, in this order:

1. `models/helmet/materials/<name>.material`: a [material file](assets.md#materials) per glTF material, named after it (made safe for a file name, and numbered when a name is taken), and `default.material`, glTF's default material (white, fully metallic, fully rough), when a primitive has none.
2. `models/helmet.scene`: a root entity named after the file, holding the file's nodes in order. A node whose mesh has one drawn primitive gets its mesh renderer; a mesh with several primitives, which Maya draws with one material each, gets a child entity per primitive, named after its material.
3. `models/helmet.glb.import`: the [import file](#import-files).
4. The project's catalog, with the [entries](#catalog-entries) for the parts and the material files.

A file that is written is replaced atomically ([replace_file](../include/maya/core/file_replace.hpp)), and a file whose text would not change is not written. Before anything is written, the new catalog is checked as a registry would read it, and the scene is validated against it: **a failed import changes nothing**, and the catalog is always written last, so it never names what was not written. Each problem is shown with its JSON path. The scene file takes another name (`helmet 2.scene`) when one of the first name exists that the import did not write.

### Catalog entries

A catalog path can name a **part of an imported file** after `#` ([split_asset_path](../include/maya/assets/asset.hpp)):

```text
mesh 6d617961 1a2b "models/helmet.glb#mesh/0/0"
texture 6d617961 1a2c "models/helmet.glb#texture/2/color"
```

- `mesh/<mesh>/<primitive>`: a glTF mesh's primitive.
- `texture/<texture>/<role>`: a glTF texture cooked as `color` (base color and emissive maps), `data` (metallic-roughness and occlusion), or `normal`. A texture used in two roles is two entries.

Only a path whose file ends in `.gltf` or `.glb` has a part, so `notes/a#b.obj` is still a file. Only meshes and textures can be parts. The registry checks the file, not the part, for presence and for staying inside the project, and calls `AssetProvider::load_imported_mesh` or `load_imported_texture`; the defaults refuse. `FileAssetProvider` keeps the last glTF file it opened while it is unchanged on disk, so a file's parts parse it once. An imported texture's compression and mips come from the import file, its role from its part, and its sampler from the glTF file (with Maya's anisotropy, 8, where it filters between mips).

### Import files

`<source>.import`, beside the source ([import_file.hpp](../include/maya/assets/import_file.hpp)):

```text
maya-import 1
compression astc
mips on
scale 1
up y
lights on
cameras on
scene "helmet.scene" 51ac09e2d3b4f607
mesh 6d617961 1a2b "mesh/0/0" "Helmet/0"
texture 6d617961 1a2c "texture/2/color" "Albedo/color"
material 6d617961 1a2d "helmet/materials/Metal.material" "Metal" 9f3c2b1a00ffe1d2
entity 6d617961 1a2e "/Helmet"
file "textures/albedo.png"
```

The **settings** come first, each at most once, and may be edited by hand; the editor imports the file again when they change:

| Setting | Values | Default |
| --- | --- | --- |
| `compression` | `astc` or `rgba8`, as a [texture's](assets.md#texture-files) | `astc` |
| `mips` | `on` or `off` (glTF samplers without mipmap filters turn them off) | `on` |
| `scale` | metres per unit of the file, 1e-6 to 1e6 | `1` |
| `up` | `y`, as glTF says; or `z`, for exporters that write +Z up: the import is turned so the file's +Z is up | `y` |
| `lights`, `cameras` | `on` or `off`: whether to import them | `on` |

The scale and axis conversion apply to the import's **root entity**, so the whole hierarchy follows; meshes are cooked as the file has them, so a mesh placed by itself is not scaled or turned. Then come what the import made, each with its ID and **identity**: what it was made from, by name where the file has names and by index where it does not (`#3`).

| Entry | Identity |
| --- | --- |
| `mesh`, `texture` | `<mesh name>/<primitive>`, `<texture name>/<role>` (a texture's name, else its image's, else its file's stem) |
| `material` | the material's name, and the file written (relative to the import file) and the hash of the text written |
| `entity` | the node's path from the root: `/Root/Panel`, with `/primitive <n>` for a primitive's entity |
| `scene` | the scene file and the hash of the text written |
| `file` | a file the source names (buffers and images), relative to the import file |

Identities that repeat are numbered (`Panel/0 ~2`). IDs are nonzero and unique within each kind.

## Reimporting

Importing a file again (from the Scene menu, or because the editor saw it change) keeps every ID it can match:

- **By identity:** a mesh, texture, material, or entity whose identity the import file has keeps its ID.
- **Renamed parts:** a mesh or texture whose identity is new, but which is where a vanished one was (the same part of the file), keeps that one's ID, and is reported as renamed. A part found in the catalog with another ID, such as one imported before the import file was deleted, keeps that ID too.
- **Edited files are kept:** a material or scene file whose text no longer has the hash the import wrote was edited, and is left as it is (`kept`, logged). Its recorded hash stays, so later imports keep it too. An unedited one is written again, and a deleted one is written afresh.
- **Reported:** what was added, renamed, and removed since the last import (`added`, `renamed`, `removed`). A removed mesh or texture leaves the catalog, so a scene still using it reports it [missing](renderer.md#render-snapshots); a removed material's file and entry stay in the project.

So a scene that places imported content, moves it, and overrides its materials renders the same after a reimport that changes only geometry: its entities keep their IDs and references, and the meshes load the new geometry. Placed copies are copies: a reimport changes the import's own scene, not the hierarchies placed from it.

## Cooking and the cook cache

Meshes are welded and given tangents, and textures are decoded, given mips, and compressed, when they load, as other assets are. What that cooks is kept in the project's [cook cache](assets.md#cook-cache): an imported part's key covers the source file, every file its import file says it names, the part, and its settings, so a part loads from the cache without parsing the glTF file. A source without an import file is not cached, since what it names is unknown.

## Cost

The [import workload](performance.md#import) on the R1 hero content (Release, Apple M4 Pro, a cool machine, the median of three runs; 5 October 2026):

| Model | Import | Cold load | Warm load | Cook cache |
| --- | --- | --- | --- | --- |
| ABeautifulGame (`.glb`; 15 meshes, 574k triangles, 38 textures) | 147 ms | 8.3 s | 1.0 s | 173 MiB |
| FlightHelmet (`.gltf`; 6 meshes, 95k triangles, 15 textures) | 110 ms | 3.9 s | 0.44 s | 48 MiB |
| CesiumMan (`.glb`; 1 mesh, 4.7k triangles, 1 texture) | 24 ms | 59 ms | 6.5 ms | 0.9 MiB |

*Import* reads, converts, and writes the material, scene, import, and catalog files. *Cold load* loads every part and material through an empty cook cache: almost all of it is ASTC compression of 2048-texel textures (15–120 ms each). *Warm load* loads them again in a new session, from the cache: reading the entries, checking their digests, and uploading. Every run cooked byte-identical entries.

## The Sample Viewer comparison

R1's visual bar ([decision](architecture/rendering-content-decision.md#r1-the-realistic-reference-environment)) asks that Khronos's material samples render as the **Khronos glTF Sample Viewer** renders them, under the same environment, exposure, and tone mapper, within a declared tolerance. [sample_viewer_tests.cpp](../tests/sample_viewer_tests.cpp) (`[visual][gpu][samples]`, in the `maya_visual_reference` CTest entry) imports MetalRoughSpheres and NormalTangentMirrorTest and renders four views of them, 640 × 480, lit only by the sample project's workshop HDRI (prefiltered at 256 per face, so mirrors show its detail), at exposure 1 (EV100 log₂(1/1.2)) with Khronos PBR Neutral and a black background. It compares them with [the Sample Viewer's renders](../tests/references/sample-viewer) of the same views.

**The tolerance**, per view, over the pixels either renderer draws the model on (both clear to black elsewhere), comparing each pixel's largest channel difference in 8-bit sRGB:

| Measure | Allowed | Measured (6 October 2026) |
| --- | --- | --- |
| Mean | ≤ 9 | 4.5–7.4 |
| Pixels within 16 | ≥ 92% | 94.2–98.7% |
| Pixels beyond 48 | ≤ 2% | 0.1–1.2% |
| Mean signed difference, each channel | within ±5 | −3.3 to −0.4 (blue the most) |

The rest of the difference is each renderer's own prefiltering of the environment, which shows in mirror reflections of fine detail (NormalTangentMirrorTest's smooth gold), its diffuse irradiance (Maya's nine spherical-harmonic coefficients are 1–4% below the exact integral for this HDRI), and edges, since neither antialiases. Maya's shading before #1036 failed it: MetalRoughSpheres differed by a mean of 11–12 with 74–80% of pixels within 16, rough metals up to 40% and partly metallic surfaces up to half darker, because light from the surroundings mixed F0 rather than the dielectric and metal results and had no multiple scattering. It now follows the Sample Viewer's form ([renderer](renderer.md#materials)).

**The captures** are taken with [tools/sample_viewer/capture.sh](../tools/sample_viewer/capture.sh): the Khronos glTF Sample Renderer (the Sample Viewer's renderer, `@khronosgroup/gltf-viewer` 1.1.0, pinned with Playwright 1.63.0 and esbuild in its `package.json`) renders [views.json](../tools/sample_viewer/views.json)'s views in headless Chromium on the GPU (ANGLE on Metal), reads the pixels back, and writes them as PNG files the tests read. The environment is turned 270° there, which puts its centre along −Z as Maya does. Its user camera's `lookAt` stores a view matrix where a world transform belongs, so the page sets the camera's transform itself. Running it again on the same machine gives byte-identical images; a capture becomes a reference only after the project owner inspects it (approved 6 October 2026). `MAYA_SAMPLE_VIEWER_OUT=<folder> maya_editor_tests "[sample-viewer-renders]"` writes Maya's renders of the same views for review beside them.

## Tests

[gltf_tests.cpp](../tests/gltf_tests.cpp) covers what the reader takes from small files written by the test (strips, fans, quantized and sparse accessors, colors, given and generated tangents, materials and samplers, texture transforms, images from data URIs and files, node decomposition, lights, and cameras), what it refuses and where, and every Khronos sample in [`tools/fetch_render_samples.sh`](../tools/fetch_render_samples.sh) when fetched (skipped otherwise, tag `[samples]`, CTest `maya_assets_samples`). [import_tests.cpp](../tests/import_tests.cpp) covers catalog parts, import files, an import's files and scene, loading what it cataloged, reimports that keep IDs and edited files and report changes, settings, failures that change nothing, the DamagedHelmet sample, and R1's three models: every node, drawn primitive, and material imported, every mesh loaded, and the scene valid. [editor_import_tests.cpp](../tests/editor_import_tests.cpp) covers the editor: the Scene menu, drops from inside and outside the project, placing, reimports when a source or a file it names changes, a scene that keeps its edits and overrides through a reimport, and reloads into a running Play. The texture transform is rendered in [material_gpu_tests.cpp](../tests/material_gpu_tests.cpp), and the [Sample Viewer comparison](#the-sample-viewer-comparison) is in [sample_viewer_tests.cpp](../tests/sample_viewer_tests.cpp). A hidden case, `[fuzz]`, opens 3,000 deterministically damaged copies of small glTF files and reads 3,000 damaged import files; run it under Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib maya_asset_tests "[fuzz]"`) after changing the reader.
