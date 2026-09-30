# Inspector, gizmos, and picking

[Issue #1001](https://work.rezee.app/kash/issues/1001) makes the selection editable. The Inspector's controls are generated from the shared [property schemas](properties.md). Transform gizmos move, rotate, and scale the selection in the viewport, and clicking the viewport selects what is under the pointer. Every change goes through validation and [SceneEditor](editing.md), so it is undoable. A drag, a slider, or typing into a field is one undo step.

## Inspector

For the primary selection, the Inspector shows:

- the name, as a large field, committed when editing ends;
- the entity's ID;
- one section per component in schema order, each with a trash button to remove it;
- an **Add component** menu for the components the entity lacks.

Each property's control comes from its schema descriptor:

| Type / presentation | Control |
| --- | --- |
| scalar | Drag field, clamped to the schema range (an exclusive bound stays just inside it), with its unit (`m`). Radian properties (`rad`) show and edit degrees. |
| vector3 | Three drag fields with red, green, and blue axis marks. |
| vector3, color | Color editor (RGB with a swatch), allowing values above 1 (HDR). |
| quaternion | Euler angles in degrees, rotation = Rz·Ry·Rx. They are kept stable during a drag so they do not jump at ±180°, and gimbal lock is handled. |
| boolean | Checkbox. |
| choice | Menu of the schema's choices (a light's kind, a collider's shape, a body's motion). |
| integer | Drag field clamped to the schema range. A collision group shows a menu of the project's group names instead. |
| flags, collision mask | A menu with one check box per collision group, named as in the project, plus All groups and None. Its label summarizes the mask: All groups, None, one group's name, or how many. |
| mesh / material / script reference | Menu of catalog assets of that kind, plus None. An asset of that kind dragged from the [Assets panel](projects.md#the-assets-panel) can be dropped on it. A reference to an asset missing from the catalog shows as "Missing" in amber; an asset that failed to load shows in red, with the reason as a tooltip. |

A light shows only the properties its kind uses: range for point and spot lights, cone angles for spot lights. A collider shows only its shape's size (half extents for a box, a radius for a sphere, a radius and half height for a capsule), and a kinematic body hides the initial velocities that only dynamic bodies use. Descriptions appear as tooltips. The editor camera's settings sit below the selection's components.

**Validation.** Every edit runs `edit_properties` with the project's asset context, and SceneEditor checks the result against the same catalog (a fix in #1002: before it, choosing any asset was refused). A rejected value leaves the component unchanged, and the reason appears in red under the entity's name. Examples are a near clip beyond the far clip, a nonpositive scale, or an asset of the wrong kind. The same happens for World rejections such as a transform the hierarchy cannot represent.

**One step per interaction.** Activating a control (starting a drag or clicking into a field) opens an [undo group](editing.md#history). Every change it makes applies immediately, so the viewport updates live, and deactivating it closes the group as one step labelled "Edit ⟨component⟩". A group left open by a control that disappeared mid-drag is closed at the end of the frame. **Typing** into a drag field (⌘-click or double-click it) applies only when entry ends (Enter or clicking away), never per keystroke. A half-typed number such as "9" on the way to "900" therefore never becomes an edit.

### Physics components

Collider, Rigid body, and Physics settings are added from **Add component** like any other component ([physics](physics.md#authored-bodies)). Under a physics component, the Inspector notes what Play will refuse, or where a collider belongs:
- a rigid body without a collider on its entity or below it;
- a rigid body on an entity that is not a root, or that is scaled;
- a collider that is part of the rigid body on an ancestor (named).

Collision group names come from the project; its Collision groups window renames them ([projects](projects.md#collision-groups)).

### Script components

A Script component (#1018, [scripting](scripting.md)) shows its script asset, then one row per property the script declares, sorted by name, labelled with the declaration's label or its name. Each row edits the stored value of that name, or shows the default until one is stored:

| Declared type | Control |
| --- | --- |
| number | Drag field clamped to the declared range, with its unit. |
| integer | Drag field clamped to the declared range. |
| boolean | Checkbox. |
| string | Text field, applied when entry ends. |
| vector | Three axis drag fields. |
| color | Color editor (RGB, HDR). |
| entity | Menu of the scene's entities in hierarchy order, plus None; a reference to an entity no longer in the scene shows as Missing. |

Edits are "Edit Script" undo steps, like other properties.

Below the fields, **Open script** opens the file in an external editor ([Assets panel](projects.md#the-assets-panel)), and the Inspector notes:
- **Scripts that cannot be read or compiled,** in red, with the reason, such as `scripts/broken.luau:2: Expected identifier when parsing expression, got 'return'`. A script that never compiled shows no fields. One that compiled before keeps the fields of that last good version, which stays in use.
- **Stored values that do not fit,** in amber: one for a property the script no longer declares, of another type, or out of range. **Remove unused values** drops those for undeclared properties, as one undo step.

The editor compiles each script when the project opens, and again whenever its file changes ([reload](scripting.md#reload)), so the fields follow the script as it is edited.

## Gizmos

The gizmo sits on the primary selection when it has a transform. [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) (MIT) draws it and handles its dragging; it is pinned to commit `22a6c900`, the last one before it moved to the ImGui 1.92.8 drawing API. It is styled to the theme: red, green, and accent axes, gold while dragging.

- **Modes:** move (W), rotate (E), and scale (R), from the tool bar in the viewport's top-left corner or the keys.
- **Space:** X toggles world and local space. Scale is always local.
- **Snap:** hold ⌘ while dragging to snap to 0.25 m, 15°, or 0.1 scale.
- **Frame:** F (or the tool bar) moves the editor camera to frame the selection's bounds from the current direction.

The keys work while the pointer is over the viewport and no control is active, so typing never switches tools. The keys to fly the camera only work while the right button is held ([input routing](editor.md#input-routing)).

**Hierarchy.** The gizmo works on the entity's world matrix. Each change is converted back to a local transform, inverse(parent world) × new world, and must decompose into a valid positive-scale transform. A move always maps back. Some rotations under a rotated, nonuniformly scaled parent would need shear: they are refused, the entity stays where it was, and the Inspector says why. A drag is one undo step, "Move ⟨name⟩", "Rotate ⟨name⟩", or "Scale ⟨name⟩". A drag interrupted by the selection vanishing closes its group.

## Picking

A left click in the viewport, away from the gizmo and tool bar, selects what is under the pointer. ⌘-click or Shift-click toggles it in the selection. Clicking empty space clears the selection, and the Hierarchy expands and scrolls to the picked entity.

- **Meshes.** Picking uses the snapshot the viewport last rendered, so it matches exactly what is on screen. It is a documented bounded technique, sketched in code after this list: a ray from the editor camera through the pixel, then a linear pass over the snapshot's instances. Each instance's local bounding box is tested with the ray transformed into its space; exact two-sided triangle tests run only for instances whose box the ray enters.
- **Overlaps.** Hits are sorted nearest first. Clicking the same spot again (within 4 pt) steps to the next object behind, then wraps around.
- **Cameras and lights.** These have no mesh, so they appear as small icons at their positions. The icons take precedence within 13 pt, since they are drawn on top.
- **Outline.** Selected meshes are outlined with their oriented bounding box: gold for the primary, dimmer for the rest.

```text
cost = O(instances) box tests + O(triangles of meshes whose box the ray enters)
```

The mesh test needs a CPU copy of the triangles. `MeshAsset` now keeps a `MeshGeometry` (local positions, indices, and bounds) from the OBJ loader. It costs 12 bytes per vertex and 4 per index, and a mesh without it cannot be picked. Release measurements per click, with every tenth instance a 2,004-triangle mesh:

| Instances | 1,000 | 10,000 | 50,000 |
| --- | --- | --- | --- |
| Pick | 0.33 ms | 2.2 ms | 10 ms |

That is fine for picking on click; a spatial acceleration structure is the next step if hover picking or much larger scenes need it.

## Tests

- [picking_tests.cpp](../tests/picking_tests.cpp) covers:
  - ray/box and ray/triangle distances, misses, parallel rays, and back faces;
  - picking overlapping instances nearest first, with exact distances;
  - a child of a rotated, scaled parent hit at its world position;
  - view rays that project back onto their pixels;
  - Euler round trips, gimbal lock, and no negative zero.
- [scene_editor_tests.cpp](../tests/scene_editor_tests.cpp) covers adding and removing components as undoable steps, removal rules for transforms in a hierarchy, and schema validation of whole-component edits.
- [editor_tests.cpp](../tests/editor_tests.cpp) drives the editor with synthetic input:
  - clicking the viewport picks the nearest object, clicking again steps to the one behind, ⌘-click adds, empty space clears, and camera and light icons are clickable;
  - a 20-frame Inspector drag is one undo step that undo reverses;
  - typing an invalid near clip is refused with a reason and changes nothing;
  - a real gizmo drag along X moves only X, as one "Move" step;
  - gizmo matrices under a rotated, nonuniformly scaled parent map moves back and refuse shear;
  - tool keys work over the viewport, but not over the Hierarchy or while a field is being typed in.
