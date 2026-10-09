# Scene persistence

[Issue #995](https://work.rezee.app/kash/issues/995) adds versioned scene save/load. `MayaScene` (`Maya::Scene`) links only `MayaWorld`; `MayaRuntime` links it publicly. The [scene API](../include/maya/scene/scene_io.hpp) has no renderer, window, editor, or asset-loading dependency. Asset references are checked through a caller-supplied resolver, normally [`asset_property_context(registry)`](properties.md).

## Save and open

```cpp
#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"

const auto context = maya::asset_property_context(registry);
auto problems = maya::save_scene_file(project_root / "levels/forest.scene",
                                      maya::capture_scene(*active_world), context);
if (!problems.empty()) { /* report problems; the previous file is unchanged */ }

auto opened = maya::open_scene_file(project_root / "levels/forest.scene", context);
if (!opened) { /* report opened.diagnostics; *active_world is untouched */ return; }
active_world = std::move(opened.world); // replace only after a complete load
```

`capture_scene` copies schema components and hierarchy from a World into a detached `SceneDocument`. `validate_scene` checks a document. `instantiate_scene` validates it, then builds a **new** World in one command commit. `read_scene`/`write_scene` work on text or streams; `load_scene_file`, `open_scene_file`, and `save_scene_file` add file handling. Each function reports failure as a list of `SceneDiagnostic` values. Returned documents and Worlds are empty or null unless the whole operation succeeded.

A World cannot be moved, so the caller keeps the active scene behind an owner such as `std::unique_ptr<World>` and replaces it only after `open_scene_file` succeeds. Loading never modifies an existing World. Asset leases are unaffected: loading creates plain `AssetRef` values and neither loads nor pins assets. Residency is acquired later by consumers such as [render extraction](renderer.md).

## File format, version 1

```text
maya-scene 1

entity ca 1
  component maya.name 1
    value "Main \"camera\""
  component maya.transform 1
    translation 0 1.7 5
    rotation 0 0.14943814 0 0.9887711
    scale 1 1 1
  component maya.camera 1
    vertical_fov 1.0471976
    near_clip 0.1
    far_clip 1000
end

entity 7e 1
  parent ca 1
  component maya.mesh_renderer 1
    mesh 6d617961 1
    material none
    visible true
end
```

The format is line-based UTF-8 text, so it diffs and merges well and is consistent with the [asset catalog](assets.md). A file starts with `maya-scene <format version>`. Each `entity <high> <low>` block may have one `parent <high> <low>` line before its components. Each `component <stable name> <schema version>` line is followed by one line per property, written as `<stable property name> <value>`. `end` closes the entity. Indentation is decorative.

| Value | Encoding |
| --- | --- |
| Entity/asset IDs | Two nonzero-together hexadecimal 64-bit words, the same as the asset catalog. |
| Numbers | Shortest decimal text that round-trips the float exactly. `nan`, `inf`, and out-of-range values are rejected. |
| Vector / quaternion | 2 or 3 numbers for a vector, 4 for a quaternion (`x y z w`). |
| Boolean | `true` or `false`. |
| Text | Double-quoted UTF-8. `\"`, `\\`, `\n`, `\r`, `\t` escapes; other control characters use `\xHH`. `\x` is only for control characters. |
| Choice | Stable choice name, such as a light kind (`directional`, `point`, `spot`), a collider shape (`box`, `sphere`, `capsule`), or a body motion (`dynamic`, `kinematic`). |
| Integer | A whole number, such as a collision group: `group 3`. |
| Flags | Hexadecimal with a `0x` prefix, such as a collision mask: `mask 0xffff`. |
| Asset reference | `none` when unassigned, otherwise the persistent AssetId words. Never a path, lease, handle, or registry token. |
| Script values | One line: a count (at most 4096), then each value's name, type (`number`, `integer`, `boolean`, `string`, `vector`, `color`, or `entity`), and data in the encodings above, in stored order. An entity value is `none` or its ID words: `values 2 speed number 1.5 target entity 6d617961 300` ([scripting](scripting.md#script-assets-and-the-component)). |

Writing keeps the document's entity order. `capture_scene` orders roots by EntityId, since the World has no root order, and follows each root depth-first by its descendants. The editor writes roots in its hierarchy order instead, and reads them back in file order ([#1002](projects.md#scene-files)). Components are ordered by ComponentId and properties by schema order. Entity order also records sibling order: children appear in their parent's child order, and loading reproduces that order. Saving, reopening, and saving again produces byte-identical output. When reading, blank lines, `#` comment lines, tabs, and CRLF line endings are accepted. Comments and formatting are not preserved on the next save.

Only persistent data is written: entity IDs, parent IDs, and the [schema](properties.md) properties. World tokens, slots, generations, pointers, derived matrices, asset leases/handles, GPU resources, and source paths never appear. Other native component types, such as components holding asset leases, are runtime state and are not saved. A scene can therefore be saved from any World lifetime and loaded into another.

`entity`, `parent`, `component`, `end`, and `maya-scene` are reserved words and must not be used as property names (a test enforces this). Version 1 files store **every** property of each component. An omitted property is a `missing_property` error rather than a default, so a future default change cannot silently reinterpret old files.

## Validation and diagnostics

Every load and save validates the complete document before publishing or writing anything. Components go through the same `edit_properties`/`validate_component` boundary as inspectors and scripts. Rotations are normalized; all other invalid values are rejected rather than clamped.

| `SceneError` | Cause | Typical remedy in the message |
| --- | --- | --- |
| `malformed` | Bad header, syntax, token count, number, escape, invalid UTF-8, missing `end`, zero ID. | Located by line; says what the line expects. |
| `unsupported_version` | Newer format or component version, or an older version without a migration. | Open with a newer Maya build. |
| `unknown_component` / `unknown_property` | Name not in this build's schema. | Lists supported components. |
| `missing_property`, `duplicate_property`, `duplicate_component` | Incomplete or repeated data. | Names the component/property. |
| `duplicate_entity` | EntityId defined twice. | Gives both line numbers. |
| `invalid_value` | Range/relationship/text-encoding failure from the shared schema. | Schema message with entity and property. |
| `invalid_hierarchy` | Missing or self parent, cycle, or a parent link without Transforms on both ends. | Names both entities. |
| `missing_asset` / `wrong_asset_type` | AssetId not in the catalog, or registered as the other kind. | Asset ID, with "restore its catalog entry or clear the reference". |
| `validation_context_required` | Nonempty reference with no resolver. | Pass `asset_property_context(registry)`. |
| `io_error` / `world_rejected` | File system failures; World commit failure after validation. | States that the existing file/scene was not changed. |

Diagnostics carry a code, a message, a 1-based source line when read from text, and the EntityId when known. File functions prefix messages with the path. Syntax and version problems stop parsing and return one diagnostic. Value, asset, identity, and hierarchy problems are collected, so one load reports every missing asset. At most `max_scene_diagnostics` (64) are returned; the last says that more were omitted.

An asset absent from the catalog is an error, so a scene never loads with an unknown reference. A catalog entry whose file is missing or fails to load is still a valid identity. The scene loads, and the missing file is reported when the asset is acquired, with the [fallback policy](assets.md) applied (missing meshes skip their draw; materials may use the magenta fallback). Validation reads only catalog metadata and never loads files.

## Failure safety

- **Loads** parse and validate privately, then build a separate World with one atomic `commit`. Any failure returns no World, so the caller's active World and its handles are never touched.
- **Saves** validate the document and encode all text before touching the file system. The text is written to a hidden `.<name>.tmp-<pid>-<token>` file in the destination directory. It is flushed with `F_FULLFSYNC` (falling back to `fsync`) and renamed over the destination. The directory is then synced as a best effort. A failed validation, missing directory, unwritable directory, destination that is not a regular file, failed or partial write, failed flush, or failed rename all remove the temporary file and leave the previous scene byte-for-byte intact.
- An existing file keeps its permission bits. A destination that is a symbolic link is replaced by a regular file rather than written through. There is no cross-process locking; with concurrent writers, the last rename wins.

## Versions and migration

The format is `maya-scene 1`. Component schemas are version 1, except `maya.camera`, version 2 since #1032 (exposure and tone mapping), `maya.light`, version 2 since #1034 (candela and shadows), and `maya.transform`, version 2 since #1065 (a double translation). Newer format or component versions are rejected with `unsupported_version` before any World is built, and version 0 is malformed.

**Additive migration** (#1032). Every schema change so far only added properties. Each property records the component version that added it (`PropertyDescriptor::since`), so an older component migrates without a table of steps: its file must hold exactly the properties its version had (a later one is an `unknown_property` for that version, and a missing one is `missing_property`), and every property added since takes its documented default. A `maya.camera 1` loads with `exposure 0` (EV100) and `tone_mapping agx`. A change that is not additive (a renamed or reinterpreted property) needs its own migration, keyed by component and version, where the parser checks versions.

**Lights** (#1034), the first migration that is not additive. `maya.light 1` stored point and spot intensities in lumens; version 2 stores candela, as glTF does, so a version-1 point or spot light's `intensity` is divided by 4π as it loads (1,256.6 lumens become 100 candela). Directional intensities are lux in both and load unchanged. The four shadow settings take their defaults: shadows on, bias 1 and normal bias 1 texel, and a 60 m shadow distance. The migration runs on the raw values before validation, in the scene codec's `migrate`.

**Transforms** (#1065). `maya.transform 2` stores the translation as three doubles, written in the shortest form that reads back exactly. A version-1 translation is read as the float it was written as and widened, so older scenes load exactly as they did; the next save writes version 2. Rotation and scale stay float.

When a schema changes, [its version increments](properties.md#persistence-and-schema-evolution) and a migration keyed by component and source version is added where the parser checks component versions. A migration rewrites the raw property list into the current shape. The result is then validated through the same schema APIs, so a failed migration produces diagnostics and publishes nothing. Loading an older file never rewrites it. The next explicit save writes current versions, and editors should confirm or back up before overwriting an older file. The #1002 editor writes a scene only on an explicit Save. Unknown data is rejected, not dropped; a preservation mode would need to be designed explicitly.

## Cooked cells

A world's cells are cooked to a packed binary ([scene_binary.hpp](../include/maya/scene/scene_binary.hpp), #1064), so loading one costs decoding, not parsing ([worlds](world.md#worlds-on-disk)). It holds every entity's ID and parent and every property of every component, as typed values, at this build's schema versions, after a header ("mayacel1") and a fingerprint of those schemas (names, versions, property IDs and types). It is a cache format, never authored: a cook made by a build with other schemas is refused and cooked again from the text. Decoding validates as reading text does (`decode_scene_binary` returns the same diagnostics), and a truncated or damaged buffer is a diagnostic, never a crash. In the cook cache an entry's kind is `cell`; in a package, a `.cell` file in the cook cache's checked envelope.

## Numerical stability

Save/reopen preserves float values, and translations' doubles, exactly. [Transform validation](spatial.md) now keeps quaternions within `quaternion_unit_tolerance` (4e-7) of unit length bit-exact instead of renormalizing them again. Renormalization had shifted about 1.4% of already-normalized rotations by one ulp on each pass. Each save/load cycle revalidates, so rotations would otherwise drift. Hand-written non-unit quaternions are still normalized on load.

## Threading, cost, and limits

Capture, validation, and instantiation follow the World's single-owner-thread rule. Call `capture_scene` outside World callbacks and commits. Resolvers must be read-only. File I/O is synchronous and reads the whole file into memory; there is no size cap, streaming, or cell partitioning yet. Prefab instances and cross-scene references are separate work. Undo and unsaved-changes handling live in the editor ([editing](editing.md), [projects](projects.md)).

A bounded Release CPU run on the development machine gave these timings. Each entity has a name, transform, and mesh renderer, grouped under parents of ten:

| Entities | Text | Capture | Write | Read + validate | Instantiate |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 0.2 MB | 1.2 ms | 3.5 ms | 8.1 ms | 4.6 ms |
| 10,000 | 2.5 MB | 4.0 ms | 14.8 ms | 35.6 ms | 9.7 ms |
| 100,000 | 25.5 MB | 20.2 ms | 95.2 ms | 397 ms | 112 ms |

These are local correctness-scale checks, not production budgets or #1004 benchmark evidence.

## Verification

```bash
cmake --build build --target maya_scene_tests
ctest --test-dir build -L scene --output-on-failure
```

[scene_tests.cpp](../tests/scene_tests.cpp) links only MayaScene, MayaAssets, and Catch2. It covers:

- Exact round trips of IDs, sibling order, nested nonuniform transforms, cameras, lights, and difficult text, with byte-identical resaves.
- Shared mesh/material references that resolve to a single loaded registry version.
- Output that does not depend on World lifetime, slot churn, or creation order.
- Hand-written files with comments, CRLF, and non-unit quaternions.
- 28 located malformed or unsupported inputs.
- Collected duplicate, value, asset, and hierarchy diagnostics, plus the diagnostic cap.
- Failed loads that leave the active World untouched, and a catalog with missing assets.
- Save failures: validation, a read-only directory, a partial write injected with `RLIMIT_FSIZE`, a missing directory, and a directory destination. Each one preserves the last valid file and leaves no temporary files.
- A 4,096-deep hierarchy plus 2,000 more entities.

A spatial test checks 100,000 random rotations for normalization idempotence.

Validation on 23 September 2026:

- A fresh Release build of every target produced no diagnostics from Maya sources. All 16 CTest entries pass (11 CPU/CLI, 5 GPU/smoke). The GPU entries need `MAYA_RESOURCES` when the build directory is outside the checkout.
- The scene suite (10 cases / 183,957 assertions), World (28 / 647,642), properties (12 / 351), and assets (14 / 307) pass in Release and UBSan.
- Clang static analysis of scene_document.cpp, scene_codec.cpp, and spatial.cpp reports no findings.
- The ASan/UBSan scene executable builds but produced no output within 15 seconds and was terminated. This is the startup limitation recorded since #991; ASan is not recorded as passing.
