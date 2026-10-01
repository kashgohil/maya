# Playing scenes

[Issue #1003](https://work.rezee.app/kash/issues/1003) runs saved scenes. The player opens a project's scene and plays it. The editor plays the open scene in a separate World and returns to the untouched scene on Stop. Both drive the same play session: a fixed-step clock, gameplay input assigned to ticks, and an explicit, ordered list of simulation systems. It follows the [scheduling contract](architecture/scheduling-contracts.md).

## Play sessions

`MayaSimulation` / `Maya::Simulation` ([play_session.hpp](../include/maya/simulation/play_session.hpp), [simulation.hpp](../include/maya/simulation/simulation.hpp)) is CPU-only and links MayaWorld, MayaScene, and [MayaPhysics](physics.md). MayaRuntime links it, so the player and the editor share one implementation.

```cpp
auto started = maya::PlaySession::start(document, maya::asset_property_context(registry), maya::play_systems(maya::registry_script_sources(registry)));
if (!started) { /* started.diagnostics (the scene) or started.error (a system) says why */ }
auto& session = *started.session;
session.input().feed(events_for_the_game);              // this host frame's gameplay events
const auto frame = session.update(wall_delta);
for (const auto& message : frame.messages) { /* script logs and errors: report, play goes on */ }
if (!frame.error.empty()) { /* stopped: report */ }
render(session.world(), session.camera());               // the first camera in document order
```

- **Start.** `PlaySession::start` validates the scene document, just as loading a scene file does, and builds a new World from it. It creates the session's [physics world](physics.md) (its settings are an optional last argument, and a scene's physics settings component replaces them). The scene's colliders and rigid bodies then become [bodies](physics.md#authored-bodies), all or none; a refusal names the entity and nothing starts. Then it starts each system in order. If the scene is invalid, the diagnostics say why. If a system throws from `start`, the error names it. The systems already entered are then stopped in reverse order, and nothing is left running.
- **Frames.** `update(wall_delta)` admits the frame's wall time to the clock and runs the ticks that are due. For each tick, the session latches that tick's input and runs every system in order. Physics then steps once ([the fixed tick](physics.md#the-fixed-tick)), and each system's `late_fixed_update` runs with the step's contact and trigger events. The session commits the systems' commands, with the moved body poses, as one atomic World batch, adds and removes bodies, and counts the tick. After the frame's ticks, each system's `frame` hook runs once with the admitted wall time, including while paused.
- **Messages.** Systems report through `TickContext::messages` and `FrameContext::messages`. `PlayFrame::messages` returns them with a level (info, warning, or error), a source, and the tick. They do not stop play: [scripts](scripting.md#errors) use them for logs and for errors that stop one instance.
- **Failures.** A system that throws or breaks the physics authority rules, or a batch the World rejects, stops the simulation. The World stays as the last completed tick left it (a body that cannot be created at the commit stops the session after that tick's batch). The error names the tick and the cause, for example `Tick 2: Spin failed: ...`, and later updates return the same error.
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

A `SimulationSystem` has a name, `start`, `fixed_update`, `late_fixed_update`, `frame`, and `stop`.

- **What an update sees.** `fixed_update` gets a `TickContext` with:
  - the World as the previous tick committed it;
  - the tick's command batch;
  - its `InputFrame`;
  - the tick index, simulation time, and fixed interval;
  - `bodies`, for physics requests, and `physics`, the body state after the previous step ([physics](physics.md#requests-during-a-tick)).
- **After the step.** `late_fixed_update` (phase 7, #1021) gets the same kind of `TickContext`, with `events` holding the step's sorted contact and trigger events and `physics` the completed step. Its commands join the tick's batch; its body requests apply before the next step ([physics](physics.md#the-fixed-tick)). It also runs once when the session stops, with `stopping` set and the events that end every contact in progress; nothing done then is kept.
- **Once per frame.** `frame` gets a `FrameContext`: the World and physics state as the last tick left them, the frame's admitted wall time, `alpha`, the tick and time, and the messages. It may not change anything. Its default does nothing.
- **When writes appear.** A system writes through the commands. Its writes are visible to the next tick, not to later systems in the same tick, so the result does not depend on how many systems read a value.
- **One writer.** Each transform should have one writer. Physics writes kinematic and dynamic bodies' transforms, and a system that writes one fails.
- **Determinism.** The same scene, input, and frame times give the same World.

Two built-in systems (`builtin_systems()`) run in this order, driven by two authored components. `play_systems(sources)` returns them followed by the [script system](scripting.md), which runs `maya.script` components; the player and the editor use it. The editor edits them like any other component, and scene files save them.

| Component | Properties | While playing |
| --- | --- | --- |
| `maya.fly_control` (ID 7) | `speed` (m/s, default 3), `look_sensitivity` (rad per point, default 0.0025) | Gameplay input flies the entity in its parent's space. WASD moves, Q and E go down and up, Shift is four times faster, and the mouse turns: yaw about +Y, then pitch, stopping just short of straight up or down. It moves only while there is input. The first turn replaces the authored rotation with yaw and pitch, dropping roll. |
| `maya.spin` (ID 6) | `axis` (local; its length is ignored), `speed` (rad/s, default π/4) | Turns the entity about the local axis at a constant rate. A zero axis or zero speed does nothing, and a negative speed turns the other way. |

The sample's spinning pyramid and flying camera are now these components in [basic.scene](../samples/basic_scene/assets/basic.scene). They are no longer sample code: `basic_scene.cpp` and `MayaBasicScene` are gone.

The play session implements the scheduling contract's clock and modes, its input assignment rules, and phases 1–6: committing, latching input, fixed-update hooks, and, since #1017, [body preparation, the physics step, and synchronization](physics.md#the-fixed-tick). This tick's commands commit at the end of the tick, which is the contract's phase 1 of the next tick with nothing queued in between. Since #1018, [scripts](scripting.md) run in phase 3 as one system. Since #1021, phase 7 delivers [contact and trigger events](physics.md#contact-and-trigger-events) and runs `late_fixed_update`. Animation arrives later. Since #1016, phase 2 keeps a pose history and views show [poses between ticks](#between-ticks). Since #1023, sessions can be [recorded and replayed](#recording-and-replay). Capture modes are still contracts.

## Between ticks

Ticks come at 60 Hz, and frames at whatever rate the display runs. Showing only the latest completed tick makes motion judder whenever the two differ. [Issue #1016](https://work.rezee.app/kash/issues/1016) shows each frame at its place between the last two ticks instead ([scheduling](architecture/scheduling-contracts.md#transform-authority)).

- **History (phase 2).** As each tick's batch commits, the session keeps the local transform each changed entity had before it. The World holds the current one.
- **`PlaySession::presentation()`** returns the poses to show now as `PresentationPoses`: world matrices for the entities that moved in the last tick, and their descendants. Everything else shows the World's own. It takes the clock's `alpha()`, the progress towards the next tick. At a completed tick (alpha 0) the previous pose shows, so views run one interval behind the simulation, as the contract intends.
- **Interpolation.** Translation and scale move linearly, and rotation turns along the shortest arc (`interpolate_transform`). World matrices are then composed down the hierarchy from the shown poses, so a child keeps its place on its parent; matrices are never blended.
- **Resets.** A teleport, a reparent, or a new entity has no pose to come from: that entity and everything below it show their current pose until the next tick. Body-mode changes will reset too, once motion types can change during play.
- **Pause and step** show the completed tick (alpha 1).
- **Read-only.** Presentation never writes the World, physics, or the authored scene.
- **One path.** The player and the editor's Play (Game and Scene views) pass the poses to extraction (`RenderExtractOptions::poses`) and to the camera's view (`extract_render_view`).

Cost, Release on the M4 Pro reference machine (thermal state nominal), with every entity moving every tick:

| Moving entities | History, per tick | `presentation()`, per frame |
| --- | --- | --- |
| 1,000 | < 0.05 ms | 0.04 ms |
| 10,000 | about 0.2 ms | 0.42 ms |
| 50,000 | about 1.4 ms | 2.15 ms |

History is the difference from the same ticks without it (about 20–28 ns per moving entity). `maya_simulation_tests "[cost]"` prints them. Entities that do not move cost nothing per frame. These are observations, not budgets.

## The player

```bash
maya_player [project [scene]] [--record file | --replay file] [--debug-physics] [--smoke N]
```

- **Project.** The project is a `project.maya` file or its folder, relative to where the player starts. Without one, the player uses the sample project.
- **Scene.** The scene is relative to the project's content root. Without one, the player uses the project's startup scene.
- **View.** The player shows the scene's first camera, in document order, at the window's size.
- **Input.** Every window event goes to the game, and the cursor is captured. Escape closes the window.
- **`--record file`** records the session and writes it to `file` when the window closes ([recording and replay](#recording-and-replay)).
- **`--debug-physics`** draws every [physics debug view](physics.md#debug-views) over the game, for debugging. Without it the player draws none.
- **`--replay file`** plays a recording's scene with its input instead of the window's. The scene comes from the recording, so no scene argument is needed. At the end the player reports `[Player] the replay matches the recording: 120 ticks, 2 checkpoints, and the final state` and stays on the last frame. A replay that differs fails with exit code 1, and so does a recording that cannot be replayed.

It reports what it runs, `[Player] My Game / basic.scene: 6 entities`, and exits with:

| Exit code | When | Example message |
| --- | --- | --- |
| 0 | The window closed normally, or a smoke run completed. | |
| 1 | The project, catalog, or scene cannot be run, the scene has no camera, the device cannot start, or something failed while playing. | `[Player] second.scene cannot be run:` followed by `line 10: unknown component 'maya.name_is_bad'; ...` |
| 2 | Unexpected arguments, `--record` or `--replay` without a file, or both together. | `[Player] unexpected argument: c` |

Render problems, such as a missing mesh file, are printed when they change, and play continues with that mesh skipped. Script logs and errors are printed as they happen, as `[Scripts] Beacon (scripts/spin.luau): ...`, and play continues. `maya_sample` is the same player on the sample project; it takes no project or scene.

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
- **Scripts.** Play uses each script's last version that compiled. A script file changed while playing is swapped in at the next tick, or a notice says why it cannot be ([reload](scripting.md#reload)).
- **Stop.** The play World and everything made for it are released. The authored scene, its history, and its unsaved state are exactly as they were. The selection returns to what it was at Play. The editor camera, the panels, and the view choice stay as they are.
- **Scene changes.** Opening or creating a scene ends play. Closing the editor is not held up by play, since play has nothing of its own to save. Cancelling the unsaved-changes prompt keeps playing.
- **Time.** Play advances with the editor's frames, so it does not advance while the window is minimized. The Diagnostics panel shows the tick, the wall time the clock refused, and the ticks it dropped, so slowdown is visible.
- **Failures.** A scene that cannot be built, such as one referencing an asset missing from the catalog, shows a notice and does not start. A system that fails while playing stops play, with the tick and the reason in a notice and in Diagnostics under "play". A script error does not: it goes to Diagnostics under "script" with the notice "A script stopped", and only that instance stops ([scripting](scripting.md#errors)). Script logs go to Diagnostics under "script" without a notice.

### Who gets the input

The [input router](editor.md#input-routing) now has three owners.

| State | Owner |
| --- | --- |
| Editing, or playing without the game having the input | The UI. The right mouse button over the viewport flies the editor camera, except over the Game view. |
| Flying | The editor camera, until the right button is released, Escape is pressed, or the window loses focus. |
| The game has the input | Starts with a left click on the Game view; that click only hands the input over. Every event goes to the play session, including ⌘ shortcuts and text, until Escape or a loss of focus takes it back. Held game keys are released then, and the cursor is captured meanwhile. |

A click on the Game/Scene toggle in the viewport's corner never hands the input to the game.

## Recording and replay

[Issue #1023](https://work.rezee.app/kash/issues/1023) records play sessions and replays them. It follows the [scheduling contract's](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick) definition of the same result: on the same machine and build, a replay repeats the recorded session tick for tick, with any physics worker count. Nothing is promised across machines.

**What a recording holds** (`PlayRecording`, [recording.hpp](../include/maya/simulation/recording.hpp)):

- **Input.** Each tick's `InputFrame`, stored only where it changes (`InputTrack`), so a held key costs one entry. Replay uses the tick each edge was assigned, not wall time.
- **The seed** the scripts ran with.
- **The scene.** The document that was played, as scene-file text, saved or not, and the scene's path for messages.
- **Asset versions.** A hash (FNV-1a 64) of each mesh and material file and each script's source that the scene names. For scripts, this is the version Play used, which in the editor is the last good one.
- **The build and physics.** `recording_build()` is the revision, build type, sanitizers, and compiler. A build from uncommitted changes carries a hash of those changes, so two different dirty builds do not share a name. `recording_physics()` is Jolt's version and configuration and the collision steps.
- **Reloads.** Each script reload applied while recording, with its tick.
- **Checkpoints.** A `state_hash` every 60 ticks, and `full_state_hash` after the last tick.

**The state hash** folds, in EntityId order:

- every entity's transform;
- each body's pose, linear and angular velocity, and sleep state;
- a running trace of every event delivered and every message the tick produced (script logs and errors).

What a script can observe shows up in what it does to the World and what it logs. `full_state_hash` adds the play World written as scene text, so every component value counts too. Values are hashed bit for bit.

**Replay.** `PlaySession::start_replay` takes the input track and the checkpoints. Each tick takes its input from the track instead of the window. The clock runs no further than the recorded ticks and pauses at the end. `replay()` reports whether it finished, how many checkpoints it checked, and the first checkpoint that differed.

**Refusals.** `replay_refusal` names the first reason a recording cannot be replayed here, and nothing plays:

| Mismatch | Reason given |
| --- | --- |
| Another build | `It was recorded by build 1a2b3c4d5e6f Release …; this is build …. A replay is promised to match only on the build that recorded it` |
| Other physics | `It was recorded with Jolt 5.5.0, …; this session uses …` |
| A reload while recording | `scripts/zone.luau was reloaded at tick 240 of the recorded session, so its replay is not promised to match` |
| A changed asset | `scripts/zone.luau has changed since the recording` |
| A removed asset | `scripts/zone.luau is no longer in the project` |

**Files.** Recordings are versioned text files that start with `maya-recording 1`. Header lines come first (build, physics, seed, ticks, scene name), then asset, reload, checkpoint, and final-state lines, and one `input` line per change. The scene comes last, as a counted block. Reading is strict: another format version, an unknown line, or a file cut short is refused with its line number.

**In the editor,** Play records nothing, since a recording has a cost (below). The scene menu offers:

- **Play and record** plays the scene as Play does and records it. The status bar shows Recording while it plays. The recording is kept until the next recorded Play.
- **Replay the last recording** plays it again with its own scene and input, while the status bar shows Replaying. Diagnostics then says `The replay matches the recording: 1800 ticks, 30 checkpoints, and the final state`. A difference raises the notice "The replay differs", naming the tick. A refusal raises "Couldn't replay" with its reason.
- **Save the last recording** writes it to `recordings/<scene>.recording` in the content folder.

The player records with `--record` and replays with `--replay` ([the player](#the-player)). A recording made in either one replays in the other.

**Reset.** Stop releases the play World, the physics world, and the script VM (`script_memory_in_use()` returns to 0). Nothing carries over into the next Play.

**Cost.** Recording stores a tick's input only when it differs from the last change. The rest is measured by `maya_simulation_tests "Recording cost*"`, in Release on the M4 Pro reference machine (thermal state nominal), with one entity in ten a body:

| Entities | Scene text, at start | State hash, every 60 ticks | Final state, at Stop |
| --- | --- | --- | --- |
| 1,000 | 1.8 ms | 0.18 ms | 2.4 ms |
| 10,000 | 11 ms | 0.94 ms | 13 ms |
| 50,000 | 30 ms | 3.1 ms | 51 ms |

Writing scene text is most of it: the final state writes the play World as text. A replay pays the same, except the scene text. These are observations, not budgets.

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
  - **Between ticks** (#1016): interpolation (linear, the shortest arc through 180°, and q against −q); poses between the last two ticks composed down a hierarchy, one interval behind; uneven frame times showing exactly the accumulated time; pause and step showing the completed tick; resets for teleports, reparenting, and new entities; the World untouched; and a hidden cost case.
  - **Play sessions:** system order, and commits visible to the next tick; the first camera; the authored document left untouched; stop order.
  - **Session failures:** start failures stop systems in reverse order; invalid scenes and empty systems are refused; a failing tick keeps the last completed World.
  - **The built-in systems:** spin about a local axis, zero and negative speed, and normalized rotations; fly movement, speed, Shift, look, and the pitch limit.
  - **Determinism:** identical results for the same scene, inputs, and uneven frame times.
- [replay_tests.cpp](../tests/replay_tests.cpp) (`maya_simulation_tests`, #1023) covers:
  - **Files:** a recording round-trips through its file; empty, foreign, other-version, cut-short, and malformed files are refused.
  - **Replay:** 1,800 ticks of stacked boxes, a trigger zone with a counting script, and a ball that a script kicks on Space, recorded at uneven frame times. It replays with 0 and 4 physics workers, matching all 30 checkpoints and the final state. Dropping one kick is found at the first checkpoint after it.
  - **Refusals:** another build, other physics (including collision steps), a changed or missing asset, and a reload.
  - **Cost:** a hidden case prints the cost of recording at 1,000, 10,000, and 50,000 entities.
- [editor_play_tests.cpp](../tests/editor_play_tests.cpp) covers:
  - **Separate state:** Play shows unsaved edits, and the play World spins while the authored World does not change. Edits, undo, Delete, and inline rename are refused during play, while selection works. Stop restores the scene text, history, unsaved state, and selection.
  - **Parity with the player:** the sample scene run through the player's path (its file, straight into a session) and through the editor's Play button gives identical Worlds after 120 ticks.
  - **Repeated play:** 40 rounds of Play and Stop return device buffers, textures, pipelines, and pending retirements to where they were, and never reallocate the view.
  - **Input ownership:** keys before the click stay with the editor. After a click on the Game view, W flies the scene's camera at its speed and ⌘P goes to the game. Escape takes the input back and releases held keys. In the Scene view a click does not hand the input over, and the editor camera flies. Focus loss also takes it back.
  - **Controls:** pause holds the World, Step runs one tick, and resume has no burst.
  - **Scene changes:** opening or creating a scene ends play, and closing is not held up by it.
  - **No camera:** a scene without a camera plays through the editor camera and never hands the game the input.
  - The router's rules for the game owner, in [editor_tests.cpp](../tests/editor_tests.cpp).
  - **120 Hz display** (#1016): the shown spin advances every frame, evenly, while the World changes every other frame.
- [editor_replay_tests.cpp](../tests/editor_replay_tests.cpp) covers (#1023):
  - **Parity:** 1,800 ticks of Play and record in the editor, with a trigger zone, a kicked ball, and Space pressed through the Game view at uneven frame times. They replay through the player's path (the project's files and the recorded scene) with every checkpoint and the final state matching. They also replay in the editor, which reports the match. The saved recording reads back the same.
  - **Reload:** a script reloaded during a recorded Play is recorded, and replaying it is refused with a notice.
  - **Reset:** 100 rounds of Play and Stop with physics and scripts, recording nothing. Script memory returns to 0 after each Stop. Device buffers, textures, and pending retirements, and physics memory, return to where they were. The scene text, history, unsaved state, and selection are unchanged.
- [renderer_gpu_tests.cpp](../tests/renderer_gpu_tests.cpp) renders a playing scene halfway between ticks on Metal and compares it byte for byte with the same cube authored at the pose between them (#1016).
- CTest runs the player with bad arguments, a missing project, a missing scene, a scene path outside the project, and a missing recording. Each must exit with its code and message ([expect_exit.cmake](../cmake/expect_exit.cmake)). It also records a smoke run of physics.scene and replays it, which must report a match. It runs physics.scene with `--debug-physics` (#1022). [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) runs the player through window resizes and covers splitting launch arguments.
