# Scene editing, undo, and selection

[Issue #1000](https://work.rezee.app/kash/issues/1000) makes the editor's scene editable. [SceneEditor](../apps/editor/scene_editor.hpp) owns the open scene's World, applies every edit as one atomic World batch, and records reversible history by persistent EntityId. It also keeps the selection and tracks unsaved changes. It lives in `MayaEditor` but has no UI dependency; the [Hierarchy panel](editor.md#panels-and-layout) is its first user.

## Edits

| Edit | Behavior |
| --- | --- |
| `create(name, parent, extra)` | A new entity with a name, an identity transform, and optional extra components (the Hierarchy offers Empty, Camera, and Directional light), added as the last child or root. The new entity is selected. |
| `rename(id, name)` | Sets the name component, adding it if missing. |
| `duplicate_selection()` | Copies each selected subtree with new EntityIds. Selected entities inside another selected subtree are copied once, with their ancestor. Parent references inside the copy are remapped, and children keep their order. Each copy goes right after its original; the root copy is named `Name (1)`, `Name (2)`, and so on, using the first free number. The copies become the selection. |
| `delete_selection()` | Deletes each selected subtree and clears the selection. |
| `move(id, target, placement)` | Moves an entity inside, before, or after `target`, or to the end of the roots. The world pose is kept: the new local transform is the inverse of the new parent's world matrix times the entity's world matrix. |
| `set_component(id, value)` | Replaces a whole validated component value, for the inspector in #1001. |

Rejected edits change nothing, record nothing, and return the reason: a missing entity, or a parent without a transform. Moves reject cycles (under itself or a descendant) and entities without transforms. They also reject a pose the new parent cannot represent, such as a rotated child under a rotated, nonuniformly scaled parent, which would need shear. An edit that changes nothing, such as renaming to the same name, is not recorded. Values go through the same schema validation as scene files and inspectors.

Root order is editor state: the World has no order for roots, and scene files order roots by EntityId. The editor keeps roots in the order they were created or moved, starting from the file's order. It is not yet saved; #1002 decides whether scene files record it. Sibling order under a parent is World state and is saved.

## History

Each step stores the before and after records (parent and schema components) of just the entities it touched, and the before and after child lists of just the parents it touched. Undo applies the "before" side and redo the "after" side, using one routine that turns the current World into the target in a single batch:

1. Detach survivors that change parent, and every current child of a re-ordered parent.
2. Destroy removed entities. A removed descendant goes with its removed ancestor, and survivors were detached first.
3. Create added entities, with their saved IDs and components.
4. Add, replace, or remove components that differ.
5. Re-attach each re-ordered parent's children, last to first, since reparenting inserts at the front.

Because history holds persistent IDs and values, never runtime handles, undo and redo stay valid across deletion and recreation. An entity deleted and restored by undo comes back with the same ID, and so does a copy removed by undo and restored by redo. After applying, the editor reads back just the touched entities, since the World may normalize values such as quaternions. Its mirror of the scene therefore always matches the World.

- **Groups.** `begin_group(label)` / `end_group()` merge every edit in between into one step. For each entity or child list, the step keeps the first "before" and the latest "after". Groups nest, and only the outermost records. Continuous drags and slider edits in #1001 will use this so that one drag is one undo step. Undo and redo are unavailable while a group is open.
- **Limit.** At most 256 steps; the oldest are dropped.
- **Unsaved changes.** `dirty()` is true when the history position differs from the one at the last open or `mark_saved()`. Undoing back to the saved position makes the scene clean again. Editing after an undo discards the redo steps. If the saved position was among them, the scene stays dirty until the next save. The top bar shows a "modified" tag.
- **Selection.** Each step records the selection before and after it, so undo restores what was selected. The selection never holds a missing entity: every edit, undo, and redo prunes it.
- **Scene replacement.** Opening a scene creates a new SceneEditor, so history, selection, and saved position start fresh. A failed open keeps the current session.

## Cost

Each edit touches only the entities and child lists it changes, so a rename, move, delete, undo, or redo costs about the same in any size of scene. Opening a scene reads the whole World once. Duplication scans all names to choose a free `(N)` suffix. Release measurements on this machine, for scenes of 1,000, 10,000, and 50,000 entities in groups of ten:

| Operation | 1,000 | 10,000 | 50,000 |
| --- | --- | --- | --- |
| Rename | 0.04 ms | 0.03 ms | 0.04 ms |
| Undo rename | 0.01 ms | 0.01 ms | 0.01 ms |
| Delete a subtree / undo it | 0.02 / 0.07 ms | 0.02 / 0.07 ms | 0.04 / 0.08 ms |
| Duplicate a subtree | 0.19 ms | 0.85 ms | 4.2 ms |

A first version recaptured and diffed the whole scene on every edit. That cost 8 ms per rename at 1,000 entities and 240 ms at 50,000, too slow for an edit on every frame of a drag.

## In the editor

The Hierarchy lists the scene in display order, each entity marked by type:

- **Select:** click to select; ⌘-click toggles and Shift-click adds. Click empty space to clear the selection, or press Esc.
- **Rename:** double-click, F2, or Enter renames inline. Enter or clicking away commits; Esc cancels.
- **Move:** drag a row onto the top or bottom quarter of another row to place it before or after, onto the middle to make it a child, or onto empty space to move it to the end of the roots. An accent line or outline shows where it will go.
- **Menus:** right-click a row for Rename, Duplicate, Move to root, Create child, and Delete. The **+** button, or right-clicking empty space, creates an entity.
- **Keys:** while the Hierarchy has focus, Delete or Backspace deletes the selection.
- **Shortcuts:** anywhere in the editor, ⌘Z undoes, ⇧⌘Z or ⌘Y redoes, and ⌘D duplicates. They are ignored while a text field has the keyboard, so Backspace and ⌘Z in a name field edit the text, not the scene. The undo and redo buttons in the top bar show what they will undo or redo.

The Inspector shows the primary selection's name, ID, and components. Failed edits appear in Diagnostics under "edit". Picking in the viewport, selection highlighting, and property editing are #1001; saving is #1002.

## Tests

- [scene_editor_tests.cpp](../tests/scene_editor_tests.cpp) tests SceneEditor directly and compares canonical scene text before and after. It covers:
  - create, rename, and subtree deletion, undone and redone by ID;
  - duplication with remapped IDs, nested selection, ordering, and copy naming;
  - moves that keep world poses, sibling ordering, root order, and rejected cycles, shear, and entities without transforms;
  - groups, nested groups, and unsaved-changes state across undo, redo, and discarded branches;
  - selection pruning and restoration;
  - the history limit.

  A seeded random test runs 250 mixed edits, undos, redos, and selections, four times, and checks after every step that the editor's mirror matches the World. Undoing everything must reproduce the initial scene text, and redoing everything the final text. New entity IDs come from a deterministic source, so each seed replays exactly.
- [editor_tests.cpp](../tests/editor_tests.cpp) drives the Hierarchy with synthetic input. It covers row order, clicking and ⌘-clicking, and clearing on empty space; Backspace deleting a subtree with ⌘Z and ⇧⌘Z restoring and removing it, including its selection; and inline renaming, where Backspace only edits text and ⌘Z is ignored in a text field. It also covers a real drag and drop that reparents while keeping the world pose, and opening a scene resetting history while a failed open keeps it.
