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

`AssetLease<T>` owns a shared immutable loaded version and is copyable. Its `value()` reference must not outlive the lease; reading an empty lease throws `logic_error`. A lease can be held in a native component or renderer-owned cache. Destroying an entity/World releases any leases it owns; its plain MeshRendererComponent references alone have no residency effect. Future extraction (#998) will acquire and retain the assets needed by rendering submissions.

`AssetHandle<T>` is non-owning and contains a registry lifetime token, slot, and version generation. A replaced/evicted/unavailable version or a foreign registry fails resolution. A successful reload publishes a fresh generation. Existing leases still own their previous version, even though their old handle no longer resolves through the registry. Registry tokens and runtime handles are not serialized. Registry slots are append-only for its lifetime; there is no unregister or slot-reuse API yet.

## Catalog and material files

The version-one catalog stores kind, hexadecimal high/low ID words, and a quoted project-relative path:

```text
maya-assets 1
mesh 6d617961 1 "models/tree.obj"
material 6d617961 2 "materials/bark.mat"
```

`write_asset_catalog(stream, registry.records())` writes metadata only. `read_asset_catalog(stream)` checks syntax/version/ID words and returns records or a diagnostic; register each record in a fresh registry before publishing that project. Registration validates identity, kind, duplicate sources, and project boundaries. Parsing/registration does not load resources. Loading a malformed catalog must discard the unpublished registry, not expose a partially registered project. Filesystem I/O and allocation exceptions remain ordinary exceptions. This catalog is not the [scene format](scene.md) (#995); scenes store only AssetIds and validate them against the catalog.

Relative paths resolve strictly against the supplied existing project directory. Missing files can be registered so their identity/diagnostics survive. Absolute paths, traversal outside the root, and symlink escapes are rejected; symlinks are rechecked on every actual load. The registry does not consult the working directory or application search roots. Moving the project directory preserves references when its relative layout and catalog move with it. Changing paths/IDs, dependency remapping, file watching, and atomic catalog saves are later editor/import work.

The initial material source contains linear base-color RGBA, metallic, and roughness factors:

```text
maya-material 1
base_color 0.8 0.5 0.2 1
metallic 0
roughness 0.7
```

All six factors must be finite and in [0,1]. Field order is fixed in version one; extra data, unsupported versions, and malformed values fail. These immutable material values are ready for renderer integration; this issue does not implement PBR shading, textures, material graphs, or shader cooking. `fallback_material()` returns explicit magenta/opaque, nonmetallic, rough data for callers choosing a fallback. Missing meshes skip their draw. Neither fallback replaces the missing reference's ID or reports the source as successfully loaded.

## Loading and reload

| State | Behavior |
| --- | --- |
| `unloaded` | Catalog entry exists without a resident version. First acquire loads synchronously. |
| `loading` | Provider is preparing a candidate. Read-only info queries can observe this state. Nested loads/catalog mutation are rejected as busy; eviction does nothing. |
| `ready` | Registry owns a cache lease. Repeated acquire returns the same immutable version. |
| `failed` | Initial load failed. Acquires return its saved diagnostic without retrying every frame; use explicit reload to retry. |

`info(id)` returns a copy of metadata/state/generation/diagnostics. A failed reload leaves a previously ready version and its generation available, with the failure recorded separately. The provider's candidate is private until validation succeeds; incomplete/failed candidates release their resources. Allocation failure restores the previous loading state and propagates; ordinary provider exceptions become diagnostics. Generation exhaustion refuses further publication rather than wrapping.

`AssetProvider` is replaceable and returns owned candidates plus diagnostics. The initial `FileAssetProvider` calls the existing OBJ loader through a checked entry point, validates material files, and uploads one vertex/index buffer pair per loaded mesh version. OBJ support is deliberately limited to positive indices and triangular faces with optional UVs/normals. Malformed numbers, missing coordinates, invalid indices, unsupported polygons, and empty geometry report file/line diagnostics before upload. Absent UVs/normals retain the legacy zero defaults; authored normals are needed for useful lighting. The legacy `ModelLoader::load_obj` adapter retains application search-root resolution.

Loading is **synchronous** and registry/device access, including final mesh-lease release and cache eviction, belongs to one owner thread. Call it at a controlled scene/asset boundary, not accidentally in a per-draw path. The separation between persistent identity, immutable candidate publication, provider, and residency leaves room for asynchronous CPU import and dependency bundles. Worker scheduling, cancellation/request tokens, dependency graphs, retries, load budgets, glTF, and cooking are not implemented. An async extension must tag completions by registry/request generation, reject stale results, and keep GPU upload/publication on the owning thread. Future composite assets must retain dependency leases for the complete lifetime of their loaded version; a plain dependency AssetRef does not pin it.

## Release and GPU retirement

The registry retains one cache lease for each published version. `evict_unused()` removes versions whose only remaining owner is that cache, preserving catalog entries as unloaded. Call it at scene-unload/maintenance boundaries. There is no hidden timed/LRU eviction; explicit maintenance is required to bound the resident cache. Old versions after reload live only as long as their remaining leases. Registry destruction drops its cache ownership; outstanding leases remain valid while their device session remains available.

`Mesh` now owns its vertex/index allocations, is noncopyable/nonmovable, and rolls back a partial upload. Final mesh destruction destroys its buffer handles. Since [#996](rhi.md), the device revokes the handles at once and releases the native buffers only after every frame that could use them completes. Frame command buffers no longer retain resources themselves. Shutdown drains submitted work before releasing native resources. These counts are distinct from catalog entries and CPU leases.

Device sessions expose an expiring lifetime guard. Metal invalidates it at shutdown and creates a fresh guard on initialize. Mesh draw/release skips an expired session, and a mesh provider from an old session refuses uploads; leases can safely be destroyed after the device itself. A nonempty old lease still owns its CPU value, but `mesh().valid()` and registry handle resolution fail after device shutdown. Create a new provider/registry for the new device session. Normal Engine teardown still destroys content and registries before the device. GraphicsDevice manages the guard and handle validity for every backend.

Buffers, textures, samplers, and pipelines share the [#996 retirement rules](rhi.md) and session/generation-checked handles. Per-frame upload memory and frames-in-flight limits are described with the [graphics device](rhi.md#frame-pacing-and-upload-memory). `MetalDevice::native_buffer_count()` counts backend-owned buffers, including ones awaiting retirement; it is not native GPU memory usage.

## Integration, cost, and verification

The [basic scene catalog](../samples/basic_scene/assets/catalog.maya) gives the sample pyramid a persistent ID. The sample resolves that catalog through the application FileSystem, then loads it through a project-rooted registry. The legacy Scene accepts a mesh lease and retains it through rendering/teardown; procedural sample meshes continue to own their allocations directly. World render extraction and material-factor rendering remain #998.

Registry ID lookup is average O(1); source loading happens once per resident version. Lease acquire/copy increments a shared ownership count. Registration canonicalizes paths and performs filesystem checks. `evict_unused()` and metadata export are O(catalog size), and catalog metadata is retained for the registry lifetime. These choices need profiling against production asset counts; the correctness tests do not promise game capacity.

The CPU-only [asset suite](../tests/asset_tests.cpp) covers sharing across Worlds, persistent catalog reconstruction/project relocation, versioned reload and rollback, stale/wrong-type/foreign handles, path boundaries, explicit retries, partial-upload cleanup, device/session teardown, and resource counters across repeated lifetimes. [Metal tests](../tests/rhi_tests.cpp) check real buffer sharing/release; [desktop tests](../tests/desktop_lifecycle_tests.cpp) release the final mesh lease after encoding a draw and then submit/drain the work. Existing application smoke tests exercise catalog loading and teardown.

Validation on 23 September 2026: all targets built in Release without compiler diagnostics; all 14 CTest entries passed (nine CPU/CLI and five GPU/smoke entries). The final asset suite passed 14 cases / 307 assertions in Release and UBSan after strengthening the device-destruction cleanup assertions. World and existing core CPU suites also passed under UBSan. Clang static analysis of registry.cpp, file_provider.cpp, and model_loader.cpp reported no findings. The asset-test link includes only MayaAssets, MayaWorld, and Catch2.

A bounded CPU benchmark completed 1,000/10,000/100,000 shared material lease acquisitions, handle resolutions, releases, and explicit eviction. It is not the #1004 production asset/rendering benchmark. The ASan/UBSan asset executable built but timed out before any output after 15 seconds and was terminated. The local ASan startup limitation recorded in #991/#992 remains unresolved; this is not an ASan pass.
