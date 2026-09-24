# Editor shell, viewport, and input routing

[Issue #999](https://work.rezee.app/kash/issues/999) turns the editor from a single offscreen view into a dockable editor window. It has hierarchy, viewport, inspector, assets, and diagnostics panels, an editor camera, and explicit routing of keyboard and mouse input between the UI and the viewport. Everything here belongs to the `MayaEditor` target. The runtime, player, and sample do not link the UI framework ([isolation](#runtime-isolation)).

## UI framework

The editor uses [Dear ImGui](https://github.com/ocornut/imgui), the candidate named in [DOC-58](https://work.rezee.app/kash/docs/58), pinned to `v1.91.9b-docking` through FetchContent. The docking branch provides resizable, rearrangeable panels. 1.91.9b is the last release before 1.92 changed how renderer backends manage textures, and upgrading will need [ui_renderer.cpp](../apps/editor/ui_renderer.cpp) updated. Only ImGui's core sources are built, into `MayaImGui`, and only when `MAYA_BUILD_EDITOR` is on. Maya supplies both backends itself:

- **Platform:** window events come from Maya's own [input events](#host-integration), not from ImGui's GLFW backend, so the editor has no GLFW dependency.
- **Renderer:** [UiRenderer](../apps/editor/ui_renderer.hpp) draws ImGui's draw lists through `GraphicsDevice`, not Metal, so the UI obeys the same validation, upload memory, and retirement rules as everything else.

[maya_imgui_config.h](../apps/editor/imgui/maya_imgui_config.h) selects 32-bit indices, so every index offset meets the RHI's 4-byte alignment and one draw list addresses all its vertices without per-command vertex offsets. It also removes obsolete ImGui APIs. `imgui.ini` is disabled: the layout is rebuilt at startup until project settings exist to store it.

## Look and type

[editor_theme.cpp](../apps/editor/editor_theme.cpp) defines the editor's visual language:

- **Palette:** a neutral near-black palette. Surfaces step up in lightness from chrome, to panels, to fields; borders are hairlines. Color is reserved for state: a single cool accent for focus, selection, and the active tab's overline; green, amber, and red for ready, warning, and problems.
- **Type:** [Inter](https://github.com/rsms/inter) is the UI face: Regular at 14 pt for text, SemiBold for emphasis and for 11 pt uppercase section captions. [Geist Mono](https://github.com/vercel/geist-font) is used for numbers, IDs, and paths. Both are under the SIL Open Font License and ship in [resources/fonts](../resources/fonts) with their licenses. Fonts are rasterized at the display scale, so text is sharp on Retina displays. Missing or invalid font data falls back to ImGui's built-in font with a diagnostic.
- **Icons:** [Phosphor Icons](https://phosphoricons.com), Light weight (MIT), chosen over the widely used Lucide set for a less generic look. Its thin, even strokes match Inter. The font ships as `resources/fonts/Phosphor-Light.ttf` and is merged into the text fonts, so labels can mix text and icons. Only the glyphs named in [editor_icons.hpp](../apps/editor/editor_icons.hpp) enter the font atlas; add a constant and its codepoint there to use another icon. Icons mark panel tabs, entity types (camera, light, mesh), asset kinds, log severity, the status bar state, and the viewport's navigation hint. Without the icon font, panels show text only and Diagnostics notes it.
- **Shape:** 6 pt rounding on fields, generous padding, and no window menu buttons or title bars.
- **Tabs:** flat and 30 pt tall, on a strip in the panel color with a hairline beneath it. Unselected labels are muted, with a soft hover. The visible tab's label is bright, with a short 2 pt underline: the accent when that panel has focus, grey otherwise. ImGui has no per-state tab label colors, so `theme::decorate_tabs` restyles the tabs after they are drawn. Tab bars may be drawn by the dock space or by a panel's `Begin`, so the shell applies the tab padding and muted text to both (`begin_panel`). Each panel then reserves exactly its tab bar's height, which a test checks.
- **Layout:** a 40 pt top bar with the Maya mark, the open scene, and frame time; a 26 pt status bar with state (ready, flying, or the number of problems), object count, and viewport size; and docked panels between them. Property grids put muted labels on the left and full-width controls on the right. Icons mark entity types in the hierarchy; dots mark load state in the asset list.

Numbers that change every frame are sampled four times a second in the Diagnostics panel. The desktop host likewise refreshes the window title four times a second, so neither flickers.

## Structure

| Piece | Responsibility |
| --- | --- |
| [EditorApplication](../apps/editor/editor_application.cpp) | Engine `Application`: reads this frame's events and window metrics from `Input`, forwards cursor-capture requests to the host, and renders into the acquired surface. |
| [EditorShell](../apps/editor/editor_shell.hpp) | Owns the ImGui context, the panels, the viewport's `RenderTarget`, the renderer, the opened World and registry, and the diagnostics log. `update` routes input and builds the UI; `render` draws the viewport, then the UI. |
| [InputRouter](../apps/editor/input_router.hpp) | Sends each event to exactly one owner, the UI or the camera; see [input routing](#input-routing). |
| [EditorCamera](../apps/editor/editor_camera.hpp) | Free-flight camera that is tool state, not a World entity: position, yaw, pitch, `CameraComponent`, and speed. |
| [editor_theme](../apps/editor/editor_theme.hpp) | Palette, style, fonts, and small shared widgets: captions, status dots, pills, and property rows. |
| [editor_icons](../apps/editor/editor_icons.hpp) | Named Phosphor icon glyphs and the atlas glyph ranges. |
| [SceneEditor](../apps/editor/scene_editor.hpp) | The open scene's World, atomic edits, undo/redo history, selection, and unsaved-changes state ([scene editing](editing.md)). |
| [UiRenderer](../apps/editor/ui_renderer.hpp) | Uploads ImGui vertices and indices to frame upload memory, turns clip rectangles into scissor rectangles, and maps texture IDs to device textures (font atlas and viewport). |

`update` runs in `Application::on_update` and `render` in `on_render`, so the UI is laid out, including the viewport's size, before the viewport renders. The viewport's image then shows the scene rendered in the same frame.

## Panels and layout

On its first frame the shell docks the panels: Hierarchy on the left, Inspector on the right, Assets and Diagnostics as tabs below, and the Viewport in the center. Panels can be resized by dragging their splitters, rearranged, undocked, or tabbed. Each panel's title carries an icon; the part after `###` in its name (for example `###Viewport`) is its stable ID for layout and tests. The mouse cursor changes over splitters and text fields.

- **Hierarchy:** the scene's entities in display order, each marked by type. Select, rename, create, duplicate, delete, and drag to reparent or reorder, all undoable; see [scene editing](editing.md).
- **Viewport:** the scene from the editor camera, with a small hint about the navigation controls.
- **Inspector:** the editor camera's position, speed, and vertical field of view (text fields), and the selection's name, ID, and components. Property editing is #1001.
- **Assets:** catalog entries by file name, with kind and load state; hovering shows the path and ID, or a failed entry's error.
- **Diagnostics:** see [diagnostics](#diagnostics).

Until project open/save lands in #1002, the editor opens the sample project's `basic.scene` and edits it in memory only; nothing is written back. If the sample cannot be found, it starts with empty panels, and the diagnostics panel explains why.

## Viewport size and display scale

ImGui lays out in points; the framebuffer is in pixels, twice as many per axis on a Retina display. The viewport's target is `viewport_pixels(panel points, scale)`: whole pixels, rounded down, so the image never extends past its panel. The image is drawn at `pixels / scale` points: one texel per framebuffer pixel, never stretched or filtered. `RenderTarget::resize` does nothing while the size is unchanged, so steady frames never allocate. A new panel size, a window resize, or moving to a display with a different scale reallocates once, and the old textures are retired after their frames complete. Fonts are rasterized at the display scale and redrawn at their size in points, and they are rebuilt when the scale changes.

A window that is appearing has no final size on that frame: the first frame, or a tab just brought forward. The viewport skips that frame instead of allocating a target that would be discarded. A hidden viewport tab or a zero-sized panel renders nothing. When the window is minimized (zero-sized), `update` skips the frame, navigation ends, and the diagnostics log records "viewport paused". Restoring resumes with the existing target when the size is unchanged.

## Input routing

The router gives each event one owner. Its context is whether the viewport image was hovered at the end of the previous frame.

| State | Event | Goes to |
| --- | --- | --- |
| Not navigating | Right button pressed over the viewport | Starts navigation: the cursor is captured, and any active text field loses focus. |
| Not navigating | Scroll over the viewport | Dollies the camera along its view. |
| Not navigating | Everything else: keys, text, clicks, motion, scrolling elsewhere, focus | The UI. |
| Navigating | W/A/S/D, Q/E (down/up), Shift (4× speed) | Held camera movement. |
| Navigating | Pointer motion | Camera look. |
| Navigating | Scroll | Dolly. |
| Navigating | Right button released, Escape, or loss of window focus | Ends navigation: the cursor is released, held keys are cleared, and the UI gets the current pointer position. |
| Navigating | Other keys and text | Dropped. They are neither shortcuts nor text while flying. |

Keyboard input therefore reaches the camera only while the right button is held over the viewport. Typing into a field, even with the pointer over the viewport, never moves the camera. Keys held when navigation starts do not count until pressed again, and keys still held when it ends stop moving the camera. The viewport disappearing (minimized, hidden, or zero-sized) also ends navigation. Escape does not quit the editor (`DesktopOptions::escape_closes = false`); close the window instead.

## Host integration

These additions to the desktop host are generic, not editor-specific:

- `Input::events()` holds the frame's ordered `KeyEvent` (with modifiers), `TextEvent` (Unicode code points), `MouseMoveEvent` (points), `MouseButtonEvent`, `ScrollEvent`, and `FocusEvent`. The host clears them after each tick. Held-key state for simple controllers such as the player's fly camera is unchanged.
- `Input::window_metrics()` gives the window size in points and in framebuffer pixels, published before each tick.
- `Input::request_cursor_capture(bool)` asks the host to capture or release the cursor after the tick. Smoke runs ignore it.
- `PlatformServices` provides clipboard access and cursor shapes without a window-library dependency. ImGui's clipboard and cursor use them.
- `KeyCode` names the keys the editor needs; values are GLFW key codes.
- The Metal layer's `contentsScale` follows the window's backing scale, and it is updated whenever the framebuffer size changes. Before this, a Retina display showed every frame as 1× content, soft and blocky. `MetalDevice::surface_scale()` reports it, and a desktop test checks it against the window.
- `DesktopOptions::escape_closes` and `DesktopOptions::device` (passed to `Engine::initialize`). The editor reserves 16 MiB of upload memory per frame for UI geometry and scene constants.

## Rendering the UI

`UiRenderer::render(draw_data, destination, clear_color)` runs inside a frame with no pass open. It uploads a small projection from points to clip space. Then, for each draw list, it uploads the vertices (20-byte `ImDrawVert`) and 32-bit indices to frame upload memory. For each command it sets a scissor rectangle and the command's texture, then issues an indexed draw from the upload slice. Clip rectangles are converted to destination pixels and clamped to the destination; empty rectangles are skipped. A texture ID whose texture is not live draws nothing and is counted in `stats().missing_textures`. It returns the first device error after closing its pass.

To support this, the RHI gained three general features (see [the graphics device](rhi.md)):

- `PipelineDesc::blend` (`BlendMode::alpha` is straight-alpha source-over);
- `set_scissor`, validated against the pass attachments and reset by each pass;
- `draw_indexed(const TransientSlice&, ...)`, which reads indices from this frame's upload memory.

[editor_ui.metal](../resources/shaders/metal/editor_ui.metal) is the UI shader.

## Diagnostics

The Diagnostics panel shows the current state:

- viewport size in pixels and points, scale, and allocation count;
- drawn, hidden, and skipped mesh renderers;
- frames, waits, and upload-memory high water against capacity;
- live resources and pending retirements.

It also lists this frame's extraction problems, such as missing meshes and materials, and a log. The log records:

- scene and catalog load errors;
- viewport target failures and reallocations;
- renderer errors, for example a pipeline that fails to compile;
- GPU errors taken from the device, also written to stderr;
- UI pass errors;
- minimize and restore.

Repeated messages are merged with a count, and the log keeps at most 200 entries. Viewport and renderer problems do not stop the editor: the rest of the UI keeps drawing, and a later frame recovers once the cause is gone. Only a failure of the UI pass itself is returned to the Engine.

## Runtime isolation

`MayaImGui` is fetched and built only when `MAYA_BUILD_EDITOR` is on; a player-only configuration never downloads it. CTest runs [check_no_editor_ui.cmake](../cmake/check_no_editor_ui.cmake) over `MayaRuntime`, `MayaRenderer`, `MayaDesktop`, `maya_player`, and `maya_sample`, and fails if any contains an ImGui symbol. The same check on `maya_editor` must fail, which proves it can detect the framework.

## Limitations

- Layout, window placement, and editor camera state are not saved between runs.
- No IME composition, gamepad or keyboard navigation of the UI, or multiple OS windows (ImGui multi-viewports).
- The cursor-shape service covers arrow, text, hand, and horizontal/vertical resize. GLFW 3.3 has no diagonal or "not allowed" cursors.

## Tests

- [editor_tests.cpp](../tests/editor_tests.cpp) (`maya_editor`, labels `cpu;editor`) runs the full shell on the null device with a 1280×720-point window at 2× scale. It covers:
  - pixel sizing, and the dock layout with all five panels;
  - the image in points times the scale equalling the target, with no reallocation over steady frames;
  - one reallocation per resize or scale change, and minimize and restore;
  - clicking into the speed field and typing WASD and digits while holding keys, without moving the camera, while the field receives the text;
  - navigation taking focus from the field and moving along the view;
  - release, Escape, focus loss, and a vanishing viewport releasing the cursor;
  - scissors staying inside the window and the viewport texture being sampled;
  - injected texture, pipeline, and upload failures landing in diagnostics.

  Router and camera rules are also tested directly.
- [editor_gpu_tests.cpp](../tests/editor_gpu_tests.cpp) (`maya_editor_gpu`, labels `gpu;editor`, Metal API validation) renders the whole editor into a window-sized texture at 2× scale and reads it back. It checks lit scene pixels at the viewport's center and dark panel pixels, then resize, a scale change, and minimize and recover. With `MAYA_EDITOR_CAPTURE=<path>.ppm` it writes the image for review.
- [rhi_validation_tests.cpp](../tests/rhi_validation_tests.cpp) and [rhi_tests.cpp](../tests/rhi_tests.cpp) cover scissor, blend, and upload-slice index rules, and a Metal half-transparent draw clipped to half the target.
- [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) runs the real editor and player through window resizes, publishing metrics as the host does.

#999 validation on 24 September 2026:

- Fresh Release and UBSan builds of every target report no diagnostics from Maya sources. All 26 CTest entries pass (19 CPU/CLI/isolation, 7 GPU/smoke), three times in Release and once under UBSan. GPU entries run with Metal API validation.
- The editor suite (10 CPU cases / 620 assertions and 1 Metal case / 50 assertions) passed 20 repeated Release runs. The RHI suite has 16 cases / 7,051 assertions, and the desktop suite 5 / 804.
- A player-only configuration (`MAYA_BUILD_EDITOR=OFF`) builds without fetching Dear ImGui. The isolation checks find no ImGui symbols in MayaRuntime, MayaRenderer, MayaDesktop, `maya_player`, or `maya_sample`, and do find them in `maya_editor`.
- Mutation checks, each rebuilt from scratch, fail the suite:
  - movement keys reaching the camera over the viewport without navigation;
  - no viewport focus when navigation starts, which leaves the text field active;
  - no skip of the appearing frame, which adds an extra allocation;
  - the viewport image drawn in pixels instead of points (stretched);
  - Escape not ending navigation;
  - UI draws without scissor rectangles.

  A redundant internal ImGui call that the focus change already covered was removed.
- Clang static analysis of the editor sources, the desktop host and window, and graphics_device.cpp reports no findings.
- In a Release run of the full editor at 1280×720 points at 2× scale, building the UI took about 0.02 ms per frame. Rendering the viewport and UI and submitting took 0.11 ms per frame on Metal (CPU time), with a 67 KiB upload high water mark. This is not #1004 evidence.
- The rendered editor was inspected in an image captured by the Metal test (`MAYA_EDITOR_CAPTURE`).
- Interactive checks on a real display are still pending: panel dragging, Retina and non-Retina displays, focus changes between applications, and cursor capture and release. Automated tests cover the same rules with synthetic events and window metrics, but they do not exercise GLFW's event delivery or the macOS cursor.
- ASan remains blocked by the local startup limitation recorded since #991.
