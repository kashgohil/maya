# Rendering and content stack

Decided 2 October 2026 for [ISSUE-1030](https://work.rezee.app/kash/issues/1030), the first issue of the [rendering and content milestone](https://work.rezee.app/kash/issues/1029), under [DOC-58](https://work.rezee.app/kash/docs/58). The measurements are from prototypes in this build, on the M4 Pro reference machine in a Release build, with the thermal state nominal.

| Decision | Choice | Instead of |
| --- | --- | --- |
| glTF parser | **cgltf v1.15** | fastgltf v0.9.1, tinygltf v3.0.1 |
| Image decoding and mips | **stb_image and stb_image_resize2**, when cooking only | Apple ImageIO |
| GPU texture formats | **ASTC**, compressed with **astcenc 5.7.0** when cooking; RGBA8 where asked | BC (also supported on Apple silicon; a seam for other platforms) |
| Cooked texture container | **KTX2**, read and written by Maya's own code, chosen by the project owner for its interoperability | A Maya-only format; libktx |
| Scene color | **RGBA16F** scene target, **EV100 exposure** on the camera, a tone-mapping pass to the view's RGBA8 target with sRGB encoding | RG11B10F, RGBA8 |
| Tone mapper | **AgX** by default, with **Khronos PBR Neutral** as a per-camera choice, chosen by the project owner | The ACES fit |
| Shading | **glTF metallic-roughness**: GGX with height-correlated Smith visibility and Schlick Fresnel, Lambert diffuse; **MikkTSpace** tangents where a mesh has none | Blinn-Phong |
| Light units | **glTF's**: lux for directional lights, candela for point and spot lights | Lumens as the stored unit |
| Shadows | **Cascaded shadow maps** for the sun (4 cascades of 2048 texels in one atlas, 3×3 comparison filtering); a shadow map for each shadowed spot light; no point-light shadows in this milestone | Point-light cube shadows |
| Environment lighting | **Cooked on the CPU**: diffuse irradiance as 9 spherical-harmonic coefficients and a GGX-prefiltered specular cube, stored with the environment; a split-sum BRDF table | Building it on the GPU at load |
| Animation | **Sampled on the CPU in the fixed tick; skinned on the GPU** (4 joints per vertex, a joint palette per draw); no animation library | CPU skinning; ozz-animation |
| Packaging | **A macOS application bundle** whose player finds resources only inside the bundle | Today's search of `MAYA_RESOURCES`, the executable's parent folders, and the working directory |
| The R1 reference environment | **ABeautifulGame, FlightHelmet, and CesiumMan under the Aerodynamics Workshop HDRI**, with the visual bar below, approved by the project owner | Sponza (its license); deciding later |

## The prototypes

The decisions were made from prototypes built behind `MAYA_BUILD_PROTOTYPES` (off by default), in [prototypes](../../prototypes). They read sample content that [fetch_render_samples.sh](../../tools/fetch_render_samples.sh) downloads: ten models from KhronosGroup/glTF-Sample-Assets at commit `f36bfdab`, and Poly Haven's Aerodynamics Workshop HDRI at 2k, by its SHA-256. None of it is committed.

| Target | What it does |
| --- | --- |
| `maya_import_prototype` | Loads 13 sample files with each parser (parse, external buffers, and validation), times them, and feeds each five broken files. It also samples animation clips, builds joint palettes, and skins on the CPU. |
| `maya_texture_prototype` | Decodes 122 images (105 MiB of PNG and JPEG) with stb_image and with ImageIO, builds mip chains, compresses three maps with astcenc at three block sizes and two qualities, reports Metal's formats, and builds an environment's irradiance and prefiltered levels. |
| `maya_render_prototype` | On Metal directly: tone maps an HDR view and an exposure chart with four operators (images and GPU times), and renders 10,000 boxes with cascaded shadows at 1920 × 1080 in three scene-target formats. |

They run in CTest under the `prototype` label when the option is on and the samples are present. The milestone's issues replace them; they are removed with the last of those issues, as #1018 removed #1015's.

## The RHI this milestone needs

The prototypes used Metal directly because the RHI ([rhi](../rhi.md)) lacks what these decisions need. The issues that use each feature add it:

| Feature | Needed by |
| --- | --- |
| Mip levels | Textures ([#1031](https://work.rezee.app/kash/issues/1031)) |
| Compressed formats (ASTC) | Textures |
| Cube textures | Environment lighting ([#1035](https://work.rezee.app/kash/issues/1035)) |
| Comparison samplers and sampled depth textures | Shadows ([#1034](https://work.rezee.app/kash/issues/1034)) |
| Per-pass GPU timing | [#1026](https://work.rezee.app/kash/issues/1026) |

Compute is not needed by any decision here. [#1031](https://work.rezee.app/kash/issues/1031) added mip levels and ASTC ([rhi](../rhi.md#mip-levels-and-compressed-formats)).

## glTF import: cgltf

Each parser loaded every sample, including skins, animations, `KHR_texture_transform`, and ABeautifulGame's 41 MiB binary file. Median of 15 loads, warm file cache:

| File | cgltf | fastgltf | tinygltf 3 |
| --- | --- | --- | --- |
| ABeautifulGame `.glb` (41 MiB of buffers) | 2.53 ms | 6.53 ms | 3.08 ms |
| ABeautifulGame `.gltf` | 0.55 ms | 1.29 ms | 0.62 ms |
| Sponza `.gltf` (103 primitives, 69 textures) | 0.63 ms | 1.24 ms | 0.83 ms |
| DamagedHelmet `.glb` | 0.09 ms | 0.47 ms | 0.16 ms |
| MetalRoughSpheres `.glb` | 0.51 ms | 1.45 ms | 0.55 ms |

Broken files:

| File | cgltf | fastgltf | tinygltf 3 |
| --- | --- | --- | --- |
| A truncated `.glb` | refused: data too short | refused: the GLB container is invalid | refused: GLB total length does not match data size |
| A JSON syntax error | refused: invalid JSON | refused: an error occurred while parsing the JSON | refused: failed to parse JSON |
| An index out of range | refused: invalid glTF | refused at validation | refused, naming `meshes[0].primitives[0].indices 99 out of range [0,4)` |
| A glTF 1.0 file | refused: legacy glTF | refused: version not supported | **accepted** |
| A missing buffer file | refused: file not found | refused: an external buffer was not found | refused, naming the file |

- **Why cgltf.** It is the fastest on large files and the smallest (one C header, MIT), it is mature and widely used, and it has helpers for normalized and sparse accessors and a writer for later export. It refuses glTF 1.0.
- **Its weakness, and the answer.** Its errors name a kind, not a place. The importer ([#1036](https://work.rezee.app/kash/issues/1036)) validates what it converts and reports problems with their JSON paths, as tinygltf 3 does.
- **The others.** tinygltf 3 has the best messages, but it is a new major version (a C rewrite) and it accepted a glTF 1.0 file. fastgltf needs simdjson and more C++, and its file buffer copy made it the slowest here.
- **Parsing is not the cost.** Every file loads in under 7 ms; decoding its images takes hundreds of times longer.
- **Extensions.** `KHR_texture_transform`, `KHR_materials_emissive_strength`, `KHR_lights_punctual`, and `KHR_mesh_quantization` are read. A file that *requires* an extension Maya does not read, such as `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, or `KHR_texture_basisu`, is refused with its name, never imported wrong. All three parsers report required extensions.

Pinned at tag `v1.15` (commit `360db1a9`). MIT.

## Images and mips: stb, when cooking

Decoding 122 images (1,084 MiB as RGBA8):

| Decoder | One thread | Every core |
| --- | --- | --- |
| stb_image | 2.52 s (113 megapixels/s) | 0.25 s |
| ImageIO, drawn into an sRGB RGBA8 bitmap | 2.43 s | — |

- **ImageIO is not faithful to the file's values.** It differed from stb by up to 215 per channel on PNGs (premultiplied alpha) and 101 on JPEGs (color management). A cooker needs the stored values, so ImageIO is not used.
- **stb_image is not hardened against hostile files.** It runs only when cooking the creator's own content; packaged games read cooked KTX2, never source images.
- **Mips** take 1.16 s for all 122 images on one thread with stb_image_resize2 (sRGB-correct for color, linear for data), and add a third to the size.

stb is pinned at commit `2c980bb5` (no releases are tagged). Public domain or MIT.

## GPU formats: ASTC, in KTX2

The M4 Pro (Metal family Apple9) supports ASTC, including HDR, and BC. ASTC on ABeautifulGame's 2048 × 2048 chessboard maps, compressed with every core:

| Map | 4×4 medium (8 bits per texel) | 6×6 medium (3.56) | 8×8 medium (2) |
| --- | --- | --- | --- |
| Base color | 56.0 dB, 57 ms | 47.4 dB, 29 ms | 44.2 dB, 33 ms |
| Occlusion-roughness-metallic | 50.3 dB, 121 ms | 45.1 dB, 42 ms | 42.6 dB, 37 ms |
| Normals (mean angle error) | 0.06°, 52 ms | 0.23°, 16 ms | 0.30°, 15 ms |

- **Defaults.** Color and data maps use 6×6 at medium quality; normal maps use 4×4 at medium in astcenc's normal mode (x in RGB, y in alpha; the shader rebuilds z). An import setting can keep a texture as RGBA8, for example UI art.
- **Cost.** Cooking a 2048 map takes 15–120 ms; cooked textures are cached ([#1036](https://work.rezee.app/kash/issues/1036)).
- **KTX2, by Maya's own code.** KTX2 is Khronos's texture container: glTF's `KHR_texture_basisu` uses it, and Khronos's `ktx` tools, PVRTexTool, RenderDoc, and other engines read it. Maya writes and reads the subset it needs: the header, the level index, the data format descriptor, key/value data, and the mip levels, holding ASTC or RGBA8 with the color space in KTX2's data format descriptor. A texture's usage, compression, and sampler settings are in its texture file (`.texture`), chosen by the project owner in [#1031](https://work.rezee.app/kash/issues/1031) ([textures](../assets.md#textures)). Tests check written files with Khronos's `ktx validate` when it is installed. libktx and its BasisU transcoder are added only if importing BasisU-compressed glTF files becomes needed.
- **BC** stays a seam: if a platform without ASTC is added, the cooker adds a BC target.

astcenc is pinned at tag `5.7.0` (commit `baff485b`). Apache-2.0.

## Color: RGBA16F, EV100, and AgX

Tone mapping 1920 × 1080 from an RGBA16F target to RGBA8 with sRGB encoding (GPU time, median of 50):

| Operator | GPU time | Look, on the workshop view and a chart from −6 to +10 stops |
| --- | --- | --- |
| Clamp | 0.12 ms | Everything above 1 is flat white |
| ACES (Hill's fit) | 0.19 ms | Contrasty; saturated colors shift hue (blue toward purple, red toward orange) |
| **AgX** | 0.26 ms | Smooth highlight roll-off toward white with stable hues; slightly soft |
| **Khronos PBR Neutral** | 0.18 ms | Base colors stay truest up to about 0.8, then compress hard |

- **AgX by default; PBR Neutral per camera.** The project owner chose AgX for scenes and PBR Neutral as a camera's option for material and product views.
- **The scene target.** At 10,000 boxes and 1920 × 1080, RGBA8, RGBA16F, and RG11B10F all took 0.136–0.137 ms (after a warm-up: the first frames on a light load run at low GPU clocks and read slower). RGBA16F keeps alpha and precision for 16 MiB at 1080p.
- **Exposure** is camera data in EV100, with a fixed value first; metered (automatic) exposure is a later option. The tone-mapping pass writes the view's existing RGBA8 target; debug lines draw after it.

## Shading and lights

- **The model** is glTF's metallic-roughness: GGX specular with height-correlated Smith visibility and Schlick Fresnel, and Lambert diffuse. Base color, metallic-roughness, normal, occlusion, and emissive maps, with glTF's alpha modes.
- **Tangents.** glTF requires MikkTSpace tangents where a mesh has none, so the importer generates them with the reference `mikktspace.c` (zlib license). Since [#1033](https://work.rezee.app/kash/issues/1033) the OBJ loader does too; `mikktspace.c` is pinned at commit `3e895b49` (MikkTSpace has no tagged releases), built as C into `MayaMikkTSpace`.
- **Implemented** by #1033 ([materials](../renderer.md#materials)). Its measured energy behavior, and the version-1 content's change, are recorded there. `KHR_texture_transform` and `KHR_materials_emissive_strength` are read by the importer ([#1036](https://work.rezee.app/kash/issues/1036)): materials hold an emissive strength, and texture transforms are added to materials with the importer that needs them.
- **Units** follow glTF's `KHR_lights_punctual`: lux for directional lights and candela for point and spot lights, so imported lights keep their values. The Inspector may show lumens beside candela.
- **Light count.** A fixed number of local lights per view, measured in [#1034](https://work.rezee.app/kash/issues/1034), with lights beyond it reported. Clustered shading is the next step if R1 needs more.
- **Implemented** by #1034 ([lights](../renderer.md#lights)): lux and candela, glTF's range window and cone falloff, and 16 point and spot lights per view, the ones bringing the most light to the camera, with the rest reported. Scenes that stored lumens load converted. The Inspector shows candela only.

## Shadows: cascades for the sun

10,000 boxes (as one instanced draw) and a floor, at 1920 × 1080, RGBA16F; GPU time, median of 30 frames:

| Cascades × texels | Shadow maps | Lit pass, 1 tap | 9 taps (3×3) | 25 taps (5×5) |
| --- | --- | --- | --- | --- |
| 3 × 1024 | 0.26 ms | 0.15 ms | 0.21 ms | 0.41 ms |
| 3 × 2048 | 0.36 ms | 0.15 ms | 0.21 ms | 0.41 ms |
| 4 × 1024 | 0.34 ms | 0.15 ms | 0.21 ms | 0.41 ms |
| **4 × 2048** | **0.45 ms** | 0.15 ms | **0.21 ms** | 0.42 ms |

- **Four cascades of 2048 texels** in one 4096 atlas, fitted by bounding spheres (their size never changes) with texel snapping against shimmer, and **3×3 comparison filtering**: 0.66 ms in all here.
- **Spot lights** get one shadow map each from the same atlas. **Point-light shadows** (six faces each) are left out of this milestone; R1 does not need them.
- The prototype's image is a cost proxy, not a quality reference; [#1034](https://work.rezee.app/kash/issues/1034) owns quality.
- **Implemented** by #1034 ([shadows](../renderer.md#shadows)) as chosen here, with one change: spot lights have their own 2048 × 2048 atlas of four 1024-texel maps, so the sun keeps all of its atlas and a fifth shadowed spot light is drawn unshadowed and reported.

## Environment lighting: cooked on the CPU

From the 2048 × 1024 HDRI (brightest texel: a relative luminance of 47):

| Step | Time |
| --- | --- |
| Loading the HDR file | 23 ms |
| Irradiance as 9 spherical-harmonic coefficients, one thread | 1 ms |
| Prefiltered specular cube, 128 per face, 6 levels, 256 samples per texel, every core | 47 ms (1.0 MiB as RGBA16F) |
| The same, 256 per face | 174 ms (4.0 MiB) |
| The same, 256 per face and 1,024 samples per texel | 638 ms |

This is cheap enough to do when cooking, once per environment version, with no GPU compute. The cooked environment stores both, and the split-sum BRDF table is built once.

**Implemented** by [#1035](https://work.rezee.app/kash/issues/1035) ([environments](../renderer.md#environments)): cooked when an environment loads, from a 1024 × 512 source in 58–60 ms at 128 per face and 202 ms at 256 (Release, M4 Pro), with filtered importance sampling, and RHI cube textures. #1036's cook cache will keep cooked environments.

## Animation: sample on the CPU, skin on the GPU

Per character, mean of 1,000 evaluations through cgltf's generic accessor reads:

| Model | Joints | Vertices | Sampling the clip | Joint palette | CPU skinning |
| --- | --- | --- | --- | --- | --- |
| CesiumMan | 19 | 3,273 | 0.005 ms | 0.0005 ms | 0.033 ms |
| Fox | 24 | 1,728 | 0.003 ms | 0.0007 ms | 0.017 ms |

- **Sampling** runs in phase 4 of the fixed tick (animation and body preparation), after phase 3's scripts, so a script's clip change takes effect that tick and replays repeat it ([scheduling](scheduling-contracts.md#frame-and-fixed-tick-order)). Cooked clips store keys per joint, faster than these generic reads.
- **Presentation** interpolates joint poses between ticks, as it interpolates transforms ([play](../play.md#between-ticks)).
- **Skinning** is on the GPU (4 joints per vertex, the palette in per-draw memory): 100 characters like CesiumMan would cost about 3.3 ms of CPU to skin. No animation library is needed for glTF's linear, step, and cubic-spline samplers.

**As built in #1038** ([animation](../animation.md)), with one change from the above, which the issue's owner chose: the animation system runs after the scripts in phase 3 and reads components as the tick began, like every system, so **a script's clip change shows one tick later**, not that tick; replays repeat it exactly either way. Joints are entities, whose transforms the clip writes through the tick's commands, so presentation and replay need nothing of their own. Clips keep glTF's channels (keys per channel, not per joint). The palette is one upload per frame that every pass reads, each instance holding its range.

## Packaging: a bundle that finds only itself

A hand-made `.app` with the player, the shaders, and the sample project:
- **Outside the checkout**, it failed to start: "the renderer shader was not found". The player searches `MAYA_RESOURCES`, the executable's folder and up to eight parents, and the working directory, and none of them is the bundle's `Resources`.
- **Inside the checkout**, with no shaders in the bundle, it **started**, using the checkout's resources through the parent search.

So a packaged player ([#1039](https://work.rezee.app/kash/issues/1039)) resolves resources only from its bundle (`Contents/Resources`: shaders, fonts, and the cooked project), with no parent search, working directory, or environment variable. Development builds keep today's search. Shaders ship as source, compiled at start as today; a precompiled library is a later option.

**Implemented** by #1039 ([packages](../projects.md#packages)): `maya_package` writes the bundle, with the startup scene and named scenes and exactly the assets they reach, cooked; the player finds only its bundle when it runs from one, and a test launches a package from inside the checkout without its shader and sees it refuse to start. The player opens no fonts, so a package holds none.

## R1: the realistic reference environment

Recipe version 1, approved by the project owner on 2 October 2026:

| Part | Content |
| --- | --- |
| Hero content | ABeautifulGame (a chess set on its board; ASWF, CC-BY 4.0), FlightHelmet (CC0), and CesiumMan (CC-BY 4.0) walking a loop, from glTF-Sample-Assets at commit `f36bfdab` |
| Lighting | The Aerodynamics Workshop HDRI (Poly Haven, CC0) for the environment and sky; a shadowed sun; a shadowed spot light; two point lights |
| Views | Five named views, and a fixed 600-tick camera path around the board |
| Content handling | Fetched by `tools/fetch_render_samples.sh`, never committed; attributions in the docs that use them |

Sponza is excluded: it is under the Cryengine Limited License Agreement.

**The visual bar:**
- **Material correctness.** MetalRoughSpheres and NormalTangentMirrorTest match the Khronos glTF Sample Viewer's renders under the same environment, exposure, and tone mapper, within a tolerance declared in [#1033](https://work.rezee.app/kash/issues/1033). (Moved to [#1036](https://work.rezee.app/kash/issues/1036) by the owner on 3 October 2026, which imports the samples; [declared there](../import.md#the-sample-viewer-comparison).)
- **R1's views** are reviewed and approved by the project owner, then blessed as references, compared as the [V1 references](../acceptance.md#regression-scenes) are.
- **Along the camera path,** no shadow acne, visible cascade seams, or shimmer.
- **The editor, the player, and the package** render the same images.

R1's benchmark manifest and budgets come with [#1040](https://work.rezee.app/kash/issues/1040).
