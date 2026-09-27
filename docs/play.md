# Playing scenes

[Issue #1003](https://work.rezee.app/kash/issues/1003) runs saved scenes. The player opens a project's scene and plays it. The editor plays the open scene in a separate World and returns to the untouched scene on Stop. Both drive the same play session: a fixed-step clock, gameplay input assigned to ticks, and an explicit, ordered list of simulation systems. It follows the [scheduling contract](architecture/scheduling-contracts.md).

## Play sessions

`MayaSimulation` / `Maya::Simulation` ([play_session.hpp](../include/maya/simulation/play_session.hpp), [simulation.hpp](../include/maya/simulation/simulation.hpp)) is CPU-only and links only MayaWorld and MayaScene. MayaRuntime links it, so the player and the editor share one implementation.

```cpp
auto started = maya::PlaySession::start(document, maya::asset_property_context(registry), maya::builtin_systems());
if (!started) { /* started.diagnostics (the scene) or started.error (a system) says why */ }
auto& session = *started.session;
session.input().feed(events_for_the_game);              // this host frame's gameplay events
if (const auto frame = session.update(wall_delta); !frame.error.empty()) { /* stopped: report */ }
render(session.world(), session.camera());               // the first camera in document order
```

- **Start.** `PlaySession::start` validates the scene document, just as loading a scene file does, and builds a new World from it. Then it starts each system in order. If the scene is invalid, the diagnostics say why. If a system throws from `start`, the error names it. The systems already entered are then stopped in reverse order, and nothing is left running.
- **Frames.** `update(wall_delta)` admits the frame's wall time to the clock and runs the ticks that are due. For each tick, the session latches that tick's input and runs every system in order. It then commits their commands as one atomic World batch and counts the tick.
- **Failures.** A system that throws, or a batch the World rejects, stops the simulation. The World stays as the last completed tick left it. The error names the tick and the cause, for example `Tick 2: Spin failed: ...`, and later updates return the same error.
- **End.** Destroying the session stops the systems in reverse order, then releases the World.

## The fixed clock

| Setting | Default | Behavior |
| --- | --- | --- |
| `ticks_per_second` | 60 | The fixed interval. Simulation time is the tick count times the interval; wall time accumulates in double precision. |
| `max_frame_delta` | 0.25 s | Wall time beyond it is not simulated and is reported as `rejected_time`. |
| `max_catch_up_ticks` | 4 | At most this many ticks run in one frame. Whole ticks beyond it are dropped and reported as `discarded_ticks`; the fraction is kept. |

- **Invalid time.** A negative or nonfinite delta is ignored and flagged.
- **Pause.** While paused, wall time does not accumulate and is not reported as rejected.
- **Step.** Step runs exactly one tick on the next update. Steps do not queue.
- **Resume.** Resuming starts from an empty accumulator, so there is no catch-up burst.
- **Alpha.** `alpha()` is the progress towards the next tick, and 1 while paused.

## Gameplay input

`GameInput` turns the window events owned by the game into one `InputFrame` per tick. The frame holds held keys and mouse buttons, the press and release edges since the previous tick, pointer movement in points, and scrolling.

- An edge goes to the next tick that runs.
  - A press and release before that tick report both, with the key not held.
  - A repeated press of a held key is not a new press.
- Catch-up ticks in the same frame see held state only.
- A frame that runs no tick keeps its edges and movement for the next one.
- Losing focus releases everything held as release edges, and the next pointer position starts a new baseline instead of a jump.

## Systems

A `SimulationSystem` has a name, `start`, `fixed_update`, and `stop`.

- **What an update sees.** `fixed_update` gets a `TickContext` with:
  - the World as the previous tick committed it;
  - the tick's command batch;
  - its `InputFrame`;
  - the tick index, simulation time, and fixed interval.
- **When writes appear.** A system writes through the commands. Its writes are visible to the next tick, not to later systems in the same tick, so the result does not depend on how many systems read a value.
- **One writer.** Each transform should have one writer.
- **Determinism.** The same scene, input, and frame times give the same World.

Until scripting and physics arrive, two built-in systems run in this order, driven by two authored components. The editor edits them like any other component, and scene files save them.

| Component | Properties | While playing |
| --- | --- | --- |
| `maya.fly_control` (ID 7) | `speed` (m/s, default 3), `look_sensitivity` (rad per point, default 0.0025) | Gameplay input flies the entity in its parent's space. WASD moves, Q and E go down and up, Shift is four times faster, and the mouse turns: yaw about +Y, then pitch, stopping just short of straight up or down. It moves only while there is input. The first turn replaces the authored rotation with yaw and pitch, dropping roll. |
| `maya.spin` (ID 6) | `axis` (local; its length is ignored), `speed` (rad/s, default π/4) | Turns the entity about the local axis at a constant rate. A zero axis or zero speed does nothing, and a negative speed turns the other way. |

The sample's spinning pyramid and flying camera are now these components in [basic.scene](../samples/basic_scene/assets/basic.scene). They are no longer sample code: `basic_scene.cpp` and `MayaBasicScene` are gone.

The play session implements the scheduling contract's clock and modes, its input assignment rules, and phases 1–3: committing, latching input, and fixed-update hooks. This tick's commands commit at the end of the tick, which is the contract's phase 1 of the next tick with nothing queued in between. Animation, physics, events, and post-physics hooks (phases 4–7) arrive with physics and scripting. Presentation interpolation is not implemented yet. Views show the latest completed tick, and `alpha()` is computed for when it is. Recorded replay and capture modes are also still contracts.

## The player

```bash
maya_player [project [scene]] [--smoke N]
```

- **Project.** The project is a `project.maya` file or its folder, relative to where the player starts. Without one, the player uses the sample project.
- **Scene.** The scene is relative to the project's content root. Without one, the player uses the project's startup scene.
- **View.** The player shows the scene's first camera, in document order, at the window's size.
- **Input.** Every window event goes to the game, and the cursor is captured. Escape closes the window.

It reports what it runs, `[Player] My Game / basic.scene: 6 entities`, and exits with:

| Exit code | When | Example message |
| --- | --- | --- |
| 0 | The window closed normally, or a smoke run completed. | |
| 1 | The project, catalog, or scene cannot be run, the scene has no camera, the device cannot start, or something failed while playing. | `[Player] second.scene cannot be run:` followed by `line 10: unknown component 'maya.name_is_bad'; ...` |
| 2 | Unexpected arguments. | `[Player] unexpected argument: c` |

Render problems, such as a missing mesh file, are printed when they change, and play continues with that mesh skipped. `maya_sample` is the same player on the sample project; it takes no project or scene.

## Play in the editor

The Play controls sit in the middle of the top bar:

| Control | Shortcut | Action |
| --- | --- | --- |
| Play / Stop | ⌘P | Plays the open scene, or stops. |
| Pause / Resume | ⇧⌘P | Holds the play World; resuming does not catch up. |
| Step | ⌥⌘P | While paused, runs exactly one tick. |

While playing, the top bar's rule turns to the accent. The status bar shows Playing or Paused, the tick, and the simulation time.

- **What plays.** The open scene as it is now, saved or not (`SceneEditor::document()`), in a new World.
- **The authored scene is locked.** Every edit, undo, and redo is refused with "Stop playing to edit the scene" (`SceneEditor::lock`). The Hierarchy still selects, but it does not create, rename, drag, or delete. The Inspector shows the play World's live values for the selection, read-only. Assets cannot be dropped. Saving saves the authored scene.
- **Two views.** The Game view shows the scene's first camera. The Scene view shows the editor camera, which flies with the right mouse button as usual. Gizmos and picking are hidden while playing. A scene without a camera plays in the Scene view only.
- **Stop.** The play World and everything made for it are released. The authored scene, its history, and its unsaved state are exactly as they were. The selection returns to what it was at Play. The editor camera, the panels, and the view choice stay as they are.
- **Scene changes.** Opening or creating a scene ends play. Closing the editor is not held up by play, since play has nothing of its own to save. Cancelling the unsaved-changes prompt keeps playing.
- **Time.** Play advances with the editor's frames, so it does not advance while the window is minimized. The Diagnostics panel shows the tick, the wall time the clock refused, and the ticks it dropped, so slowdown is visible.
- **Failures.** A scene that cannot be built, such as one referencing an asset missing from the catalog, shows a notice and does not start. A system that fails while playing stops play, with the tick and the reason in a notice and in Diagnostics under "play".

### Who gets the input

The [input router](editor.md#input-routing) now has three owners.

| State | Owner |
| --- | --- |
| Editing, or playing without the game having the input | The UI. The right mouse button over the viewport flies the editor camera, except over the Game view. |
| Flying | The editor camera, until the right button is released, Escape is pressed, or the window loses focus. |
| The game has the input | Starts with a left click on the Game view; that click only hands the input over. Every event goes to the play session, including ⌘ shortcuts and text, until Escape or a loss of focus takes it back. Held game keys are released then, and the cursor is captured meanwhile. |

A click on the Game/Scene toggle in the viewport's corner never hands the input to the game.

## Cost

Release measurements on this machine, for scenes with a spinning root and nine children per ten entities:

| Operation | 1,000 | 10,000 | 50,000 |
| --- | --- | --- | --- |
| Start a play session (validate, build the World, start systems) | 0.9 ms | 8.6 ms | 45 ms |
| One tick (spin writes every root, fly control has no input) | 0.012 ms | 0.11 ms | 0.55 ms |
| Stop (release the World) | 0.012 ms | 0.11 ms | 0.60 ms |

In the editor, Play also builds the document from the scene (2.7 ms at 50,000 entities, [projects](projects.md#cost)). A tick costs O(entities with the components the systems read). The World commits each tick's batch once.

## Tests

- [simulation_tests.cpp](../tests/simulation_tests.cpp) (`maya_simulation_tests`, CPU) covers:
  - **The clock:** exact multiples, fractional frames, the clamp and the catch-up cap with their reports, invalid deltas, pause, step, and resume without a burst.
  - **Game input:** one edge per tick, catch-up ticks, retained edges, key repeats, pointer baselines, and focus loss.
  - **Play sessions:** system order, and commits visible to the next tick; the first camera; the authored document left untouched; stop order.
  - **Session failures:** start failures stop systems in reverse order; invalid scenes and empty systems are refused; a failing tick keeps the last completed World.
  - **The built-in systems:** spin about a local axis, zero and negative speed, and normalized rotations; fly movement, speed, Shift, look, and the pitch limit.
  - **Determinism:** identical results for the same scene, inputs, and uneven frame times.
- [editor_play_tests.cpp](../tests/editor_play_tests.cpp) covers:
  - **Separate state:** Play shows unsaved edits, and the play World spins while the authored World does not change. Edits, undo, Delete, and inline rename are refused during play, while selection works. Stop restores the scene text, history, unsaved state, and selection.
  - **Parity with the player:** the sample scene run through the player's path (its file, straight into a session) and through the editor's Play button gives identical Worlds after 120 ticks.
  - **Repeated play:** 40 rounds of Play and Stop return device buffers, textures, pipelines, and pending retirements to where they were, and never reallocate the view.
  - **Input ownership:** keys before the click stay with the editor. After a click on the Game view, W flies the scene's camera at its speed and ⌘P goes to the game. Escape takes the input back and releases held keys. In the Scene view a click does not hand the input over, and the editor camera flies. Focus loss also takes it back.
  - **Controls:** pause holds the World, Step runs one tick, and resume has no burst.
  - **Scene changes:** opening or creating a scene ends play, and closing is not held up by it.
  - **No camera:** a scene without a camera plays through the editor camera and never hands the game the input.
  - The router's rules for the game owner, in [editor_tests.cpp](../tests/editor_tests.cpp).
- CTest runs the player with bad arguments, a missing project, a missing scene, and a scene path outside the project. Each must exit with its code and message ([expect_exit.cmake](../cmake/expect_exit.cmake)). [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) runs the player through window resizes and covers splitting launch arguments.
