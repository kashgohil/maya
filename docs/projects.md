# Projects, scene files, and assets in the editor

[Issue #1002](https://work.rezee.app/kash/issues/1002) connects the editor to projects on disk. A project file names its content folder. The editor opens, creates, and saves scenes there, and never loses unsaved changes without asking. An Assets panel lists the project's scenes, meshes, and materials, and places or assigns them by dragging. Scenes store only catalog IDs, never file paths, and a project keeps working wherever its folder is moved.

## Projects

A project is a folder with a `project.maya` file:

```text
maya-project 1
content "assets"
catalog "catalog.maya"
startup "basic.scene"
group 1 "Player"
```

| Field | Meaning |
| --- | --- |
| `content` | The content root: where the catalog, sources, and scenes live, relative to the project file's folder. `"."` is the folder itself. |
| `catalog` | The [asset catalog](assets.md#catalog-and-material-files), relative to the content root. |
| `startup` | Optional. The scene opened with the project, relative to the content root. |
| `script_work <n>` | Optional: the [scripts'](scripting.md#sandbox-and-limits) work budget, in safepoints per hook call, from 1,000 to 1,000,000,000. Without it, 1,000,000. |
| `script_memory <n>` | Optional: the scripts' memory limit per play session, in MiB, from 1 to 4,096. Without it, 64. |
| `group <n> "<name>"` | Optional, any number, after the others: the name of [collision group](#collision-groups) *n* (0–15). Groups named as by default are not written. |

Every path must be relative and may not contain `..`, so a copied or moved project keeps working. [project.hpp](../include/maya/assets/project.hpp) in `MayaAssets` reads and writes the file (`read_project`, `write_project`) and opens it (`open_project`). The result holds canonical absolute paths derived from the project file. `Project::resolve` maps a content-relative path into the content root and refuses any path that would leave it, including through a symlink. Nothing about a project comes from the working directory or from the application's resource search roots. Only the editor's own shaders and fonts are found that way.

The sample project is [samples/basic_scene/project.maya](../samples/basic_scene/project.maya).

### Collision groups

A project names the 16 physics [collision groups](physics.md#authored-bodies) that colliders choose from. Group 0 is `Default`; the others are unnamed until named and show as "Group *n*". Names are 1 to 32 bytes of printable text without quotes or backslashes, and without leading or trailing spaces. `Project::settings` holds them with the rest of the file's settings, and `save_project` writes the file back, replacing it through a temporary file beside it.

In the editor, the project's name in the top bar opens a menu with **Collision groups**. Its window has one field per group; a name applies when its field loses focus, and the project file is saved at once. An empty field makes the group unnamed. Renaming is not an undoable scene edit, and an invalid name is refused with the reason. `EditorShell::rename_collision_group` does the same from code.

**Starting the editor.** `maya_editor [project]` opens a project file, or a folder containing `project.maya`. A relative path is taken from where the editor was started. Without an argument, the editor opens the sample project when it can find it. A project that cannot be opened leaves an empty editor with a notice explaining why. The editor also prints the opened project, or the reason, to standard error. Opening a project reads its catalog into a new asset registry and opens its startup scene. If the project has no startup scene, or the startup scene fails to open, the editor starts with a new scene instead and shows why in a notice.

## Scene files

The top bar reads **Maya / project / scene**. The scene's name opens the scene menu:

| Item | Shortcut | Action |
| --- | --- | --- |
| A scene of the project | | Opens it. The open one has a check mark. |
| New scene | ⌘N | A never-saved scene with a camera and a directional light. It is not dirty until edited. |
| Save | ⌘S | Saves to the scene's file, or asks for one if the scene has never been saved. |
| Save as… | ⇧⌘S | Asks for a path, then saves there. The saved file becomes the open scene. |
| Refresh project | | Rereads the catalog and lists scene files again (see [Assets](#the-assets-panel)). |

**Save as** takes a path relative to the content folder, such as `levels/intro`. `.scene` is added when there is no extension, and missing folders are created. Paths outside the content folder are refused, and so are other extensions and folders. Replacing a different existing file takes a second Save; the dialog says the file exists first.

**Saving** goes through `save_scene_file`, which validates the whole scene, writes a temporary file beside the target, and renames it over the target. A failed save therefore leaves every file as it was, and the scene stays unsaved. The reason appears in a notice (or in the dialog that asked for the save) and in Diagnostics under "scene". Reasons include a read-only folder and a reference to an asset that is no longer in the catalog. Saving an unchanged scene again writes the same bytes.

**Opening** a scene replaces the editing session (history and selection start empty). A scene that cannot be opened leaves the open scene and its unsaved changes alone. The notice shows the first problems with their line numbers, and the rest are in Diagnostics. Examples are a malformed file, a missing file, and an asset ID that is not in the catalog.

**Root order is saved.** The World has no order for root entities, so `capture_scene` still orders roots by EntityId. The editor saves roots in its hierarchy order instead: `SceneEditor::document()` lists them in display order, each followed by its descendants, and scene files keep entity order. When the editor opens a scene, the roots appear in the file's order. So an arrangement made in the Hierarchy survives saving and reopening, and a resave of an unchanged scene is byte-identical.

## Unsaved changes

Opening another scene, creating one, or closing the editor while the scene has unsaved changes shows a prompt:

- **Save** saves, then carries on. A never-saved scene first asks for a path. Cancelling that cancels the whole action.
- **Don't save** carries on and leaves the file as it was.
- **Cancel** (or Esc) changes nothing.

Undoing back to the saved state counts as no changes, so nothing is asked. A save that fails keeps the prompt open with the reason.

**Closing** (the close button or ⌘Q) now asks the application first. `Application::on_close_requested()` returns false to keep the window open, and `Engine::request_close()` forwards the question. The desktop host withdraws the window's close flag and keeps running. Once the person decides, the editor closes through a new `PlatformServices::request_close`, and the host then asks again. Smoke runs close without asking. An application that throws while answering does not keep the window open. Asking to close while the prompt is already up makes closing the pending action.

## The Assets panel

The Assets panel is the bottom panel shown first (Diagnostics is its neighbouring tab). It has a filter and a refresh button above three columns:

- **Scenes:** the `.scene` files in the content folder, with the open scene highlighted. Double-click one to open it.
- **Meshes:** meshes from the catalog, by file name.
- **Materials:** materials from the catalog, each with a swatch of its base color.

Scripts in the catalog ([scripting](scripting.md)) are not listed yet; they are assigned in the Inspector until [#1020](https://work.rezee.app/kash/issues/1020) adds them here.

A row may show a status: **missing** (amber) when its file is not in the content folder, **failed** (red) when loading failed, or **not loaded** for a mesh no frame has drawn yet. Materials load when their row is shown, since they are small CPU data. The tooltip shows the path, the ID, and the reason for a problem. The right-click menu has **Place in scene** (meshes), **Assign to selection**, **Reload** (read the file again), and **Copy ID**.

| Gesture | Result |
| --- | --- |
| Drag a mesh into the viewport | A new entity named after the file, placed where it is dropped. It rests on the surface under the pointer, else on the ground plane (y = 0) within 500 m, else 5 m in front of the camera. It is lifted by how far the mesh reaches below its origin, so it sits on the surface. |
| Drag a material onto an object in the viewport | Assigns it to that object. |
| Drag a mesh or material onto a Hierarchy row | Assigns it to that entity. An entity with a transform but no mesh renderer gets one. |
| Drag a mesh onto the Hierarchy's empty space | Places it in view, as a double-click does. |
| Drag onto an Inspector mesh or material field | Chooses it, when the kind matches. |
| Double-click a mesh | Places it where the center of the view meets the scene. |
| Double-click a material | Assigns it to every selected entity. |

Each placement or assignment is one undo step. Duplicating an instance with ⌘D (from [#1000](editing.md)) keeps its references. Placed meshes have no material until one is assigned, and draw with the default white factors. References always hold the catalog's AssetId. The Inspector's asset fields show a failed asset in red with the reason, and an ID missing from the catalog in amber.

**Refresh** rereads the catalog into a new registry and keeps it only if it is valid; otherwise the notice says why and the previous catalog stays. It also lists scene files again and rechecks which asset files exist. This picks up new catalog entries, new scene files, and restored files without restarting.

**Missing assets stay diagnosable.** A project whose files are partly missing still opens. Its scenes open, and references are never changed or dropped. The Assets panel marks the missing files. The renderer skips meshes it cannot load and uses the fallback material, and reports each in Diagnostics. Restoring a file and choosing Reload or Refresh brings it back.

## Decisions and limits

- **Material factors are not edited in the editor yet.** Materials are files. Edit a material file, then choose Reload in its row. Editing factors in place means writing asset files, with their own undo, outside scene history. That is a separate piece of work.
- **No native file dialogs.** A project is chosen on the command line, and scenes live inside its content folder.
- **Catalog changes are made to the file.** Importing new sources and editing the catalog in the editor are later work.
- Layout, window placement, and editor camera state are still not saved between runs.

**A fix to #1001.** `SceneEditor` validated created and edited components without an asset resolver, so it refused every nonempty mesh or material reference with "A catalog resolver is required". In practice, choosing an asset in an Inspector field never worked. `SceneEditor::set_validation_context` now supplies the resolver. The editor gives it the open project's catalog, including after a refresh. References must be in the catalog and of the right kind.

## Cost

Release measurements on this machine, with scene entities in groups of ten, each with a mesh renderer:

| Operation | 1,000 | 10,000 | 50,000 |
| --- | --- | --- | --- |
| Open a scene (read, validate, build the World) | 5 ms | 53 ms | 310 ms |
| Save a scene (validate, write, flush, rename) | 6.6 ms | 15 ms | 62 ms |
| `SceneEditor::document()` | 0.04 ms | 0.41 ms | 2.7 ms |

| Catalog entries | 6 | 1,000 | 10,000 |
| --- | --- | --- | --- |
| Open the project or refresh it | 0.4 ms | 20 ms | 190 ms |
| Editor frame with the Assets panel shown | 0.02 ms | 0.02 ms | 0.02 ms |

Opening a project and refreshing it register every catalog entry, which canonicalizes each path, and check that each file exists. The panel reads the catalog only then. It filters only when the filter text or the lists change, and each column draws only its visible rows. A first version copied and filtered the whole catalog every frame and searched the missing list for each row, which cost 21 ms per frame at 10,000 entries. Listing scene files stops after 4,096 directory entries, and Diagnostics says so.

## Tests

- [project_tests.cpp](../tests/project_tests.cpp) (in `maya_asset_tests`) covers:
  - project files that round-trip, with or without a startup scene;
  - rejection of bad headers, versions, field orders, and trailing data;
  - rejection of absolute paths and `..` paths;
  - opening from a file or a folder, and from a relative path in another working directory, for the original and a copy;
  - paths resolving only inside the content root, including a symlink escape;
  - clear reasons for a missing project, a missing content root, and an invalid content path.
- [editor_project_tests.cpp](../tests/editor_project_tests.cpp) drives the editor with synthetic input, on copies of the sample project in scratch folders:
  - **The authoring workflow.** A project with a renamed content folder is opened by a path relative to another working directory. Then, through real input: a new scene with ⌘N; the cube dragged from Assets onto the ground, resting on it; red dropped on it in the viewport; a value edited; ⌘D; blue dropped on the copy's Hierarchy row; a mesh dropped on empty Hierarchy space and undone; a pyramid double-clicked in and moved first. ⌘S asks for `levels/workshop`. The scene is reopened from yet another working directory: its saved text matches exactly (IDs, values, hierarchy, root order), references are catalog IDs, everything draws, and a resave is byte-identical.
  - **Unsaved changes.** Cancel, Esc, Don't save, and Save when opening another scene. A failed save keeps the prompt and the changes. ⌘N asks. Closing asks, and supersedes an open prompt. Closing a never-saved scene asks for a path and closes once it is saved. Cancelling the path cancels the close. A clean scene, including one undone back to its saved state, closes without asking.
  - **Failures.** Paths outside the content folder, other extensions, and folders are refused, with and without the dialog. Replacing another file takes a second Save. A read-only folder fails with a notice and changes nothing. A catalog that loses an entry blocks the save and names the ID. Malformed scenes, unknown asset IDs, missing files, and paths outside the project fail to open with their reasons and keep the open scene.
  - **Missing files.** They are listed. The meshes that use them are skipped and the materials report their reason. Restored files, a new catalog entry, and a new scene appear on Refresh. A broken catalog keeps the previous one.
  - **Startup.** A missing startup scene gives a new scene and a notice; none gives a new scene. A project that fails to open keeps the open one.
  - **Inspector fields.** They take dropped assets of the right kind as one undo step, and check references against the catalog.
- [scene_editor_tests.cpp](../tests/scene_editor_tests.cpp) covers root order from the file, `document()` order, and a byte-identical round trip.
- [engine_tests.cpp](../tests/engine_tests.cpp) covers close requests: allowed, refused, throwing, and with nothing running. [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) covers setting and withdrawing a real window's close flag.
