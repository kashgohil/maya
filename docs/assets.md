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
```

`write_asset_catalog(stream, registry.records())` writes metadata only. `read_asset_catalog(stream)` checks syntax/version/ID words and returns records or a diagnostic; register each record in a fresh registry before publishing that project. Registration validates identity, kind, duplicate sources, and project boundaries. Parsing/registration does not load resources. Loading a malformed catalog must discard the unpublished registry, not expose a partially registered project. Filesystem I/O and allocation exceptions remain ordinary exceptions. This catalog is not the [scene format](scene.md) (#995); scenes store only AssetIds and validate them against the catalog.

Relative paths resolve strictly against the supplied existing project directory. Missing files can be registered so their identity/diagnostics survive. Absolute paths, traversal outside the root, and symlink escapes are rejected; symlinks are rechecked on every actual load. The registry does not consult the working directory or application search roots. Moving the project directory preserves references when its relative layout and catalog move with it. Changing paths/IDs, dependency remapping, file watching, and atomic catalog saves are later editor/import work.

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

Until [#1036](https://work.rezee.app/kash/issues/1036) caches cooked results, a PNG or JPEG source is cooked whenever its texture loads ([texture_cook.hpp](../include/maya/assets/texture_cook.hpp)):

1. **Decode** with stb_image (PNG and JPEG only, from memory) to straight-alpha RGBA8, exactly the stored values: no color management or premultiplication. Images above the device's largest texture are refused before their pixels are decoded. stb_image is not hardened against hostile files; it cooks the project's own content, and packaged games will read cooked KTX2 only ([#1039](https://work.rezee.app/kash/issues/1039)).
2. **Mips** with stb_image_resize2, each level halved from the one before: sRGB-correct with alpha-weighted color for color, every channel independent for data, and renormalized for normals (a straight-up normal where a texel holds no direction).
3. **Compress** with astcenc 5.7.0 at medium quality on every core, or keep RGBA8. A device that cannot sample ASTC gets RGBA8. The same source, settings, and device always give the same bytes, whatever the thread count.

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

## Loading and reload

| State | Behavior |
| --- | --- |
| `unloaded` | Catalog entry exists without a resident version. First acquire loads synchronously. |
| `loading` | Provider is preparing a candidate. Read-only info queries can observe this state. Nested loads/catalog mutation are rejected as busy; eviction does nothing. |
| `ready` | Registry owns a cache lease. Repeated acquire returns the same immutable version. |
| `failed` | Initial load failed. Acquires return its saved diagnostic without retrying every frame; use explicit reload to retry. |

`info(id)` returns a copy of metadata/state/generation/diagnostics. A failed reload leaves a previously ready version and its generation available, with the failure recorded separately. The provider's candidate is private until validation succeeds; incomplete/failed candidates release their resources. Allocation failure restores the previous loading state and propagates; ordinary provider exceptions become diagnostics. Generation exhaustion refuses further publication rather than wrapping.

`AssetProvider` is replaceable and returns owned candidates plus diagnostics. The initial `FileAssetProvider` calls the existing OBJ loader through a checked entry point, validates material files, and uploads one vertex/index buffer pair per loaded mesh version. OBJ support is deliberately limited to positive indices and triangular faces with optional UVs/normals. Malformed numbers, missing coordinates, invalid indices, unsupported polygons, and empty geometry report file/line diagnostics before upload. Absent UVs/normals retain the legacy zero defaults; authored normals are needed for useful lighting. Since #1033 the loader generates [MikkTSpace tangents](renderer.md#materials) per triangle corner and then shares the corners that agree (`generate_tangents`, `weld_vertices`), so a vertex on a UV seam may be split where it was not before. The legacy `ModelLoader::load_obj` adapter retains application search-root resolution.

Loading is **synchronous** and registry/device access, including final mesh-lease release and cache eviction, belongs to one owner thread. Call it at a controlled scene/asset boundary, not accidentally in a per-draw path. The separation between persistent identity, immutable candidate publication, provider, and residency leaves room for asynchronous CPU import and dependency bundles. Worker scheduling, cancellation/request tokens, dependency graphs, retries, load budgets, glTF, and cooking are not implemented. An async extension must tag completions by registry/request generation, reject stale results, and keep GPU upload/publication on the owning thread. Future composite assets must retain dependency leases for the complete lifetime of their loaded version; a plain dependency AssetRef does not pin it.

## Release and GPU retirement

The registry retains one cache lease for each published version. `evict_unused()` removes versions whose only remaining owner is that cache, preserving catalog entries as unloaded. Call it at scene-unload/maintenance boundaries. There is no hidden timed/LRU eviction; explicit maintenance is required to bound the resident cache. Old versions after reload live only as long as their remaining leases. Registry destruction drops its cache ownership; outstanding leases remain valid while their device session remains available.

`residency()` (since #1004) counts entries by state, resident mesh, material, script, and texture versions, versions also held by an outside lease, resident mesh GPU and CPU bytes, and (since #1031) resident texture GPU bytes, for [measurements](performance.md#what-is-measured).

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
