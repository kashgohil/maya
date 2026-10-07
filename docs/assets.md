# Asset identity, loading, and residency

[#993](https://work.rezee.app/kash/issues/993) adds `MayaAssets` / `Maya::Assets`, publicly linked by MayaRuntime. [registry.hpp](../include/maya/assets/registry.hpp) provides the project registry; [asset.hpp](../include/maya/assets/asset.hpp) defines typed handles, leases, loaded values, and providers. The library links only MayaWorld and uses the abstract GraphicsDevice interface for mesh uploads. It has no Metal, GLFW, editor, or global FileSystem dependency. World components continue to store plain [AssetRef values](../include/maya/assets/asset_ref.hpp).

## Identity and use

`AssetRef<T>` stores a persistent 128-bit AssetId and does not load or retain anything. IDs come from the catalog, never process addresses or hashes of mutable paths. A registry maps each ID to one asset kind and a normalized project-relative source path. Duplicate IDs and canonical source-path aliases are rejected; distinct files with equal contents are not content-deduplicated. A project normally shares one registry across its authoring/play Worlds.

```cpp
maya::AssetRegistry assets(project_root,
    std::make_unique<maya::FileAssetProvider>(device));
const auto mesh_ref = maya::AssetRef<maya::MeshAsset>{{0x6d617961, 1}};
const auto registered = assets.register_asset(mesh_ref, "models/tree.obj");
if (registered) { /* report registered.message */ return; }

auto first = assets.acquire(mesh_ref);
if (!first) { /* report first.diagnostic.message; skip this mesh draw */ return; }
auto second = assets.acquire(mesh_ref); // same loaded version, no second upload
const auto handle = first.lease.handle();
const auto resolved = assets.resolve(handle); // validates owner, slot, generation and availability
first.lease.value().mesh().draw(); // existing renderer adapter; lease must cover encoding
```

`AssetLease<T>` owns a shared immutable loaded version and is copyable. Its `value()` reference must not outlive the lease; reading an empty lease throws `logic_error`. A lease can be held in a native component or renderer-owned cache. Destroying an entity/World releases any leases it owns; its plain MeshRendererComponent references alone have no residency effect. [Render extraction](renderer.md#render-snapshots) acquires the meshes a frame draws and keeps their leases in the snapshot until it is released.

`AssetHandle<T>` is non-owning and contains a registry lifetime token, slot, and version generation. A replaced/evicted/unavailable version or a foreign registry fails resolution. A successful reload publishes a fresh generation. Existing leases still own their previous version, even though their old handle no longer resolves through the registry. Registry tokens and runtime handles are not serialized. Registry slots are append-only for its lifetime; there is no unregister or slot-reuse API yet.

## Catalog and material files

The version-one catalog stores kind, hexadecimal high/low ID words, and a quoted project-relative path:

```text
maya-assets 1
mesh 6d617961 1 "models/tree.obj"
material 6d617961 2 "materials/bark.mat"
script 6d617961 20 "scripts/spin.luau"
texture 6d617961 50 "textures/bark.texture"
environment 6d617961 58 "environments/workshop.environment"
mesh 6d617961 1a2b "models/helmet.glb#mesh/0/0"
```

A path whose file ends in `.gltf` or `.glb` can name a **part** of that file after `#`: a glTF mesh's primitive (`#mesh/<mesh>/<primitive>`) or a texture in a role (`#texture/<texture>/<role>`). Imports write these entries ([import](import.md#catalog-entries)); only meshes and textures can be parts, and the registry checks the file's presence and boundary, not the part's.

`write_asset_catalog(stream, registry.records())` writes metadata only. `read_asset_catalog(stream)` checks syntax/version/ID words and returns records or a diagnostic; register each record in a fresh registry before publishing that project. Registration validates identity, kind, duplicate sources, and project boundaries. Parsing/registration does not load resources. Loading a malformed catalog must discard the unpublished registry, not expose a partially registered project. Filesystem I/O and allocation exceptions remain ordinary exceptions. This catalog is not the [scene format](scene.md) (#995); scenes store only AssetIds and validate them against the catalog.

Relative paths resolve strictly against the supplied existing project directory. Missing files can be registered so their identity/diagnostics survive. Absolute paths, traversal outside the root, and symlink escapes are rejected; symlinks are rechecked on every actual load. The registry does not consult the working directory or application search roots. Moving the project directory preserves references when its relative layout and catalog move with it. Imports ([#1036](import.md)) rewrite the catalog atomically, last, after every file it names is written; the editor watches loaded assets' files ([watched files](editor.md#watched-files)).

### Materials

A material file holds a [MaterialAsset](../include/maya/assets/material.hpp): glTF's metallic-roughness inputs, each a factor and, for most, an optional texture ([how they shade](renderer.md#materials)). Since #1033 it is written at version 2, one line per [material property](properties.md#materials), keyed by the property's name ([material_file.hpp](../include/maya/assets/material_file.hpp)):

```text
maya-material 2
base_color 1 1 1
alpha 1
base_color_texture 6d617961 50
metallic 0
roughness 0.6
metallic_roughness_texture none
normal_texture 6d617961 51
normal_scale 1
occlusion_texture none
occlusion_strength 1
emissive 0 0 0
emissive_strength 1
emissive_texture none
alpha_mode opaque
alpha_cutoff 0.5
double_sided false
```

- **Reading.** Lines may come in any order, blank lines and `#` comments are skipped, and a property without a line has its default, so a hand-written file can be short. Each value is checked as the property system checks an edit: colors, alpha, metallic, roughness, occlusion strength, and the cutoff in [0, 1]; normal scale in [0, 10]; emissive strength in [0, 100,000]; textures `none` or two hexadecimal ID words, not both zero. Unknown keys, a key given twice, and bad values fail with the line number and what was expected. Texture references are checked for form only; extraction reports a [missing one or one of the wrong role](renderer.md#materials).
- **Writing** (`write_material_file`, `save_material_file`) writes every property in schema order, with floats in their shortest exact form, so the same material always writes the same bytes. `save_material_file` replaces the file atomically, as scenes are saved (`replace_file`, [file_replace.hpp](../include/maya/core/file_replace.hpp)): a failure leaves the old file untouched.
- **Version 1** (base color RGBA, metallic, and roughness, in that fixed order, separated by any whitespace) still loads unchanged, with the other inputs at their defaults. Saving it, as the [editor](editor.md#materials) does after an edit, writes version 2. Base color's alpha is its own `alpha` line at version 2.

`fallback_material()` returns explicit magenta/opaque, nonmetallic, rough data for callers choosing a fallback. Missing meshes skip their draw. Neither fallback replaces the missing reference's ID or reports the source as successfully loaded.

**Publishing.** `AssetRegistry::publish(ref, material)` makes an in-memory material the entry's next version, as a successful reload would, without reading its file: later acquisitions and extractions see it, leases of the previous version keep theirs, and a failed entry becomes ready. The editor publishes its unsaved edits this way; `reload` reads the file again.

Script assets (`script`, `ScriptAsset`, since #1018) are Luau source text. `AssetProvider::load_script` reads the file as text, and every provider inherits it; the registry neither compiles nor checks the source. Play sessions and the editor compile it ([scripting](scripting.md)), and bytecode is never stored. Since #1020 the editor watches script files and reloads changed ones through `reload` ([scripting](scripting.md#reload)); the Assets panel lists scripts.

## Textures

[#1031](https://work.rezee.app/kash/issues/1031) adds textures (`texture`, `TextureAsset`), in the stack the [rendering and content record](architecture/rendering-content-decision.md#gpu-formats-astc-in-ktx2) chose. Materials sample them since [#1033](https://work.rezee.app/kash/issues/1033) ([materials](#materials)); the editor's [Assets panel](projects.md#the-assets-panel) lists them with thumbnails, and they are dragged from it into a material's map slots.

### Texture files

A catalog `texture` entry names a texture file, which names its source image and states every setting. Nothing is guessed from file names or pixels.

```text
maya-texture 1
source "board_color.png"
usage color
compression astc
mips on
filter linear linear
mip_filter linear
anisotropy 8
address repeat repeat
```

| Setting | Values | Meaning |
| --- | --- | --- |
| `source` | a quoted path | The image, relative to the texture file's folder and at or below it (no `..`, no absolute paths; symlinks are checked on every load). A `.png`, `.jpg`, or `.jpeg` is cooked when loaded; a `.ktx2` is already cooked. |
| `usage` | `color`, `data`, `normal` | Color is sRGB-encoded; data (occlusion, roughness, metallic, masks) and normal maps are linear. |
| `compression` | `astc`, `rgba8` | ASTC 6×6 for color and data and 4×4 for normals, or uncompressed RGBA8 (e.g. for UI art). |
| `mips` | `on`, `off` | A full chain to 1×1, or level 0 only. |
| `filter` | `nearest`/`linear` twice | Minification, then magnification. |
| `mip_filter` | `none`, `nearest`, `linear` | Between levels. Must be `none` when `mips` is off. |
| `anisotropy` | 1 (off) to 16 | Clamped to the device's limit. |
| `address` | `repeat`/`clamp`/`mirror` twice | U, then V. |

Each setting appears exactly once, in any order; blank lines are allowed. `read_texture_settings` reports the first problem with its line (`line 3: usage must be color, data, or normal`), and `write_texture_settings` writes every setting in the order above. The descriptor and its formats live in `MayaTextures` ([texture_data.hpp](../include/maya/assets/texture_data.hpp)), which MayaAssets links.

| `usage` | `compression astc` | `compression rgba8` |
| --- | --- | --- |
| color | `astc_6x6_srgb` | `rgba8_srgb` |
| data | `astc_6x6_unorm` | `rgba8_unorm` |
| normal | `astc_4x4_unorm` | `rgba8_unorm` |

**Normal maps** are stored with tangent-space x in red, green, and blue and y in alpha, as astcenc's normal mode expects, in both formats. Shaders rebuild z = √(1 − x² − y²) from x and y in [−1, 1].

### Cooking at load

A PNG or JPEG source is cooked when its texture loads and the [cook cache](#cook-cache) does not have it ([texture_cook.hpp](../include/maya/assets/texture_cook.hpp)):

1. **Decode** with stb_image (PNG and JPEG only, from memory) to straight-alpha RGBA8, exactly the stored values: no color management or premultiplication. Images above the device's largest texture are refused before their pixels are decoded. stb_image is not hardened against hostile files; it cooks the project's own content, and [packages](projects.md#packages) hold only cooked content (#1039).
2. **Mips** with stb_image_resize2, each level halved from the one before: sRGB-correct with alpha-weighted color for color, every channel independent for data, and renormalized for normals (a straight-up normal where a texel holds no direction).
3. **Compress** with astcenc 5.7.0 at medium quality on the [job system](jobs.md#cooking)'s frame tier and the calling thread, or keep RGBA8. A device that cannot sample ASTC gets RGBA8. The same source, settings, and device always give the same bytes, whatever the thread count.

On the reference machine (M4 Pro, Release), ABeautifulGame's 33 maps of 2048×2048 decode in 1.1 s on one thread and take 5.0 s to build mips and compress (40–540 ms each; noisy normal and ORM maps take longest), for 108 MiB of ASTC. A `.ktx2` source skips all of this.

### KTX2

Maya reads and writes KTX 2.0 with its own code ([ktx2.cpp](../src/maya/assets/ktx2.cpp)), without libktx. The subset is one 2D image with its mip levels, no array layers, cube faces, or supercompression, in RGBA8 (`vkFormat` 37, 43) or ASTC 4×4 and 6×6 LDR (157, 158, 165, 166). `write_ktx2` writes:

- the identifier and header (`typeSize` 1, depth 0, no layers, one face), and the level index;
- a basic data format descriptor: the RGBSDA or ASTC color model, BT.709 primaries, an sRGB or linear transfer function matching `vkFormat`, four 8-bit samples (alpha marked linear in sRGB files) or one 128-bit ASTC sample with the block size;
- key/value data sorted by key: `KTXorientation` `rd` and `KTXwriter`;
- the levels from smallest to largest, each aligned to the least common multiple of its block size and 4.

`read_ktx2` checks the identifier, the header, every level's size, offset, and alignment against the file, and the descriptor's color model and transfer function against `vkFormat`, before allocating or copying texels, and refuses images larger than 16384 on a side (the RHI's limit). It refuses anything else with the first problem (`supercompression scheme 1 is not supported`). Reading 200,000 mutated files under Guard Malloc found no read past a buffer and no unbounded allocation. It reads files Khronos's own tools write as well as Maya's. A cooked source must match its texture file: its color space and format family (sRGB for color, ASTC or RGBA8 as stated) and its mips (a full chain, or one level).

With KTX-Software 4.3 or later installed (or `MAYA_KTX_TOOL` set), CTest's `maya_ktx2_validate` writes every format and layout Maya writes (`maya_ktx2_samples`) and checks each with `ktx validate --warnings-as-errors`.

### Texture assets

A `TextureAsset` owns a sampled texture with every level and a sampler built from the settings (labelled with the file's stem), and records its usage. `gpu_bytes()` counts every level as the device does (`texture_bytes`). Loading, sharing, reload, eviction, and retirement follow the mesh rules below: a reload publishes a new version, and the old texture and sampler are released only after every frame that could sample them completes. A failed reload keeps the previous version. `residency()` counts resident textures and their GPU bytes.

**Missing or failed textures** fail with a diagnostic naming the texture file, its line or its source, and allocate nothing. `make_placeholder_texture(device)` makes the declared stand-in: an 8×8 magenta and black checkerboard of 2×2-texel squares (`placeholder_texture_pixels()`), sRGB RGBA8, nearest filtering, repeating. A consumer draws it in a failed texture's place and still reports the problem; the reference keeps its ID. Materials use it from #1033; the editor's thumbnails show it now.

## Environments

[#1035](https://work.rezee.app/kash/issues/1035) adds environments (`environment`, `EnvironmentAsset`) for [image-based lighting](renderer.md#environments): an HDR image of the surroundings, cooked on the CPU when it loads, as the [rendering and content record](architecture/rendering-content-decision.md#environment-lighting-cooked-on-the-cpu) chose. A scene uses one through its Environment component.

An environment file (`.environment`) names its source and, optionally, how to cook it ([environment_cook.hpp](../include/maya/assets/environment_cook.hpp)):

```text
maya-environment 1
source "aerodynamics_workshop_1k.hdr"
specular_size 128
samples 256
```

- `source` (required): a Radiance `.hdr` image, equirectangular (twice as wide as tall), at most 8,192 wide, at or below the file's folder, as a texture's source must be.
- `specular_size`: texels per side of the prefiltered cube's first level, a power of two from 16 to 512 (default 128).
- `samples`: importance samples per prefiltered texel, 16 to 4,096 (default 256).

Each key appears at most once; anything else is refused with its line. **Loading** decodes the source with stb_image (negative or nonfinite values read as 0), then `cook_environment` builds, on the [job system](jobs.md#cooking)'s frame tier and the calling thread, and deterministically whatever the thread count:

- **the background:** the source and its full mip chain (2 × 2 box filtered), RGBA16F, for the sky;
- **irradiance:** nine spherical-harmonic coefficients, projected from every texel by its solid angle and convolved with the cosine lobe;
- **the specular cube:** RGBA16F, up to six levels from `specular_size` (128 gives 128 to 4). Level 0 reads the source at the mip that matches a cube texel; each rougher level averages GGX importance samples around each texel's direction (N = V = R), each read from the source at the mip that matches its solid angle (filtered importance sampling, Karis 2013), so few samples stay smooth.

Values above the half-float range (65,504) are clamped. An `EnvironmentAsset` owns the background, the cube, a sampler (linear, repeating around the horizon), and the coefficients, and reports its cooking time and GPU bytes. **Once per version:** an environment is cooked when it is first acquired and when it is reloaded, never per frame; `residency()` counts resident environments and their GPU bytes. A 1024 × 512 source cooks in about 60 ms at 128 per face in Release (200 ms at 256) and takes 6.3 MiB on the GPU ([cost](renderer.md#environments)); the [cook cache](#cook-cache) keeps cooked environments between runs.

**Sample environments.** The sample project's [environments](../samples/basic_scene/assets/environments) are two [Poly Haven](https://polyhaven.com) HDRIs at 1024 × 512, CC0 (public domain): *Aerodynamics Workshop* (indoors, `workshop`) and *Kloofendal 48d Partly Cloudy (Pure Sky)* (outdoors, `sky`). Unlike #1030's sample content, which is fetched, they are committed (about 1.4 MiB each) because the reference images need them; their sources and hashes are in the folder's README. The 2k workshop, which R1 uses ([acceptance](acceptance.md#r1)), is fetched.

## Cook cache

[#1036](https://work.rezee.app/kash/issues/1036) keeps what loading cooks, so a later load reads it instead of cooking again ([cook_cache.hpp](../include/maya/assets/cook_cache.hpp)). `FileAssetProvider` takes an optional `CookCache`; the editor and the player give it the project's, `<project folder>/.maya/cache` (`cook_cache_folder`), and tests and the benchmark's frame workloads give it none.

| Entry (`.<kind>`) | Payload | Key: besides the kind, its version, and the source's SHA-256 |
| --- | --- | --- |
| `texture` | the cooked image as [KTX2](#ktx2) | role, compression, mips, and whether the device samples ASTC |
| `environment` | the background, the specular cube, and the irradiance | `specular_size`, `samples`, and the largest dimension decoded |
| `mesh` (imported) | the welded vertices and indices, and (since #1038, `imported_mesh_cook_version` 2) each vertex's four joints and weights | the part |
| `imported-texture` | the sampler's description, then KTX2 | the part, role, compression, mips, and ASTC |
| `skin`, `animation` (#1038) | a [skin's](animation.md#skins-and-clips) joint paths and inverse bind matrices; a clip's name, duration, and channels | the part |

- **Keys** are SHA-256 digests of the kind, a **cook version** per kind (`texture_cook_version`, ...), the source's digest, and the settings as text. A change to how something is cooked that changes its bytes raises its version, so older entries are never read. An imported part's source digest covers the glTF file and [every file its import file names](import.md#import-files); without an import file, parts are not cached. A source's digest is remembered while its size and modification time stay the same, so a file's parts hash it once per session.
- **Entries** are `<first two hex digits>/<digest>.<kind>`: a header with the payload's size and SHA-256, then the payload. An entry that does not match its digest (damaged, or caught half-written by another program) is a miss, cooked again, and replaced. Entries are written atomically ([replace_file](../include/maya/core/file_replace.hpp)).
- **Never shared:** the folder holds a `.gitignore` of `*`, written with its first entry. Deleting the folder is always safe; it is filled again as assets load. Nothing removes old entries yet: a changed source leaves the entries cooked from what it was.
- **Never fatal:** an entry that cannot be written (a read-only project, a full disk) is counted in `stats().failures`, and the asset loads anyway.
- **Deterministic:** cooking is, so the same source and settings give byte-identical entries; the [import workload](performance.md#import) checks this on every run.

A cached environment reports a cooking time of 0. With R1's content, loading from the cache is 8 times faster than cooking ([cost](import.md#cost)).

## Cooked content

Since [#1039](https://work.rezee.app/kash/issues/1039) cooking is separate from the device ([asset_cooker.hpp](../include/maya/assets/asset_cooker.hpp)). `AssetCooker` turns an OBJ file, a texture or environment file, or an imported glTF part into its cooked form (`CookedMesh`: welded vertices with tangents and indices, and joints and weights for a skinned part; `SkinAsset` and `AnimationAsset` for skins and clips (#1038); `CookedTexture`: the mip chain in its GPU format, the sampler, and the role; `CookedEnvironment`) for the `CookLimits` it is given (ASTC or RGBA8, and the largest dimension), through the cook cache with the same keys and entries as before. `FileAssetProvider` cooks for its device's limits and uploads (`upload_mesh`, `upload_texture`, `upload_environment`); packaging cooks and writes the results into a [package](projects.md#packages), whose `PackageAssetProvider` uploads them. `ModelLoader::parse_obj` reads an OBJ without a device.

A package's cooked files are `write_cooked_mesh`, `write_cooked_texture` (the role, the sampler, then KTX2), and `write_cooked_environment` payloads in the cache's checked envelope (`wrap_cooked`: "MAYACOOK", the format, the payload's size and SHA-256). `unwrap_cooked` and the readers refuse damaged or malformed bytes, so a damaged package fails to load that asset, with the reason, rather than drawing garbage.

## Loading and reload

| State | Behavior |
| --- | --- |
| `unloaded` | Catalog entry exists without a resident version. First acquire loads synchronously. |
| `loading` | Provider is preparing a candidate. Read-only info queries can observe this state. Nested loads/catalog mutation are rejected as busy; eviction does nothing. |
| `ready` | Registry owns a cache lease. Repeated acquire returns the same immutable version. |
| `failed` | Initial load failed. Acquires return its saved diagnostic without retrying every frame; use explicit reload to retry. |

`info(id)` returns a copy of metadata/state/generation/diagnostics. A failed reload leaves a previously ready version and its generation available, with the failure recorded separately. The provider's candidate is private until validation succeeds; incomplete/failed candidates release their resources. Allocation failure restores the previous loading state and propagates; ordinary provider exceptions become diagnostics. Generation exhaustion refuses further publication rather than wrapping.

`AssetProvider` is replaceable and returns owned candidates plus diagnostics. The initial `FileAssetProvider` calls the existing OBJ loader through a checked entry point, validates material files, and uploads one vertex/index buffer pair per loaded mesh version. OBJ support is deliberately limited to positive indices and triangular faces with optional UVs/normals. Malformed numbers, missing coordinates, invalid indices, unsupported polygons, and empty geometry report file/line diagnostics before upload. Absent UVs/normals retain the legacy zero defaults; authored normals are needed for useful lighting. Since #1033 the loader generates [MikkTSpace tangents](renderer.md#materials) per triangle corner and then shares the corners that agree (`generate_tangents`, `weld_vertices`), so a vertex on a UV seam may be split where it was not before. The legacy `ModelLoader::load_obj` adapter retains application search-root resolution.

Loading is **synchronous** and registry/device access, including final mesh-lease release and cache eviction, belongs to one owner thread. Call it at a controlled scene/asset boundary, not accidentally in a per-draw path. The separation between persistent identity, immutable candidate publication, provider, and residency leaves room for asynchronous CPU import and dependency bundles. Worker scheduling, cancellation/request tokens, retries, and load budgets are not implemented; glTF files are [imported](import.md) and their parts load through the same provider, and cooked results are [cached](#cook-cache). An async extension must tag completions by registry/request generation, reject stale results, and keep GPU upload/publication on the owning thread. Future composite assets must retain dependency leases for the complete lifetime of their loaded version; a plain dependency AssetRef does not pin it.

## Release and GPU retirement

The registry retains one cache lease for each published version. `evict_unused()` removes versions whose only remaining owner is that cache, preserving catalog entries as unloaded. Call it at scene-unload/maintenance boundaries. There is no hidden timed/LRU eviction; explicit maintenance is required to bound the resident cache. Old versions after reload live only as long as their remaining leases. Registry destruction drops its cache ownership; outstanding leases remain valid while their device session remains available.

`residency()` (since #1004) counts entries by state, resident mesh, material, script, texture, and environment versions, versions also held by an outside lease, resident mesh GPU and CPU bytes, (since #1031) resident texture GPU bytes, and (since #1035) resident environment GPU bytes, for [measurements](performance.md#what-is-measured).

Since #1001, `MeshAsset` also keeps a CPU `MeshGeometry` (local positions, triangle indices, and bounds) from the OBJ loader for [picking](inspector.md#picking), at 12 bytes per vertex and 4 per index; providers may omit it, and such meshes cannot be picked.

`Mesh` now owns its vertex/index allocations, is noncopyable/nonmovable, and rolls back a partial upload. Final mesh destruction destroys its buffer handles. Since [#996](rhi.md), the device revokes the handles at once and releases the native buffers only after every frame that could use them completes. Frame command buffers no longer retain resources themselves. Shutdown drains submitted work before releasing native resources. These counts are distinct from catalog entries and CPU leases.

Device sessions expose an expiring lifetime guard. Metal invalidates it at shutdown and creates a fresh guard on initialize. Mesh draw/release skips an expired session, and a mesh provider from an old session refuses uploads; leases can safely be destroyed after the device itself. A nonempty old lease still owns its CPU value, but `mesh().valid()` and registry handle resolution fail after device shutdown. Create a new provider/registry for the new device session. Normal Engine teardown still destroys content and registries before the device. GraphicsDevice manages the guard and handle validity for every backend.

Buffers, textures, samplers, and pipelines share the [#996 retirement rules](rhi.md) and session/generation-checked handles. Per-frame upload memory and frames-in-flight limits are described with the [graphics device](rhi.md#frame-pacing-and-upload-memory). `MetalDevice::native_buffer_count()` counts backend-owned buffers, including ones awaiting retirement; it is not native GPU memory usage.

## Integration, cost, and verification

The [basic scene catalog](../samples/basic_scene/assets/catalog.maya) gives the sample's pyramid and cube meshes, four material files, a script, and (since #1031) two textures persistent IDs: a 256×256 tile grid and its normal map, made procedurally. `basic.scene`'s mesh renderers reference those IDs. Since #1002, a [project file](projects.md#projects) names a project's content root and catalog; `open_project_assets` reads the catalog into a registry rooted at the content root. The editor and, since #1003, the [player](play.md#the-player) open projects this way and never use the FileSystem search roots for project content. Since #998, [render extraction](renderer.md) acquires the meshes and materials each frame and shades with the material factors.

Registry ID lookup is average O(1); source loading happens once per resident version. Lease acquire/copy increments a shared ownership count. Registration canonicalizes paths and performs filesystem checks. `evict_unused()` and metadata export are O(catalog size), and catalog metadata is retained for the registry lifetime. These choices need profiling against production asset counts; the correctness tests do not promise game capacity.

The CPU-only [asset suite](../tests/asset_tests.cpp) covers sharing across Worlds, persistent catalog reconstruction/project relocation, versioned reload and rollback, stale/wrong-type/foreign handles, path boundaries, explicit retries, partial-upload cleanup, device/session teardown, and resource counters across repeated lifetimes. [Metal tests](../tests/rhi_tests.cpp) check real buffer sharing/release; [desktop tests](../tests/desktop_lifecycle_tests.cpp) release the final mesh lease after encoding a draw and then submit/drain the work. Existing application smoke tests exercise catalog loading and teardown.

Validation on 23 September 2026: all targets built in Release without compiler diagnostics; all 14 CTest entries passed (nine CPU/CLI and five GPU/smoke entries). The final asset suite passed 14 cases / 307 assertions in Release and UBSan after strengthening the device-destruction cleanup assertions. World and existing core CPU suites also passed under UBSan. Clang static analysis of registry.cpp, file_provider.cpp, and model_loader.cpp reported no findings. The asset-test link includes only MayaAssets, MayaWorld, and Catch2.

A bounded CPU benchmark completed 1,000/10,000/100,000 shared material lease acquisitions, handle resolutions, releases, and explicit eviction. It is not the #1004 production asset/rendering benchmark. The ASan/UBSan asset executable built but timed out before any output after 15 seconds and was terminated. The local ASan startup limitation recorded in #991/#992 remains unresolved; this is not an ASan pass.
