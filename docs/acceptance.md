# Milestone acceptance: author, save, and run a scene

[Issue #1005](https://work.rezee.app/kash/issues/1005) closes the first milestone ([#988](https://work.rezee.app/kash/issues/988)), and [#1024](https://work.rezee.app/kash/issues/1024) the second, physics and behavior ([#1014](https://work.rezee.app/kash/issues/1014); [below](#milestone-2-physics-and-behavior)). This page is the acceptance record:

- the automated checks, split by what they need;
- a short manual script for what only a person at the machine can check;
- the steady states that repeated work must return to;
- the scenes kept for regression;
- the benchmark baselines, the approved budgets, and the limits of what they show.

**Re-blessed for physically based shading (#1033).** The seven V1 references and four of the HDR references were re-blessed when surfaces began shading with glTF's metallic-roughness model and the sample lights were scaled by π. Large surfaces moved by at most 18 levels and new GGX highlights by more; the [table](renderer.md#version-1-content) records each image. The physics debug references and the luminance view still match. The project owner inspected the before-and-after images and the new material references and approved them on 3 October 2026.

**The material test scene** (#1033, [renderer](renderer.md#sample-content)) has six references in [tests/references/materials](../tests/references/materials), at 512 × 288 with the same tolerance. Under its sky environment (#1035): its camera's view through AgX and through PBR Neutral, the textured row from close by, and the row from behind, where the cutout's back faces and the glass show. Under the workshop environment: the camera's view and the textured row. The project owner inspected the four views re-rendered under the sky, against their #1033 versions, and the two workshop views, and approved them on 4 October 2026. The V1, HDR, and physics-debug references did not change: scenes without an environment differ only by the split-sum table replacing Karis's fit, within tolerance. Rendering requires no diagnostics, so a map of the wrong role fails the test.

**Re-blessed for the Sample Viewer's environment lighting (#1036).** When light from the surroundings began mixing dielectric and metal results and adding multiple scattering, as the Khronos glTF Sample Viewer does ([renderer](renderer.md#materials)), the six material-scene references and three V1 camera-path frames (`path-0000`, `path-0300`, `path-0600`, where the blue metal beam lightens by at most 21 levels) were re-blessed: rough and partly metallic spheres are brighter, and smooth metals and dielectrics unchanged. The other V1 images, the HDR and the physics-debug references stayed within tolerance and were not rewritten. **The [Sample Viewer references](import.md#the-sample-viewer-comparison)** in [tests/references/sample-viewer](../tests/references/sample-viewer) are the Khronos Sample Renderer's own renders, compared with their own declared tolerance. The project owner inspected the before-and-after images and the Sample Viewer captures and approved them on 6 October 2026.

**Re-blessed for shadows (#1034).** Lights cast [shadows](renderer.md#shadows) by default, so nine references were re-blessed: the six V1 views (`overlap` and the five camera-path frames, where the cubes, pyramids, and slab now shadow the floor and wall; 0.5–5.3% of pixels changed), the two textured material views (the cutout cube shadows itself through its leaves, and each tiled cube shades a strip of its neighbour's side), and the physics debug `triggers` view (the pillar and cube cast shadows). Elsewhere the lit floor moved by at most one level. Only those nine files were rewritten, from the renders the test wrote; the HDR, Sample Viewer, and other physics-debug references still match. The project owner inspected the before, after, and difference images and approved them on 6 October 2026.

**Debug views** (#1037, [renderer](renderer.md#debug-views)). [tests/references/debug-views](../tests/references/debug-views) holds the material test scene's overview in each of its 14 debug views (every one but `none`, which the materials overview is), at 512 × 288 with the same tolerance, named as the views are. The project owner inspected each beside the lit overview and approved them on 6 October 2026.

## Automated checks

```bash
cmake -S . -B build && cmake --build build -j 8
tools/check_milestone.sh build
```

[check_milestone.sh](../tools/check_milestone.sh) runs three CTest groups. It runs a group only when what it needs is there, and reports every group that could not run as **unavailable**, with the reason. It never reports a skipped group as passed.

| Group | CTest selection | Needs | What it covers |
| --- | --- | --- | --- |
| CPU | `-L cpu` | nothing else | World, properties, scenes, assets, projects, simulation, physics, scripting, metrics, the editor on the null device (including the authoring half of both acceptance workflows), the benchmark runner on the null device, argument handling, and UI isolation |
| GPU, headless | `-L gpu -LE smoke` | a Metal device | Metal rendering and retirement, GPU timing, the authored scene through the player's path, the V1 visual references, resize/reload/play-reset steadiness, and the editor on Metal |
| Windowed | `-L smoke` | Metal and a logged-in desktop session | The real `maya_player` on both authored projects from another directory, the player's failure exit codes, recording and replay, and debug views, the editor, player, and sample smoke runs, the desktop host, and benchmark CLI smoke runs, including `p1_small` |

The script ends by listing the manual steps and benchmark baselines as **manual**. Neither is run automatically: one needs a person, the other a quiet machine and a Release build.

### The acceptance workflow, automated

[acceptance_tests.cpp](../tests/acceptance_tests.cpp) and three CTest entries chained by a fixture:

1. **`maya_acceptance_author`** (CPU) drives the editor with synthetic input.
   - It opens a fresh copy of the sample project in `build/acceptance/Authored Game` and creates a scene with ⌘N.
   - It places two cubes and a pyramid by dragging them from the Assets panel onto the ground. It assigns red by dropping it on a cube in the viewport, blue by dropping it on a Hierarchy row, and amber through the Inspector's path.
   - It edits values (a rotated, nonuniformly scaled pyramid, and a spinning cube), duplicates the pyramid with ⌘D, parents a cube under the copy, and saves with ⌘S as `levels/authored`.
   - It checks that every object has a distinct transform, then reopens the scene in a fresh editor. IDs, hierarchy, values, and root order round-trip exactly.
2. **`maya_acceptance_render`** (GPU) runs the saved scene through the player's path: the project file, its catalog, the scene file, and a play session for one second.
   - It renders through the scene's camera and checks that each object appears where it was placed, in its material's color.
   - Four instances share two mesh allocations (four buffers) and make four draws.
3. **`maya_acceptance_player`** (windowed) runs the real `maya_player "../Authored Game" levels/authored.scene` from `build/acceptance/elsewhere`, a different working directory. It must report `Authored Game / levels/authored.scene: 6 entities` and complete 120 frames.

Authoring/play isolation is covered by [editor_play_tests.cpp](../tests/editor_play_tests.cpp) (#1003).

## Steady states

Repeated work must return to these states, with no stale handles and no unbounded growth.

| Repeated work | Returns to | Checked by |
| --- | --- | --- |
| Loading, playing, and unloading a 10,000-entity scene 300 times | No buffers or resident meshes beyond the empty session. Two textures (the view target), no pending retirements, and no outside leases. | `l1_load` benchmark; `benchmark_tests.cpp` |
| Starting and stopping 300 play sessions from one authored scene | The same, and the authored scene is unchanged | `l1_play` benchmark; `benchmark_tests.cpp`; 40 editor Play/Stop rounds in `editor_play_tests.cpp` |
| Resizing a view 200 times across five sizes | Two textures sized for the current size, no pending retirements. The first size's texture no longer resolves, and Metal's reported memory grows by at most 32 MiB. | `acceptance_tests.cpp` |
| Evicting and reloading an asset | Its old handle resolves as stale; the reload is a new generation | `acceptance_tests.cpp` |
| 25 play resets | No handle from one session's World is alive in the next; no outside leases once the sessions end | `acceptance_tests.cpp` |

**Process footprint.** In the load and play cycles, the footprint rises by about 200 MiB during the first cycle, then levels off. Since #1024 the cycles run 300 times and the slope is fitted over cycles 101–300: over cycles 11–100 it had failed intermittently near its limit (116 and 108 KB/cycle in #1016) while the footprint was still settling, though it levels off by cycle 200 with no later growth. All tracked counts are back at the empty session after every cycle. The retained memory is not attributed to engine allocations; it is consistent with allocator and driver retention. It is recorded as a plateau, not called a leak.

## Regression scenes

| Scene | Where | Purpose |
| --- | --- | --- |
| V1 visual reference | [v1_reference.scene](../samples/basic_scene/assets/v1_reference.scene) in the sample project | See below. |
| I1 repeated instances | [benchmarks/i1_*.benchmark](../benchmarks) (generated) | 1,000, 10,000, and 100,000 instances of one cube and one material; a subset view |
| L1 load/unload and play reset | [benchmarks/l1_load.benchmark](../benchmarks/l1_load.benchmark), [l1_play.benchmark](../benchmarks/l1_play.benchmark) | 300 cycles of a 10,000-entity scene (100 before #1024); malformed and missing-asset scenes |
| The sample scene | [basic.scene](../samples/basic_scene/assets/basic.scene) | The fast regression case; not evidence of scale |
| Material test scene | [materials.scene](../samples/basic_scene/assets/materials.scene), [benchmarks/materials.benchmark](../benchmarks/materials.benchmark) | Spheres across metallic and roughness, and surfaces with each kind of map, a cutout, and glass (#1033), lit by an environment and drawn against its sky (#1035); its reference images below |
| Lights test scene | [lights.scene](../samples/basic_scene/assets/lights.scene), [benchmarks/lights.benchmark](../benchmarks/lights.benchmark) | A shadowed sun, 16 point lights, and 6 shadowed spot lights over 35 props: the light limits and the shadow passes' cost (#1034) |

**The V1 visual reference scene** contains:

- a 20 m floor and a back wall;
- shared cubes and pyramids, with a near cube in front of a far one;
- a rotated, nonuniformly scaled slab and a parent/child stack;
- four materials, a directional light, and a camera on a spinning rig (a fixed 600-tick camera path).

**Named views.** `maya_visual_reference` renders the scene at 256×144 at ticks 0, 150, 300, 450, and 600, plus an overview and an overlap view. It compares each against the PNGs in [tests/references/v1](../tests/references/v1).

- The views after 600 ticks and at tick 0 must match, since one turn returns the camera to its start.
- The authored World and the play World render identical images at tick 0.

**Tolerance.** Each channel must be within 6 on at least 99.5% of pixels. This tolerance is declared for Apple silicon Metal. On a mismatch, the test writes the actual image and a difference image to `build/visual-diffs`. Moving the pyramid by 0.5 m fails four of the seven views.

**Re-blessing.** Render the references with `MAYA_BLESS_REFERENCES=1 build/maya_editor_tests "[visual]"`, then inspect them before committing. The current references were inspected: the path stays inside the room, and each view shows the objects it names.

**Re-blessed once for HDR (#1032).** The V1 and [physics debug](../tests/references/physics-debug) references were re-blessed when views began rendering through the [HDR pipeline](renderer.md#exposure-and-tone-mapping): scene light exposed at EV100 0, tone-mapped by AgX, and sRGB-encoded. The old images showed linear values unencoded, so the new ones are lighter in the midtones, flatter, and less saturated (AgX turns the pure red cubes pinkish red), and the background clear color shows as mid grey. Debug lines keep their colors, drawn after tone mapping. The project owner inspected the before-and-after images and approved them on 3 October 2026. Five [HDR references](../tests/references/hdr) were added from the V1 overview: EV100 −2 and +2, PBR Neutral, and the luminance and false-color exposure views.

## Manual acceptance script

Run on the reference machine with a Release build. Each step lists its expected result.

1. **Copy the project somewhere else.** `cp -R samples/basic_scene ~/Desktop/Trial`, then `cd /tmp` and run `maya_editor ~/Desktop/Trial`. The editor opens *Trial / basic.scene*.
2. **Create.** Press ⌘N. You get an untitled scene with a camera and a sun.
3. **Place.** Drag `cube` from Assets into the viewport three times, and `pyramid` once. Each lands on the ground where it was dropped.
4. **Assign.** Drop `red` on one cube in the viewport, and `blue_metal` on another cube's Hierarchy row. The colors change, and ⌘Z undoes each assignment separately.
5. **Edit.** Rotate and scale the pyramid with the gizmo (E, R), and type a scale into the Inspector. Add Spin to a cube. Drag one cube onto the pyramid's row to parent it.
6. **Save and reopen.** Press ⌘S and save as `levels/trial`. Quit with ⌘Q: no prompt appears. Reopen the editor and open `levels/trial` from the scene menu. Everything is as it was left.
7. **Play.** Press ⌘P: the cube spins. Click the view and fly with WASD, then press Esc and ⌘P. The scene is back to its authored state, and the selection is restored.
8. **Run standalone.** `cd /tmp && maya_player ~/Desktop/Trial levels/trial`. The same scene runs, and the camera flies with the mouse and WASD.
9. **Unsaved changes.** Rename an entity, then press ⌘Q. You are asked Save / Don't save / Cancel, and Cancel keeps the editor open.

## Baselines

Measured for #1005:

- **Machine:** a Mac16,7 with an Apple M4 Pro (10 performance and 4 efficiency cores, a unified-memory GPU) and 24 GiB of memory, on macOS 26.6.2 (25G83).
- **Build:** Release. The per-instance workloads ran at revision `7329fda` plus the #1005 runner changes; the cycle workloads were re-measured at `a0f84ca`. The engine code is the same in both.
- **Rendering:** 1920×1080 offscreen, one tick per frame.
- **Conditions:** the measuring thread ran at user-interactive quality of service. Each manifest started after the machine reached the `nominal` thermal state or after a two-minute pause, as noted below. The JSON results record everything else.

The per-instance workloads used 300 warmup and 3,000 sampled frames per run and three runs, plus a matched uninstrumented run.

| Manifest | Thermal (start → end) | CPU frame mean / P95 / P99 (ms) | GPU mean / P95 (ms) | Draws, triangles | Unique meshes, materials |
| --- | --- | --- | --- | --- | --- |
| `sample` | nominal → nominal | 0.039–0.043 / 0.08–0.11 / 0.12–0.13 | 0.044–0.046 / 0.06–0.08 | 4, 42 | 2, 4 |
| `i1_1k` | nominal → nominal | 0.216–0.221 / 0.23 / 0.24 | 0.16–0.17 / 0.17 | 1,000, 12,000 | 1, 1 |
| `i1_10k` | nominal → nominal | 1.98–1.99 / 2.01–2.03 / 2.04–2.06 | 1.17 / 1.20 | 10,000, 120,000 | 1, 1 |
| `i1_10k_subset` | nominal → nominal | 1.98–1.99 / 2.01–2.03 / 2.05–2.07 | 1.12 / 1.14 | 10,000, 120,000 (3,310 in view) | 1, 1 |
| `i1_100k` | nominal → **fair** | runs 1–2: 20.5–20.8 / 20.8–21.0 / 21.3–21.7 | 7.1–7.3 / 9.1–9.3 | 100,000, 1,200,000 | 1, 1 |

- **`i1_100k` throttled.** The machine reached `fair` near the end: run 3 was steady at 20.8–21.0 ms for 2,700 frames, then rose to a 58 ms average over its last 300 (P99 110 ms). The uninstrumented run that followed was throttled throughout. This manifest's run 3 and overhead figure are therefore recorded as disturbed, not as a baseline. In two earlier attempts on the same commit, the same step appeared during run 3. Sustaining this workload for the full protocol exceeds what this machine holds at nominal temperature.
- **No missing GPU samples.** No sampled frame lost its GPU time, in any run, instrumented or not.
- **Instrumentation overhead.** On the cool manifests it was −1.4% to −0.7% of the frame: not measurable.

| Cycles (100 each) | Thermal | Load or start (ms, mean / P95) | First frame (ms) | Stop (ms) | Tracked counts after every cycle | Footprint |
| --- | --- | --- | --- | --- | --- | --- |
| `l1_load`: a 10,000-entity scene from its file (two runs) | nominal → nominal | 34.2–34.4 / 35.1 | 2.26–2.29 | 0.12–0.13 | back at the empty session | 218 MiB empty; 422–433 MiB; slope +16 and +59 KB/cycle |
| `l1_play`: play sessions from the authored scene | nominal → nominal | 6.5 / 6.8 | 2.28 | 0.13 | back at the empty session | 218 MiB empty; 416–422 MiB; slope +46 KB/cycle |

- **Refusals.** The malformed and missing-asset scenes were refused, with nothing left behind.
- **Authored scene.** It was unchanged after the play cycles.
- **Throttled first measurement.** The first measurement of both cycle workloads followed `i1_100k` and started in the `fair` state. It loaded in 39.7 ms (P95 46.4 ms) and started play in 6.9 ms (P95 7.2 ms). The cool re-measurement above replaces it; the two cool load runs agreed to within 0.2 ms.
- **Footprint slopes.** All runs of the same workloads measured slopes from −25 to +59 KB/cycle, within about ±10 MiB of cycle-to-cycle variation. The data shows no unbounded growth.

### Budgets

Approved on 29 September 2026 by the project owner, after the cycle workloads were re-measured on a cool machine. No budget existed before this milestone.

These are **regression budgets for the M4 Pro reference runs**. They are not the production budgets of the [performance baseline](architecture/performance-baseline.md): the 60 fps target, the M1 Baseline and M2 Pro Headroom profiles, and subsystem allocations stay unallocated.

Every budget shares these conditions:

- **Workloads:** the `maya-benchmark 1` manifests in [benchmarks/](../benchmarks) at `a0f84ca`. A change to a manifest is a new workload and needs a new baseline.
- **Hardware:** the Mac16,7 above (Apple M4 Pro, 24 GiB, macOS 26.6.2).
- **Quality:** a Release build, 1920×1080 offscreen, RGBA8 color and 32-bit depth, no antialiasing, one tick per frame.
- **Method:** `maya_benchmark` at user-interactive QoS. Percentiles are nearest-rank over the sampled frames or the cycles after warmup.
- **Valid runs:** the machine must be `nominal` at the start and at the end. A run that ends in any other state does not count, pass or fail, and is repeated after the machine cools.

| Budget | Limit | Checked by | Observed | Rationale |
| --- | --- | --- | --- | --- |
| Tracked counts after load, play, and resize cycles | Exactly the empty session | Tests, on every CTest run; `l1_load` and `l1_play` | Exact in every run | Any drift is a defect |
| Process footprint slope over cycles 101–300 (11–100 before #1024) | At most 100 KB/cycle | `l1_load`, `l1_play` | See [#1024's baselines](#physics-baselines) | About twice the observed spread; flags growth of 10 MiB per 100 cycles. The window moved on 1 October 2026, approved by the project owner; the limit stayed. |
| I1 10k frame | CPU P99 at most 1.0 ms; GPU P95 at most 1.3 ms (2.5 ms and 1.5 ms before #1025) | `i1_10k`, `i1_10k_subset` | 0.80–0.86 ms; 0.67–1.06 ms (2.04–2.13 ms and 1.14–1.22 ms before instancing) | About 20% above the observations. Tightened on 6 October 2026 with instanced, culled draws (#1025), approved by the project owner. |
| Loading a 10,000-entity scene | P95 at most 50 ms | `l1_load` | 35.1 ms cool (46.4 ms throttled) | About 40% above the cool observations |
| Starting play | P95 at most 10 ms | `l1_play` | 6.8 ms cool (7.2 ms throttled) | About 45% above the cool observations |
| I1 100k frame | CPU P99 at most 10.5 ms; GPU P95 at most 5.5 ms | `i1_100k` | 8.6–9.0 ms; 4.4 ms (20.5–22.4 ms CPU before #1025) | About 20% above the observations, and under the proposed 16.67 ms target. Added on 6 October 2026 with #1025's instancing and culling, approved by the project owner. Before, it had no budget: 21 ms of CPU, over the target. |

The benchmarks are not part of CTest, so the timing and slope budgets are checked by running the manifests on the reference machine, not on every build.

## Limits against the workload contract

- **Reference hardware.**
  - The contract's Baseline profile (Apple M1, 8-core GPU, 16 GiB, 1920×1080) and Headroom profile (M2 Pro) were not available. Both are **unmeasured**.
  - The measurements above come from an M4 Pro. They are that machine's observations and are not interchangeable with either profile.
- **Scale.** The sample scene is not evidence of large-game readiness, and neither is I1: its 100,000 instances are a stress input, not a promised capacity.
  - At 100,000 instances a frame took about 21 ms of CPU time at #1005, over the proposed 16.67 ms interactive target: the renderer encoded one draw with its own constants per instance and did not cull. Since #1025 it batches instances into instanced draws and culls each pass, and a frame takes 8.2–8.4 ms (P99 8.6–9.0 ms); what remains is mostly extraction ([renderer](renderer.md#culling-and-batching)).
  - On this machine, the full 100k protocol can run into thermal throttling, and an idle machine's sleep spoils long unattended runs: keep it awake (`caffeinate -dimsu`).
- **Not implemented at #1005, so not measured:** per-pass GPU timing, streaming (S1), physics, scripting, texture and PBR/shadow quality, import and cook times, and edit-to-preview latency. Physics and scripting are measured for milestone 2 below.
- **Present pacing** is not measured: benchmarks render offscreen. The editor's live display shows the frame interval, but it is not a controlled measurement.
- **Retained footprint.** The ~200 MiB footprint plateau after the first load cycle is unattributed. Attributing it needs Instruments on the reference hardware.
- **Cold-cache loads.** The OS file cache is not controlled, so every load time is a warm-cache time.

## Milestone 2: physics and behavior

[Issue #1024](https://work.rezee.app/kash/issues/1024) closes the physics and behavior milestone ([#1014](https://work.rezee.app/kash/issues/1014)): author a physics interaction, reset it safely, edit behavior, and run it in both the editor and the player. The checks run in the same three groups as above.

### The reference interaction, automated

A ball rolls down a ramp into a stack of crates; a trigger zone's script opens a kinematic door when something enters. [physics_acceptance_tests.cpp](../tests/physics_acceptance_tests.cpp) and two CTest entries chained by a fixture:

1. **`maya_physics_acceptance_author`** (CPU) drives the editor on a fresh copy of the sample project in `build/acceptance/Physics Game`.
   - **Author.** The script file is written as a person would in a text editor and listed in the catalog. In a new scene, the editor makes a floor, a ramp, a heavy ball, three light crates, a sensor zone, and a kinematic door, with a child for its mesh (moving bodies have unit scale). The script is dragged from the Assets panel onto the zone's Hierarchy row, given the door, and its `lift` is edited by dragging its Inspector field. The scene is saved as `levels/door`.
   - **Reopen.** A fresh editor opens it; every entity has its ID.
   - **Play.** A recorded Play: the ball knocks the bottom crate into the zone, the script logs "the door rises for Crate 1", and the door rises by the edited lift. Stop returns the scene text to the authored one.
   - **Edit behavior.** The script file changes while the editor is open: the door now slides aside. Replaying the first Play is refused ("scripts/door.luau has changed since the recording"). A new recorded Play slides the door and no longer raises it, and its replay matches the recording.
   - **Reset.** 25 Play/Stop rounds return device buffers, textures, and pending retirements, physics memory, and script memory (0) to the empty session, and leave the scene text and its saved state unchanged.
   - **Errors.** The script is broken to fail when something enters. The error is reported once with the notice "A script stopped", and the session plays on for two more seconds, with the door shut.
2. **`maya_physics_acceptance_player`** (windowed) runs the real `maya_player "../Physics Game" levels/door.scene --smoke 900` from `build/acceptance/elsewhere`. It must log that the door slides open.

**Elsewhere in the milestone.**
- Bodies, colliders, queries, and events: [physics_tests.cpp](../tests/physics_tests.cpp) and [editor_physics_tests.cpp](../tests/editor_physics_tests.cpp).
- Scripts and reload: [scripting_tests.cpp](../tests/scripting_tests.cpp) and [editor_scripting_tests.cpp](../tests/editor_scripting_tests.cpp).
- Recording, replay, and editor/player parity: [replay_tests.cpp](../tests/replay_tests.cpp) and [editor_replay_tests.cpp](../tests/editor_replay_tests.cpp).
- Debug views, with their reference images: [physics_debug_gpu_tests.cpp](../tests/physics_debug_gpu_tests.cpp) and [tests/references/physics-debug](../tests/references/physics-debug).
- CTest runs the player with `--record`, `--replay`, and `--debug-physics` (and, since #1037, `--debug-view`), and `p1_small`.

### Steady states, physics and behavior

| Repeated work | Returns to | Checked by |
| --- | --- | --- |
| 100 Play/Stop rounds with physics and scripts | Device buffers, textures, and pending retirements, physics memory, and script memory at the empty session; the scene text, history, unsaved state, and selection unchanged | `editor_replay_tests.cpp`; 25 rounds in the reference interaction |
| Repeated play sessions in the player's path | Jolt's memory back to the empty-session baseline | `physics_tests.cpp` |
| 200 edits to a script while playing | Script memory flat; the session keeps running | `editor_scripting_tests.cpp` |
| Replaying a recording | The same checkpoints and final state, with any worker count | `replay_tests.cpp`, `editor_replay_tests.cpp` |
| Turning debug capture on | Nothing changes: identical state hashes over 300 ticks | `physics_debug_tests.cpp` |

### Regression scenes, physics and behavior

| Scene | Where | Purpose |
| --- | --- | --- |
| The physics sample | [physics.scene](../samples/basic_scene/assets/physics.scene) | A floor, a stack of crates, a falling crate, and a scripted beacon |
| The reference interaction | `levels/door.scene`, written by the acceptance test | The milestone's workflow end to end |
| The debug-view scene | [physics_debug_gpu_tests.cpp](../tests/physics_debug_gpu_tests.cpp) (in code) | One reference image per debug category |
| P1 physics stress | [p1_small](../benchmarks/p1_small.benchmark), [p1_physics](../benchmarks/p1_physics.benchmark), [p1_20k](../benchmarks/p1_20k.benchmark) | The [physics stress workload](architecture/performance-baseline.md#p1-physics-stress), version 1 |

### Manual script, physics and behavior

Run on the reference machine with a Release build, after the steps above.

1. **Play physics.** Open `physics.scene` and press ⌘P. The crates settle, the falling crate lands on them, and the beacon spins. Stop: everything is back where it was authored.
2. **Debug views.** Open the eye menu in the viewport and check Colliders, Body state, and Contacts. While editing, outlines sit on each collider. In Play, active bodies are green and turn slate as they fall asleep, and contacts show while the stack settles. Uncheck group 0: everything in it disappears. Quit and reopen the editor: the checks are as you left them.
3. **Edit a collider.** Select Crate 1 and press C. Drag the +X face's dot: the box grows to the right while its left face stays. ⌘Z undoes the whole drag.
4. **Pause and step.** In Play, pause (⇧⌘P) and step (⌥⌘P): each step advances one tick, and Diagnostics' Physics section counts bodies, contacts, and steps.
5. **Edit a script while playing.** Change the beacon's speed in `scripts/spin.luau` and save. The beacon changes at the next tick, and Diagnostics logs the reload.
6. **Record and replay.** Choose Play and record from the scene menu, click the Game view and fly the camera for a few seconds, then stop. Replay the last recording: the camera flies the same path, and Diagnostics says the replay matches.
7. **Run standalone.** `maya_player <project> physics.scene --debug-physics`: the same scene with outlines over it. Without the flag, none.
8. **Break a script.** Add `error("boom")` to `spin.luau`'s `fixed_update` while playing. Diagnostics reports the error with the notice "A script stopped", and the rest of the scene keeps playing.

### Physics baselines

Measured for #1024 on 1 October 2026, on the Mac16,7 above (Apple M4 Pro, 24 GiB, macOS 26.6.2), in a Release build of revision `81b1d88` plus #1024's changes, at user-interactive quality of service. Each manifest started after the machine reported a nominal thermal state and a further two-minute pause, and every one ended nominal too.

**L1 at 300 cycles,** fitted over cycles 101–300:

| Manifest | Load or start (ms, mean / P95) | First frame (ms) | Stop (ms) | Tracked counts after every cycle | Footprint (MiB) | Slope, cycles 101–300 | For comparison, cycles 11–100 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `l1_load` | 35.9 / 37.6 | 2.42 | 0.13 | back at the empty session | 220 empty; 426 after cycle 1; 435 from cycle 100 on | +35 KB/cycle | −9 KB/cycle |
| `l1_play` | 6.6 / 7.1 | 2.40 | 0.13 | back at the empty session | 220 empty; 411 after cycle 1; 425–426 from cycle 100 on | +2.3 KB/cycle | **+105 KB/cycle** |

The `l1_play` run shows why the window moved: over the old window its slope was over the limit, while the footprint stopped growing by cycle 100 and stayed within 1 MiB for the other 200. Both malformed and missing-asset scenes were refused, and the authored scene was unchanged.

**P1** (`p1_physics`, version 1: 5,556 bodies, 5,000 of them dynamic; three runs per configuration, 300 warmup and 3,000 sampled ticks each):

| Per tick (ms, means of the three runs) | 7 workers (the default) | No workers |
| --- | --- | --- |
| Whole tick: mean / P50 / P95 / P99 / max | 5.85–5.98 / 5.83–5.97 / 6.24–6.51 / 6.40–7.08 / 9.3–14.4 | 11.40–11.56 / 11.03–11.41 / 12.82–12.94 / 13.22–13.68 / 16.6–26.9 |
| Physics step (phase 5) | 2.53–2.54 | 8.04–8.14 |
| Queries (1,000 closest-hit rays, 100 overlaps, 20 casts) | 2.49–2.60 | 2.52–2.54 |
| Scripts (500 instances, phase 3) | 0.25–0.27 | 0.26 |
| Events, then post-physics hooks (phase 7) | 0.17, then 0.02 | 0.16–0.18, then 0.02 |
| Synchronize (6), body preparation (4), body commit | 0.08, 0.03, 0.01 | 0.07–0.08, 0.03, 0.01 |
| Starting the session | 9–12 ms | 8–9 ms |

- **Counts per tick (means, the same in every run):** 2,046 active and 2,955 sleeping bodies; 9,906 touching pairs, 9,090 of them solid contacts; 161 events; 5,373 query hits.
- **Memory:**
  - Jolt's heap peaks at 39.6 MiB, and is back to 0.3 MiB (the job pool and type registry) after every run.
  - The per-step scratch allocator reaches 15.5 MiB against its 4 MiB default, so Jolt falls back to malloc every step. A 64 MiB allocator changed nothing measurable (6.18–6.25 ms against 6.22–6.27 ms over 1,000 ticks), so the default stays.
  - The script VM holds 0.6 MiB.
  - The process footprint is 235–247 MiB, with slopes from −876 to +366 bytes per tick over the sampled ticks.
- **Determinism.** All six runs ended in the same state (`77030e512a75db62`), and no step hit a physics limit.
- **Queries.** Before `raycast_nearest`, the rays alone took 5.0 ms per tick; they take 0.95 ms as closest-hit queries ([physics](physics.md#queries)).

**P1 at 20,000 dynamic bodies** (`p1_20k`, 20,556 bodies; a stress input with no budget). All six runs completed with no physics limit hit and ended in the same state (`44972d3041dd06d8`).
- **7 workers:** 17.6–17.8 ms per tick (P99 19.4–19.9), with the step at 10.6–10.7 ms. That is over a 16.67 ms frame.
- **No workers:** 42.9–43.3 ms (P99 52–56).
- **Per tick:** 8,935 active bodies and 39,695 solid contacts; Jolt's heap peaks at 95–96 MiB and the scratch allocator at 38.9 MiB.

### Physics budgets

Approved on 1 October 2026 by the project owner, from the baselines above. They share the conditions of the [milestone 1 budgets](#budgets) (the reference machine, a Release build, user-interactive quality of service, nominal at the start and the end), on the `maya-benchmark 1` P1 manifests of #1024. A change to the recipe is a new version and needs a new baseline.

| Budget | Limit | Checked by | Observed | Rationale |
| --- | --- | --- | --- | --- |
| P1 whole tick, default workers | P99 at most 8.5 ms | `p1_physics` | 6.40–7.08 ms | About 20% above the worst run |
| P1 Jolt heap | Peak at most 48 MiB | `p1_physics` | 39.6 MiB | About 20% above the observation |
| P1 determinism | Every run and worker configuration ends in the same state, and no step hits a physics limit | `p1_physics`, `p1_small` (on every CTest run) | Exact in every run | Any difference is a defect |
| P1 with no workers, and `p1_20k` | **No budget** | — | 11.4–11.6 ms; 17.6–17.8 ms with workers | A diagnostic configuration, and a stress input, not a capacity |

The footprint-slope budget above now uses cycles 101–300: `l1_load` +35 and `l1_play` +2.3 KB/cycle, within its 100 KB/cycle. Loading (P95 37.6 ms against 50) and starting play (P95 7.1 ms against 10) stay within their budgets.

### Limits, physics and behavior

- **One machine.** The same limits as above apply: the M1 Baseline and M2 Pro Headroom profiles are unmeasured, and these are M4 Pro observations.
- **Determinism is per build and machine.** Runs repeat exactly across worker counts on this machine and build; nothing is promised across machines, OS or compiler versions, or Jolt and Luau versions ([scheduling](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick)).
- **P1 is headless.** It measures simulation, not rendering; debug views have their own measurement ([renderer](renderer.md#debug-lines)).
- **Contact constraints** are not counted: Jolt does not report them. Touching pairs and solid contacts are counted instead.
- **20,000 bodies** do not fit a 60 Hz frame on this machine with the default workers. It is a stress input, not a capacity.
- **What a person checks.** The editor's interaction (debug views, handles, pause and step, a script reloading while playing) is covered by tests on the null device, and by the manual script on the real editor.

