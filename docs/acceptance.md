# Milestone acceptance: author, save, and run a scene

[Issue #1005](https://work.rezee.app/kash/issues/1005) closes the first milestone ([#988](https://work.rezee.app/kash/issues/988)), [#1024](https://work.rezee.app/kash/issues/1024) the second, physics and behavior ([#1014](https://work.rezee.app/kash/issues/1014); [below](#milestone-2-physics-and-behavior)), [#1040](https://work.rezee.app/kash/issues/1040) the third, rendering and content ([#1029](https://work.rezee.app/kash/issues/1029); [below](#milestone-3-rendering-and-content)), and [#1069](https://work.rezee.app/kash/issues/1069) the fourth, world scale ([#1059](https://work.rezee.app/kash/issues/1059); [below](#milestone-4-world-scale)). This page is the acceptance record:

- the automated checks, split by what they need;
- a short manual script for what only a person at the machine can check;
- the steady states that repeated work must return to;
- the scenes kept for regression;
- the benchmark baselines, the approved budgets, and the limits of what they show.

**Re-blessed for physically based shading (#1033).** The seven V1 references and four of the HDR references were re-blessed when surfaces began shading with glTF's metallic-roughness model and the sample lights were scaled by π. Large surfaces moved by at most 18 levels and new GGX highlights by more; the [table](renderer.md#version-1-content) records each image. The physics debug references and the luminance view still match. The project owner inspected the before-and-after images and the new material references and approved them on 3 October 2026.

**The material test scene** (#1033, [renderer](renderer.md#sample-content)) has six references in [tests/references/materials](../tests/references/materials), at 512 × 288 with the same tolerance. Under its sky environment (#1035): its camera's view through AgX and through PBR Neutral, the textured row from close by, and the row from behind, where the cutout's back faces and the glass show. Under the workshop environment: the camera's view and the textured row. The project owner inspected the four views re-rendered under the sky, against their #1033 versions, and the two workshop views, and approved them on 4 October 2026. The V1, HDR, and physics-debug references did not change: scenes without an environment differ only by the split-sum table replacing Karis's fit, within tolerance. Rendering requires no diagnostics, so a map of the wrong role fails the test.

**Re-blessed for the Sample Viewer's environment lighting (#1036).** When light from the surroundings began mixing dielectric and metal results and adding multiple scattering, as the Khronos glTF Sample Viewer does ([renderer](renderer.md#materials)), the six material-scene references and three V1 camera-path frames (`path-0000`, `path-0300`, `path-0600`, where the blue metal beam lightens by at most 21 levels) were re-blessed: rough and partly metallic spheres are brighter, and smooth metals and dielectrics unchanged. The other V1 images, the HDR and the physics-debug references stayed within tolerance and were not rewritten. **The [Sample Viewer references](import.md#the-sample-viewer-comparison)** in [tests/references/sample-viewer](../tests/references/sample-viewer) are the Khronos Sample Renderer's own renders, compared with their own declared tolerance. The project owner inspected the before-and-after images and the Sample Viewer captures and approved them on 6 October 2026.

**Re-blessed for shadows (#1034).** Lights cast [shadows](renderer.md#shadows) by default, so nine references were re-blessed: the six V1 views (`overlap` and the five camera-path frames, where the cubes, pyramids, and slab now shadow the floor and wall; 0.5–5.3% of pixels changed), the two textured material views (the cutout cube shadows itself through its leaves, and each tiled cube shades a strip of its neighbour's side), and the physics debug `triggers` view (the pillar and cube cast shadows). Elsewhere the lit floor moved by at most one level. Only those nine files were rewritten, from the renders the test wrote; the HDR, Sample Viewer, and other physics-debug references still match. The project owner inspected the before, after, and difference images and approved them on 6 October 2026.

**Debug views** (#1037, [renderer](renderer.md#debug-views)). [tests/references/debug-views](../tests/references/debug-views) holds the material test scene's overview in each of its 14 debug views (every one but `none`, which the materials overview is), at 512 × 288 with the same tolerance, named as the views are. The project owner inspected each beside the lit overview and approved them on 6 October 2026. Since #1066 it also holds `lod`, at 512 × 288: a field of the sample sphere to 240 m in bands of the [level of detail](renderer.md#levels-of-detail) each draws, green nearest to blue at the horizon, with a quarter-pixel threshold so every level is in view. The project owner approved it on 10 October 2026.

## Automated checks

```bash
cmake -S . -B build && cmake --build build -j 8
tools/check_milestone.sh build
```

[check_milestone.sh](../tools/check_milestone.sh) runs five groups. It runs a group only when what it needs is there, and reports every group that could not run as **unavailable**, with the reason. It never reports a skipped group as passed.

| Group | Selection | Needs | What it covers |
| --- | --- | --- | --- |
| CPU | `-L cpu` | nothing else | World, properties, scenes, assets, projects, simulation, physics, scripting, metrics, the editor on the null device (including the authoring half of every acceptance workflow), the benchmark runner on the null device, argument handling, and UI isolation |
| Static analysis | [static_analysis.py](../tools/static_analysis.py) | `python3` and the build's `compile_commands.json` | Clang's analyzer on every `src/` and `apps/` source, with its own compile command, in parallel (about a minute on the M4 Pro). Any warning in the repository's own files, the analyzer's or the compiler's, fails the group; the SDK's and the dependencies' headers are left out. A source the analyzer cannot compile fails it too, and a compile database with none of this checkout's sources makes it unavailable. |
| UBSan | `-L cpu -LE "r1\|w1"` in the UBSan build | a UBSan build of the same revision | The CPU checks again under UndefinedBehaviorSanitizer, without R1's and W1's data runs, which take too long under it |
| GPU, headless | `-L gpu -LE smoke` | a Metal device | Metal rendering and retirement, GPU timing, the authored scene through the player's path, the visual references, resize/reload/play-reset steadiness, and the editor on Metal |
| Windowed | `-L smoke` | Metal and a logged-in desktop session | The real `maya_player` on the authored projects from another directory, packaged players, the player's failure exit codes, recording and replay, and debug views, the editor, player, and sample smoke runs, the desktop host, and benchmark CLI smoke runs, including `p1_small` |

**The UBSan build** is `<build>-ubsan` (or `MAYA_UBSAN_BUILD`). The script uses it but never builds it:

```bash
cmake -S . -B build-ubsan -DMAYA_ENABLE_SANITIZERS=ON -DMAYA_SANITIZERS=undefined && cmake --build build-ubsan -j 8
```

Without one, or with one built from another revision than the build checked (each build records its revision, `git describe --dirty` with a digest of uncommitted changes), UBSan is reported unavailable, with the command. AddressSanitizer and ThreadSanitizer are not used: on the reference machine's toolchain their executables hang or crash at startup (#1071).

**R1 and W1** are assembled from fetched samples, never committed. Without the render samples (`MAYA_RENDER_SAMPLES`, default `build/render-samples`), the checks labelled `r1` and `w1` are left out of every group; without the world samples (`MAYA_WORLD_SAMPLES`), those labelled `w1`. Each is reported unavailable, with the fetch script to run.

`MAYA_CHECK_GROUPS` runs some groups alone: any of `cpu`, `analysis`, `ubsan`, `gpu`, and `windowed`, separated by spaces or commas (default: all). A name that is not a group runs nothing and exits 2. The script ends by listing the manual steps and benchmark baselines as **manual**. Neither is run automatically: one needs a person, the other a quiet machine and a Release build.

### Logs, failures, and flaky tests

Each run keeps its logs in `<build>/milestone-logs/<date>-<time>/` (#1027). The summary ends with this folder's path.

- `environment.txt`: the date, the revision (`git describe --always --dirty`), the build type, the machine and macOS version, the load average, and `pmset -g therm`'s thermal state.
- `cpu.log`, `ubsan.log`, `gpu.log`, `windowed.log`: each CTest group's output, with `--output-on-failure`.
- `analysis.log`: each source the analyzer warned about, with its warnings and the paths it followed, or could not analyze, then the count.
- `<group>.<test>.rerun.log`: the output of each failed test, run again on its own.

The script runs each failed test again once, alone. A test that fails again is **FAILED**. A test that passes alone is **FLAKY**, and its group is not reported as passed. Both lines name the test and its logs.

| Exit code | Meaning |
| --- | --- |
| 0 | Every group that ran passed. Groups that could not run are still listed as unavailable. |
| 1 | At least one test failed twice. |
| 3 | No test failed twice, but at least one was flaky. |
| 2 | No configured build in the given directory, or an unknown group in `MAYA_CHECK_GROUPS`. |

`maya_check_milestone_selftest` (CPU) checks this reporting. It runs the script on a throwaway CTest project with a passing, a failing, and a flaky test, and checks the summary, the exit codes, and the logs. Since #1069 it also checks that UBSan is unavailable without a UBSan build, passes in one (the same project configured as one), and is unavailable for one of another revision; that static analysis is unavailable without a compile database; that a test labelled `w1` is left out and R1 and W1 reported unavailable without the samples, W1 alone with only the render samples, and run with both; and that commas separate groups and an unknown one exits 2.

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

**Process footprint.** In the load and play cycles, the footprint rises by about 200 MiB during the first cycle, then levels off. Since #1024 the cycles run 300 times and the slope is fitted over cycles 101–300: over cycles 11–100 it had failed intermittently near its limit (116 and 108 KB/cycle in #1016) while the footprint was still settling, though it levels off by cycle 200 with no later growth. All tracked counts are back at the empty session after every cycle. The retained memory is not attributed to engine allocations; it is consistent with allocator and driver retention. It is recorded as a plateau, not called a leak; #1063 has since attributed it ([performance](performance.md#first-observations)).

## Regression scenes

| Scene | Where | Purpose |
| --- | --- | --- |
| V1 visual reference | [v1_reference.scene](../samples/basic_scene/assets/v1_reference.scene) in the sample project | See below. |
| I1 repeated instances | [benchmarks/i1_*.benchmark](../benchmarks) (generated) | 1,000, 10,000, and 100,000 instances of one cube and one material; a subset view of 10,000 and (since #1066) of 100,000 |
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
- **Retained footprint.** The ~200 MiB footprint plateau after the first load cycle was unattributed here; #1063 attributed it to Metal's memory charged to the process and memory the allocator keeps, with 0 leaks ([performance](performance.md#first-observations)).
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
- **Double positions** (#1065, Jolt with `JPH_DOUBLE_PRECISION`; Release, back to back with the code before it, two invocations each, thermal state nominal at the start): the whole tick took 6.36–6.48 ms (P99 6.94–7.45 ms) against 6.14–6.35 ms (P99 6.67–8.72 ms), the step 2.51–2.56 ms against 2.54–2.66 ms, and Jolt's heap peaked at 40.4 MiB against 39.6 MiB, within the 8.5 ms and 48 MiB budgets. Every run ends in the same state, `62db8f85e75f9005`: positions are double, so it differs from the single-precision one above.
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

## Milestone 3: rendering and content

[Issue #1040](https://work.rezee.app/kash/issues/1040) closes the rendering and content milestone ([#1029](https://work.rezee.app/kash/issues/1029)): import content, author a realistic scene with physically based materials, physical lights, shadows, and an environment, play it, and ship it as a standalone package. The checks run in the same three groups as above. The [rendering and content record](architecture/rendering-content-decision.md) (#1030) chose the stack; its prototypes, behind `MAYA_BUILD_PROTOTYPES`, are removed now that the milestone's issues have replaced them.

In [check_milestone.sh](../tools/check_milestone.sh)'s groups: the CPU group adds `maya_content_acceptance_author`, `maya_r1_assemble`, and `maya_r1_steady`; the headless GPU group `maya_r1_reference`; and the windowed group `maya_content_acceptance_package` and `maya_package_gpu`, which launch real packaged players (since #1040 they carry the `smoke` label). Without the fetched samples, the script reports the R1 checks as unavailable.

### The content workflow, automated

[content_acceptance_tests.cpp](../tests/content_acceptance_tests.cpp) and [package_tests.cpp](../tests/package_tests.cpp), with two CTest entries chained by a fixture:

1. **`maya_content_acceptance_author`** (CPU) drives the editor on a fresh copy of the sample project in `build/acceptance/Content Game`.
   - **Import.** A glTF file and the normal map it names are saved into the project, as a modelling program would. The Scene menu's import catalogs its meshes, textures, and materials and places it in view.
   - **Edit.** The model is moved, turned, and renamed; a project material is dropped on its tile's Hierarchy row, and edited (rougher, orange).
   - **Light.** The new scene's sun casts shadows; a spot light comes from the Create menu and is aimed at the model; the workshop environment is used from the Assets panel. Nothing is skipped when drawn.
   - **Play.** A panel spins in Play only; Stop returns the scene's text to the authored one.
   - **Reload.** The normal map changes on disk: the editor imports the model again, the texture reloads, and the placement, name, and material survive.
   - **Save.** ⌘S writes `levels/lit` and the edited material at version 2; a fresh editor reopens the same scene text.
2. **`maya_content_acceptance_package`** (windowed) packages the project with `levels/lit.scene` and renders it from the sources and from the package: the same pixels. The packaged player, copied to a temporary folder with `MAYA_RESOURCES` unset, runs `levels/lit.scene` for 30 frames.

**Elsewhere in the milestone.**
- Textures, materials, lights, shadows, and environments: [texture_gpu_tests.cpp](../tests/texture_gpu_tests.cpp), [material_gpu_tests.cpp](../tests/material_gpu_tests.cpp), [lighting_gpu_tests.cpp](../tests/lighting_gpu_tests.cpp), and the editor's tests of each.
- Import, cooking, and reimport: [gltf_tests.cpp](../tests/gltf_tests.cpp), [import_tests.cpp](../tests/import_tests.cpp), [cook_cache_tests.cpp](../tests/cook_cache_tests.cpp), and [editor_import_tests.cpp](../tests/editor_import_tests.cpp); the Sample Viewer comparison in [sample_viewer_tests.cpp](../tests/sample_viewer_tests.cpp).
- Instancing and culling: [draw_batches_tests.cpp](../tests/draw_batches_tests.cpp); debug views: [editor_debug_view_tests.cpp](../tests/editor_debug_view_tests.cpp); animation: [animation_tests.cpp](../tests/animation_tests.cpp) and [animation_samples_tests.cpp](../tests/animation_samples_tests.cpp); packages: [package_tests.cpp](../tests/package_tests.cpp).

### R1

R1 is the realistic reference environment, recipe version 1 ([record](architecture/rendering-content-decision.md#r1-the-realistic-reference-environment)). Its content is fetched by [fetch_render_samples.sh](../tools/fetch_render_samples.sh), never committed, so **`maya_r1`** ([apps/r1](../apps/r1)) assembles the project from it:

```bash
tools/fetch_render_samples.sh                  # once: build/render-samples
maya_r1 build/render-samples build/r1-astc      # ASTC textures, as packages ship them
maya_r1 --compression rgba8 build/render-samples build/r1   # what the tests use: far faster to cook in unoptimized builds
```

- **Content.** ABeautifulGame's chess set on its board at the origin (0.7 m square), FlightHelmet to its left, and CesiumMan at half scale (0.75 m) walking a loop of 0.8 m radius around the board. An 8 m floor (grey, roughness 0.75), which the recipe left open, receives their shadows.
- **Light.** The Aerodynamics Workshop HDRI (2k) for the environment and the sky; a shadowed sun (3 lux), a shadowed spot light over the board (40 cd), and a warm and a cool point light (8 cd).
- **Motion.** `models/r1_paths.gltf`, which `maya_r1` writes, holds the **path camera** and the **walk loop** and one 10-second clip that turns both once around the board: the camera at 2.6 m and 1.35 m high, looking at the board, and the loop, which CesiumMan rides. The path camera is the scene's first camera, so the player, the editor's Game view, and the benchmark all follow it, through the [animation system](animation.md).
- **Views.** Five cameras, `View: overview`, `board`, `helmet`, `walker`, and `grazing` (a low view along the floor, for long shadows and cascade edges), with fixed IDs.
- **Again.** Assembling again imports again, which keeps every ID, and keeps the cook cache, so only the first load cooks.

CTest's **`maya_r1_assemble`** (fixture `r1_project`, skipped with 77 without the samples) writes `build/r1` with RGBA8 textures. In an unoptimized build, its first load cooks for about 6 minutes; later runs read the cook cache.

**The visual bar** ([r1_tests.cpp](../tests/r1_tests.cpp), `maya_r1_reference`):
- The five views at the first tick, and the path camera at ticks 1, 150, 300, and 450, match the references in [tests/references/r1](../tests/references/r1), at 640 × 360 with the usual tolerance. The project owner approved them on 7 October 2026, and on 10 October 2026 seven of them again, rendered as the player draws them with [levels of detail](renderer.md#levels-of-detail) (#1066): the board view, the grazing view, the walker view, and the four path frames, where 0.52–2.40% of pixels had changed, in fine texture and edges on the chess set, the helmet, and the walker. The views are rendered through a visibility index with the project's settings, and the editor-and-player comparison extracts once a tick on the player's side, as the editor draws a frame, since a level depends on the frames before.
- **The editor and the player** show the same image: the editor on Metal plays R1 in its Game view, held at tick 300, and every pixel of its viewport (between its tool bar and its "Click to play" hint) equals the player's path at the same size and tick.
- **The package** ([package_tests.cpp](../tests/package_tests.cpp), `maya_package_gpu`): R1 packaged through its warm cook cache renders each view, and the path after 300 ticks of play, pixel for pixel as from its sources, and the packaged player runs from a folder outside the checkout.
- **No acne, cascade seams, or shimmer** along the path: the cascades' stability and bias are tested in [lighting_gpu_tests.cpp](../tests/lighting_gpu_tests.cpp) (#1034), and the path is reviewed in the [manual script](#manual-script-rendering-and-content).

### Steady states, rendering and content

| Repeated work | Returns to | Checked by |
| --- | --- | --- |
| With R1 open in the editor: three unchanged reimports of CesiumMan, every texture reloaded, and 10 Play/Stop rounds of 60 ticks | The same device buffers, textures, samplers, and pending retirements, and the same resident and leased assets, as R1 first shown; the first Play loads the two clips, which the registry keeps | `maya_r1_steady` ([r1_tests.cpp](../tests/r1_tests.cpp)) |
| 40 Play/Stop rounds of the sample | Device buffers, textures, pipelines, and pending retirements at the empty session | `editor_play_tests.cpp` |
| Reimporting a model, unchanged or changed | Every ID kept; edited material and scene files kept | `import_tests.cpp`, `editor_import_tests.cpp` |
| Packaging the same project again, from a cold or a warm cook cache | Byte-identical packages | `package_tests.cpp` |
| Cooking the same content again | Byte-identical cook cache entries | `r1_import` benchmark |

### Regression scenes, rendering and content

| Scene | Where | Purpose |
| --- | --- | --- |
| R1 | assembled by `maya_r1` | The milestone's reference environment, its views, and its path |
| The material test scene | [materials.scene](../samples/basic_scene/assets/materials.scene) | Spheres across metallic and roughness, and textured surfaces, under two environments, and every debug view |
| The lights test scene | [lights.scene](../samples/basic_scene/assets/lights.scene) | Physical light units and shadows |
| MetalRoughSpheres, NormalTangentMirrorTest | fetched | Against the Khronos Sample Viewer |
| CesiumMan and Fox | fetched | Skinning, against [references](../tests/references/animation) and an independent evaluator |
| The content workflow | `levels/lit.scene`, written by the acceptance test | The milestone's workflow end to end |
| A1, I1, R1 | [benchmarks](../benchmarks) | Animation, instancing, and the reference environment's cost |

### Manual script, rendering and content

Run on the reference machine with a Release build, after `maya_r1 build/render-samples build/r1-astc`.

1. **Open R1.** `maya_editor build/r1-astc`: the scene opens on the board under the workshop. The first open cooks R1's textures (about 15 seconds); reopening is quick.
2. **Debug views.** From the eye menu, choose Base color, Normals, Roughness, Cascades, and Texels in turn, then Lit. Turn on Skeletons: CesiumMan's bones show through him.
3. **Play the path.** Press ⌘P and choose the Game view. The camera circles the board in 10 seconds while CesiumMan walks his loop. Watch the board's and pieces' shadows as the camera moves: no acne, no seams between cascades, no shimmer. Stop: CesiumMan is back in his authored pose, and the camera at its start.
4. **Edit a material.** Select a chess piece, unfold its material in the Inspector, and drag Roughness: every piece of that material changes at once. ⌘Z undoes it.
5. **Light.** Select the spot light and turn its outer cone wider: the pool on the board grows, with its shadow. Diagnostics lists the view's lights.
6. **Import.** Drag `build/render-samples/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb` onto the window: it is copied into `models/`, imported, and placed in view.
7. **Package.** `maya_package build/r1-astc R1.app`, then copy `R1.app` elsewhere and open it: the same path, with the same images.
8. **Benchmark.** `maya_benchmark benchmarks/r1.benchmark r1.json` on a cool machine: the results should be near the [baselines](#rendering-and-content-baselines).

### Rendering and content baselines

Measured for #1040 on 7 October 2026, on the Mac16,7 above (Apple M4 Pro, macOS 26.6.2), in a Release build of revision `49b64ed` plus #1040's changes, with R1 assembled with ASTC textures (`build/r1-astc`). The thermal state was nominal at the start and the end of every run. Two invocations of [r1](../benchmarks/r1.benchmark) after one that warmed the cook cache, each three runs of 600 warmup and 3,000 sampled frames at 1920 × 1080, offscreen, along the camera path.

**Per frame** (means of the six runs; one run was disturbed and is listed apart):

| | Mean | P95 / P99 |
| --- | --- | --- |
| GPU frame | 2.60–2.62 ms | P95 2.70–2.71 ms |
| `view` pass | 1.00–1.01 ms | |
| `sun shadows` (4 cascades) | 1.35–1.36 ms | |
| `spot shadows` (1 map) | 0.44–0.45 ms | |
| `tone map` | 0.07 ms | |
| CPU frame | 1.97–1.98 ms | P99 2.12–2.16 ms |
| of which waiting for the GPU | 1.86–1.87 ms | |
| simulation (animation 0.003 ms), extraction, encoding, submission | 0.010, 0.039, 0.052, 0.004 ms | |

R1 is bound by the GPU: the CPU's own work is 0.1 ms of a 2 ms frame, which it spends waiting for frames in flight. The disturbed run (the third of the first invocation) took 2.82 ms on the GPU (P95 4.08 ms) and 2.13 ms on the CPU (P99 3.85 ms), with the same counts.

**Double positions** (#1065, camera-relative rendering): Release, back to back with the code before it, two invocations each at thermal state nominal, with one disturbed run in each build set apart. The two builds match: frame 2.03–2.04 ms (P99 2.87–2.97 ms) against 2.02–2.04 ms (P99 2.85–3.01 ms), GPU P95 3.29–3.32 ms in both, CPU work (simulation, extraction, and encoding) 0.10–0.42 ms against 0.10–0.43 ms, Metal's allocation 565 MiB in both, and the warm first frame 1.64–1.72 s against 1.68–1.69 s. In this round the code before #1065 also measured a GPU P95 at the 3.3 ms budget rather than the 2.70 ms above, and CPU work up to 0.43 ms in some invocations: that drift is the machine's or an earlier change's, not #1065's, and is recorded for the next R1 review.

**Counts per frame:** 119 instanced draws of 289 instances and 8.0 million triangles, over 4 passes; 5 shadow maps (the sun's four cascades and one spot map) with 94.5 shadow draws; 3 point and spot lights, none left out or unshadowed; 57 mesh renderers, all in view; 1 skinned instance (CesiumMan, 19 joints).

**Memory**, with R1 resident:

| | |
| --- | --- |
| Textures (54, ASTC with mips) | 178.3 MiB |
| Meshes | 44.1 MiB on the GPU, 13.1 MiB of picking geometry |
| The environment (background and specular cube) | 22.3 MiB |
| Tracked device memory | 312.3 MiB of textures (with the view targets and shadow maps), 44.1 MiB of buffers |
| Reported by Metal | 565 MiB allocated |
| Process | 923 MiB footprint, 427 MiB resident (peak 447 MiB) |

**Loading** (through the cook cache): reading `r1.scene` 1.3–1.5 ms; building its World and starting play 1.7–2.3 ms; the first frame, which loads every mesh, texture, and the environment from the cache and uploads them, 1.66–1.67 s. With an empty cook cache, the first frame cooks everything: 12.8 s.

**Import and cooking** ([r1_import](../benchmarks/r1_import.benchmark), medians of three runs; every run cooked the same bytes):

| Model | Import | Cold load (cooking) | Warm load (cache) | Content |
| --- | --- | --- | --- | --- |
| ABeautifulGame | 89 ms | 8.37 s | 1.04 s | 15 meshes, 38 textures, 574k triangles; 173 MiB cached |
| FlightHelmet | 53 ms | 3.92 s | 0.45 s | 6 meshes, 15 textures, 95k triangles; 48 MiB cached |
| CesiumMan | 25 ms | 58 ms | 7 ms | 1 mesh, 1 texture, 4.7k triangles, a skin, and a clip |

### Rendering and content budgets

Approved on 7 October 2026 by the project owner, from the baselines above. They share the conditions of the [milestone 1 budgets](#budgets) (the reference machine, a Release build, user-interactive quality of service, nominal at the start and the end), on `r1.benchmark` with R1 recipe version 1 assembled with ASTC textures. A change to the recipe is a new version and needs a new baseline.

| Budget | Limit | Checked by | Observed | Rationale |
| --- | --- | --- | --- | --- |
| R1 GPU frame | P95 at most 3.3 ms | `r1` | 2.70–2.71 ms | About 20% above the observations; R1 is bound by the GPU |
| R1 CPU work per frame | Simulation, extraction, and encoding together at most 0.2 ms (means) | `r1` | 0.10 ms | The CPU frame is mostly waiting for the GPU, so it is not budgeted itself |
| R1 memory | Metal-reported allocation at most 700 MiB | `r1` | 565 MiB | About 20% above the observation |
| R1 warm load | The first frame through a warm cook cache at most 2.0 s | `r1` | 1.66–1.67 s | About 20% above the observation |
| Cold cooking and import | **No budget** | `r1`, `r1_import` | 12.8 s cold first frame; per model above | Recorded; cooking cost depends on the content's textures |

**Visibility and levels of detail** (#1066; Release, back to back with the commit before it, two invocations each, nominal throughout; [renderer](renderer.md#what-it-costs)). R1 draws 0.43 million triangles a frame instead of 8.0 million: GPU frame 1.54–1.75 ms (P95 1.71–1.87 ms) against 2.60–2.67 ms (P95 2.71–2.92 ms). Metal's allocation is 573 MiB against 565 MiB (the levels' indices), and the warm first frame 1.72–1.73 s against 1.64 s. CPU work (simulation, extraction, and encoding) is 0.08 ms in each invocation's first run and 0.25–0.28 ms in the others, over the 0.2 ms budget, with the same work and the same 0.64 ms frame: the frame is now a third as long, and the waiting thread's clock drops. Kept at speed by another process loading the performance cores, every run measures 0.074–0.077 ms, against 0.083–0.106 ms before #1066 under the same condition. The I1 budgets hold: `i1_10k` CPU P99 0.80–0.85 ms and GPU P95 1.04–1.07 ms; `i1_100k` CPU P99 9.63–9.99 ms and GPU P95 4.14–4.15 ms.

### Limits, rendering and content

- **One machine.** These are M4 Pro observations, as above.
- **R1's content is fetched.** Without it, the R1 checks are reported unavailable, not passed.
- **Shimmer is reviewed, not measured.** The automated checks hold cascade stability in test scenes and compare R1 at four points along its path; a person watches the whole path.
- **The editor's comparison** excludes its overlays (the Game view's tool bar and hint), which draw over the view.


## Milestone 4: world scale

The world-scale milestone ([#1059](https://work.rezee.app/kash/issues/1059)) adds checks as its issues land. The editor's side of worlds ([#1067](https://work.rezee.app/kash/issues/1067), [docs/editor.md](editor.md#worlds)) is covered automatically by [world_editor_tests.cpp](../tests/world_editor_tests.cpp): a world is created, filled across cells, linked across them, saved, reopened in part, and played. It is also covered by the overlay and Streaming panel references. A person checks what synthetic input cannot.

In [check_milestone.sh](../tools/check_milestone.sh)'s groups ([#1069](https://work.rezee.app/kash/issues/1069)): the CPU group adds `maya_world_acceptance_author`, `maya_w1_assemble`, and `maya_w1`; the headless GPU group `maya_w1_reference` and `maya_w1_editor`; and the windowed group `maya_world_acceptance_package`, `maya_w1_package`, and the S1 and soak smoke runs. Without the fetched samples, the script leaves the W1 checks out and reports them unavailable. The static analysis and UBSan groups cover the whole engine.

### The world workflow, automated

[world_acceptance_tests.cpp](../tests/world_acceptance_tests.cpp) and [package_tests.cpp](../tests/package_tests.cpp), with two CTest entries chained by a fixture (`world_project`). The creator writes no engine C++: a Luau script and its catalog line, written in a text editor, are the only files made outside the editor.

1. **`maya_world_acceptance_author`** (CPU) drives the editor on a fresh copy of the sample project in `build/acceptance/World Game`.
   - **Create a world.** New world, named `levels/valley` in its prompt. Its camera is moved up over the valley.
   - **Add terrain.** The Hierarchy's **+** menu, Terrain: a new `terrain/terrain.generator`, cataloged, named by a Terrain entity in the persistent part. Opened from the Assets panel, it becomes a gentle valley (4 octaves, 160 m, 6 m), and the terrain gets the grass material.
   - **Add scatter.** **+**, Scatter: `terrain/scatter.generator` over the terrain, named by a Scatter entity in the persistent part, given the sphere in the rock material, 8 m apart. Its items appear in the scene.
   - **Place and reference content across cells.** A crate dragged from the Assets panel into view (cell 0_0), a tower far east (2_0), a post south (0_-3), and a beacon by the crate (0_0), whose script points at the tower: a reference across cells.
   - **Save.** ⌘S writes the world in its parts and both generators' files with their edits.
   - **Stream and inspect residency.** The camera goes far away: every cell leaves, with the scatter's items, and the Residency panel's cells figure falls to 0. Back by the beacon with a 150 m radius, its cell loads from disk and the tower's does not; the Residency panel shows the loaded cells' cooked bytes, the same as the Streaming figures. Following the beacon's reference from the Inspector loads the tower's cell and selects the tower, and the cells' bytes grow.
   - **Play.** The play World streams around the world's camera: both cells active, the scatter's items in, and the beacon turned toward the tower across cells. Stop leaves the editor's world as it was, unchanged and saved.
   - **Reopen.** A fresh editor opens the world: nothing unsaved, its cells and scatter running again, the terrain and scatter in the persistent part, the crate and the beacon in 0_0, the beacon still pointing at the tower, and the world on disk as saved.
2. **`maya_world_acceptance_package`** (windowed) packages the project with `levels/valley.world`: its three cells cooked, both generators, the scatter's mesh, the grass, and the script. It streams the world from the sources and from the package as the player does, from the world's camera after the ticks Play starts with: every channel within one level on all but 0.01% of the pixels, as W1's. From the package, the terrain, the rocks, and the crate, the beacon, and the tower are in. The packaged player, copied to a temporary folder with `MAYA_RESOURCES` unset, runs `levels/valley.world` for 30 frames.

**Elsewhere in the milestone.**
- Jobs and loading: [jobs_tests.cpp](../tests/jobs_tests.cpp) and [asset_loading_tests.cpp](../tests/asset_loading_tests.cpp); residency: [editor_residency_tests.cpp](../tests/editor_residency_tests.cpp) and [r1_tests.cpp](../tests/r1_tests.cpp).
- Cells and streaming: [world_tests.cpp](../tests/world_tests.cpp), [streaming_tests.cpp](../tests/streaming_tests.cpp), [world_cells_tests.cpp](../tests/world_cells_tests.cpp), and [world_editor_tests.cpp](../tests/world_editor_tests.cpp); precision: [precision_tests.cpp](../tests/precision_tests.cpp).
- Visibility: [spatial_tests.cpp](../tests/spatial_tests.cpp) and [visibility_tests.cpp](../tests/visibility_tests.cpp); terrain and scatter: [terrain_tests.cpp](../tests/terrain_tests.cpp), [procedural_tests.cpp](../tests/procedural_tests.cpp), [scatter_tests.cpp](../tests/scatter_tests.cpp), [editor_terrain_tests.cpp](../tests/editor_terrain_tests.cpp), and [editor_scatter_tests.cpp](../tests/editor_scatter_tests.cpp).
- W1, S1, and the soak: [w1_tests.cpp](../tests/w1_tests.cpp), [benchmark_tests.cpp](../tests/benchmark_tests.cpp), and [below](#w1).

### W1

W1 is the reference world, recipe version 3: the [record's](architecture/world-scale-decision.md#w1-the-reference-world) version 1 with the project owner's changes of 12 and 14 October 2026 ([#1069](https://work.rezee.app/kash/issues/1069)). Its content is fetched by [fetch_world_samples.sh](../tools/fetch_world_samples.sh) and [fetch_render_samples.sh](../tools/fetch_render_samples.sh), never committed, so **`maya_w1`** ([apps/w1](../apps/w1)) assembles the project from it:

```bash
tools/fetch_world_samples.sh                    # once: build/world-samples
tools/fetch_render_samples.sh                   # once: build/render-samples (the landmarks)
maya_w1 build/world-samples build/render-samples build/w1-astc                  # ASTC textures, as packages ship them
maya_w1 --compression rgba8 build/world-samples build/render-samples build/w1   # what the tests use
```

- **The world.** `w1.world`: 128 m cells 32 to 63 along x and z (4,096 m to 8,192 m), loaded at 640 m and activated at 384 m with 64 m of hysteresis, written in `project.maya`. Its persistent part holds the terrain, the generators and their exclusions, the sun, the sky, and the cameras; every one of the 1,024 cells holds authored content, 17,323 entities in all.
- **Terrain.** W1's noise (seed 990, 6 octaves, 900 m, 140 m, 120 m of warp) over the whole world, its heights from −104 m to 107 m: `forest_ground_04` everywhere, and `rock_face_03` on slopes from 28° to 38°, their color, normal, and ARM maps repeating every 8 m.
- **Scatter** (version 2's spacings: scatter's must divide the 128 m cell):

  | Generator | Meshes | Spacing | Rules | Colliders |
  | --- | --- | --- | --- | --- |
  | Boulders | `boulder_01`, `namaqualand_boulder_02` | 8 m | density 0.44 (64/144, the 12 m set's count), slope ≤ 35°, scale 0.8–1.6 | spheres of 0.8 m and 0.7 m |
  | Rocks | two meshes each of `rock_moss_set_01` and `_02` | 4 m | density 0.4, scale 0.25–0.6 | none |
  | Deadwood | `tree_stump_01`, `dead_tree_trunk_02` | 8 m | density 0.19 (0.3 × 64/100, the 10 m set's count), scale 0.8–1.2 | spheres of 0.5 m |
  | Soak rocks | as Rocks | 4 m | as Rocks, over cells 44–45 × 43–44 only | none |

  The main rocks are kept out of the soak's 2 × 2 cells by a box exclusion naming them ("Soak cells"), and the soak rocks cover those cells: the soak gives them a new seed each cycle (`w1::soak_rocks_generator()`). A 45 m sphere ("Clearing") keeps every generator off the clearing.
- **The clearing** (the flattest ground near the middle, about 5,450 m by 5,620 m): ABeautifulGame's board and FlightHelmet at three times their size, and CesiumMan walking a 24 m loop at 1.2 m/s around the corner of cells 42_43, 43_43, and 42_44, 20 m from each edge, on the terrain's height. His walk loop is in `models/w1_walk.gltf`, which `maya_w1` writes, one clip with the terrain's heights in its keys, its node at rest where the clip starts.
- **The watch camera**, in the persistent part as a world keeps every camera, runs `scripts/watch.luau`, which turns it each tick toward CesiumMan's walk loop in its cell, a weak reference: while his cell is not in, it holds still.
- **Light.** The sun where the sky's is (48° up, toward −z and +x), 4.3 lux as the sky's sun is, with shadows to 150 m (decided 14 October 2026: at 250 m S1's GPU P95 reached 7.9–9.1 ms, over its 8 ms budget in most runs); the sky at half strength, since its own sun lights everything again unshadowed; and every camera at 1 EV100, a stop above R1's indoor 0.
- **Physics.** 400 rocks resting on gentle ground (under 15°) within 40 m of S1's walk, and 20 boulders on slopes of 25° to 35° within 80 m of it, which roll down once their cells are in. Each is a unit-scale body (rock, 2,600 kg/m³) with a box or sphere collider, its mesh scaled on an entity below it.
- **Cairns** (version 3, so S1 streams authored cells all along its path): in every cell, 16 static rocks from the rock sets' four meshes, scaled 0.4 to 0.9 and piled within 4 m of a point the cell's seed chooses, clear of the clearing, S1's walk, slopes over 25°, and (by 8 m) the resting rocks and boulders, which version 3 placed with a fixed seed of their own. Each is a static body: its box collider is its mesh's bounds, its scale baked in.
- **S1**, the traversal (version 2: the record's speeds and times): `models/w1_s1.gltf`'s camera, a key every tick, 9,600 ticks at 60 Hz. 120 s on foot at 8 m/s, 1.7 m over the terrain, east from the clearing along z = 5,640 m over the hill: 400 m, back 160 m, on 240 m, back 160 m, each turn-back across a cell edge; then a 40 s flyover at 60 m/s, 220 m up and north-east, looking 15° down: 3.36 km. It is the persistent part's first camera, so the player, the editor's Game view, and the packaged player follow it, and loop it. `w1::s1_point(tick)` gives the same path.
- **Views.** Five cameras with fixed IDs, each looking away from the sun: `View: clearing` (the board, the helmet, and CesiumMan), `hillside` (the steepest ground near the walk, sunlit), `vista` (from the highest ground, 2 km across the world), `flyover` (20 s into it, looking 35° down, so the world's edge is out of sight), and `cell-edge` (across the corner where cells 44_43, 45_43, 44_44, and 45_44 meet).
- **Again.** Assembling again imports again, which keeps every ID, writes the world's parts afresh, and keeps the cook cache.

CTest's **`maya_w1_assemble`** (fixture `w1_project`, skipped with 77 without the samples) writes `build/w1` with RGBA8 textures; `maya_w1` checks the recipe: the parts, the cameras, the generators' spacings, the 420 bodies, a cairn of 16 in every cell, and S1's length and turns.

**The visual bar** ([w1_tests.cpp](../tests/w1_tests.cpp), `maya_w1_reference`):
- The five views match the references in [tests/references/w1](../tests/references/w1), at 640 × 360 with the usual tolerance; the project owner approved them on 13 October 2026, and again on 14 October 2026 with version 3's cairns and 150 m shadows, with the rock layer's maps visibly repeating at a distance (the flyover), since the terrain material has no anti-tiling. Each is rendered as Play reaches it: W1 settled around S1's start, two ticks, a frame from S1's camera, then settled around the view. The test streams W1 as the player does (`tests/support/w1_play.hpp`): the terrain, then the cells once the terrain under them collides, then scatter once the cells are in, drawn with the poses between the last two ticks, through a visibility index with the project's settings.
- **The editor and the player** show the same image: the editor on Metal plays W1 in its Game view and, for each view, puts S1's camera where the view's camera is and streams around it frame by frame until nothing changes; every pixel of its viewport between its tool bar and its "Click to play" hint is within one level of the player's, but for at most 0.01% (an unoptimized build has shown 2 of 279,000 pixels differing in the flyover; Release, none).
- **The package** ([package_tests.cpp](../tests/package_tests.cpp), `maya_w1_package`): W1 packaged through its warm cook cache (1.8 GB with RGBA8 textures) streams and draws each view within one level of its sources on all but 0.01% of the pixels (it makes terrain and scatter as it runs, with no cook cache: at most 4 pixels in 129,600 differed), and the packaged player runs from a folder outside the checkout.
- **No seams** at the cell edges, in the cell-edge view; **no LOD popping** and **shadows on terrain and scatter**, in the views and along S1 in the [manual script](#manual-script-w1).

### S1 and the soak

S1 and the soak run as [benchmark manifests](performance.md#traverse-and-soak): [s1](../benchmarks/s1.benchmark) traverses W1 three times along S1's camera path, and [soak](../benchmarks/soak.benchmark) runs 20 cycles of loading W1, 60 s of S1, giving the soak's rocks a new seed, resetting, and unloading. Both stream W1 as the player does, at 1920 × 1080 offscreen, with 5% of the cells (chosen by their index) loading 500 ms late. Assemble `build/w1-astc` first and run `s1` once, so the cook cache is warm. S1 in the editor's Play is measured by a hidden test in [w1_tests.cpp](../tests/w1_tests.cpp): set `MAYA_W1_BENCHMARK_PROJECT=build/w1-astc` and `MAYA_W1_BENCHMARK_OUT=<file>`, then run `maya_editor_tests "[w1-benchmark]"` in a Release build.

#### W1 baselines

These are on the M4 Pro reference machine, in Release, on 14 October 2026, against W1 recipe version 3 with 150 m shadows (revision 5926d15 and stage 2's changes).
- **S1's three traversals** ran at thermal state nominal, undisturbed.
- **The soak's first three cycles are set apart**, as the [protocol](architecture/performance-baseline.md#measurement-protocol) says: other activity pushed their GPU P95 to 7.9–10.9 ms. The soak started and ended at thermal state "fair". Cycles 4–20 are measured here.

| Measure | S1 | Soak (60 s of S1 a cycle; cycles 4–20) |
| --- | --- | --- |
| Frame | P50 3.94–4.03, P95 4.83–5.02, P99 5.63–6.26 ms | P50 4.17–4.62, P95 4.80–5.82, P99 5.14–6.84 ms |
| GPU P95 | 6.27–6.58 ms (on foot; the flyover is 2.3–2.6 ms) | 6.36–7.13 ms |
| CPU work P95 (simulation with streaming, extraction, encoding) | 2.64–3.87 ms | 1.32–3.61 ms |
| Activation frames, against the steady frame (P50) | P99 +0.9–1.0 ms; longest +2.0–3.2 ms | P99 and longest +0.5–2.0 ms |
| Warm first frame | 2.84–3.60 s: terrain 31–34 ms, cells 1.0 s (the delayed ones), scatter 50–77 ms, assets 1.7–2.4 s | 2.73–2.97 s |
| Cells | 365 loads a traversal (26 delayed, none cancelled, none failed), 219 activations, none waiting for the terrain | 132 loads a cycle (11 delayed), 62 activations |
| Resident peak (categories and terrain) | 521 MiB: textures 259, meshes 98, renderer 96, terrain 45, environments 22, cells 0.8 | the same |
| After unloading | 95.67 MiB tracked; nothing stale; no jobs, loads, or leases outstanding | 95.67 MiB tracked after all 20 cycles; footprint 930–1,277 MiB, slope −10.3 MB a cycle; nothing stale or outstanding after any |
| The soak's regeneration | | 16–39 ms from the new seed until its cells were made again |
| Unresolved references | the watch camera's, in 2,199 frames of each traversal, while CesiumMan's cell is out (the flyover) | none |

**Activation frames** are no slower than others: their P99 is at or below the other frames' P99 in every run. Single frames hitch to 16–71 ms regardless (the first frames after loading, and the system), so the longest frame of all measures the machine more than streaming.

**CPU work** depends on the clock its thread runs at. The frame waits for the GPU, and the waiting thread's clock drops, as R1's did in #1066. Within one traversal, its P95 over 10-second windows moves between about 3.8, 2.2, and 1.3 ms for the same work; with the performance cores held busy by other processes, it stays at 1.4–3.2 ms.

**S1's cancellations.** The record expected S1's turn-backs to cancel two loads; none are cancelled. A cell starts loading at the 640 m radius, and a load is cancelled only past 704 m (the radius and its hysteresis): 8 s of walking back at 8 m/s, while even a delayed load finishes in 0.5 s. Cancelled loads are covered by the [streaming tests](world.md#tests) instead.

**S1 in the editor's Play** (recorded, not budgeted): the Game view is 1164 × 680 inside a 1920 × 1080 window. Frame P50 3.13, P95 3.96, P99 4.44 ms; GPU P95 4.48 ms; CPU work (update and render) P95 3.89 ms; Play's start, settling around S1's start, 0.9 s.

#### W1 budgets

Approved by the project owner on 14 October 2026; they were first approved with CPU work at 3 ms and the longest activation frame at +3 ms, then set to these after a second measurement. They apply to undisturbed runs on the M4 Pro at 1920 × 1080 in Release, started at thermal state nominal, and are checked by running the manifests on the reference machine, as the others are ([budgets](#budgets)).

| Budget | Limit | Checked by | Observed | Rationale |
| --- | --- | --- | --- | --- |
| S1 GPU frame | P95 at most 8 ms | `s1`, `soak` | 6.27–7.13 ms | The record's; W1's shadows end at 150 m (at 250 m it was 7.9–9.1 ms) |
| S1 CPU work | P95 at most 4 ms | `s1`, `soak` | 1.32–3.87 ms | The record's; it moves with the clock of the waiting thread |
| Activation frames | P99 at most the steady frame (P50) + 2 ms, and the longest + 4 ms | `s1`, `soak` | P99 +0.5–2.0 ms; longest +0.5–3.2 ms | The record's +2 ms on the longest frame alone would measure one frame in 9,600, the machine's hitches more than activation |
| Resident memory | Peak at most 640 MiB in S1 | `s1` | 521 MiB | About 20% above; well within the 1.5 GiB residency budget, which the record proposed |
| Warm first frame | At most 4 s | `s1`, `soak` | 2.73–3.60 s | Includes S1's delayed cells (1.0 s); the record proposed 3 s |
| Soak plateau | Tracked memory after unloading equal every cycle; footprint slope at most 1 MiB a cycle from cycle 2; nothing stale or outstanding after any cycle | `soak` | 95.67 MiB every cycle; −10.3 MB a cycle; none | Unloading returns exactly what loading took |

### Manual script, world editing

Run on the reference machine with a Release build, in a copy of the sample project (`cp -R samples/basic_scene ~/Desktop/Worlds`, then `maya_editor ~/Desktop/Worlds`).

1. **Create a world.** From the scene menu, choose New world… and accept `levels/untitled.world`. The world opens with a camera and a sun, and the Hierarchy is headed WORLD. The Streaming panel shows its cells.
2. **Fill it.** Drag `cube` into the viewport a few times. Then fly east with the right mouse button held and WASD for a few hundred metres, and drop more cubes there. Each root's cell shows at the right of its Hierarchy row.
3. **Watch cells come and go.** Save with ⌘S, then press G for the Cells tool. Fly back west. The overlay shows the cells around the camera loaded, the far ones going, and Streaming's counts and frame times following. Nothing in the top bar says "modified".
4. **Use the Cells tool.** Click a far cell and choose Load and lock: it loads and shows a lock, and stays when you fly away. Drag across several cells and choose Unload: they go. Choose Let the camera decide: they come back when near.
5. **Edit across a boundary.** Select a cube and drag it with the gizmo into the next cell. Its row's cell changes, and both cells are outlined as changed. Fly far away: the changed cells stay loaded. ⌘Z moves it back, and the outlines go.
6. **Keep a hierarchy together.** Drag a second cube onto the first cube's Hierarchy row to make it a child. Move the child far into another cell with the gizmo. The move stops at the boundary, and the Inspector says why, under the entity's name.
7. **Follow a reference.** Give a cube a script with an entity property and choose, as its target, a cube in a cell that is not loaded, from that cell's list. The cell loads. Save, fly away until the target's cell unloads, then select the first cube: its target shows as unloaded, with its cell. Press the button beside it. The cell loads, the target is selected, and the view frames it.
8. **Save and reopen.** Press ⌘S, quit, and open the world again from the scene menu. Only the cells near the camera load, and everything is as it was left.
9. **Play and Stop.** Press ⌘P. Streaming shows the play World's cells around the play camera. Press ⌘P again: the editor's loaded cells are as they were, and nothing is unsaved.

### Manual script, the world workflow

Run on the reference machine with a Release build, in a copy of the sample project (`cp -R samples/basic_scene ~/Desktop/Valley`, then `maya_editor ~/Desktop/Valley`). It is the automated workflow as a person does it, with what synthetic input cannot see.

1. **Create a world.** From the scene menu, choose New world… and name it `levels/valley`.
2. **Add terrain.** Press **+** in the Hierarchy and choose Terrain. A Terrain entity appears and is being renamed, Diagnostics logs "Created terrain/terrain.generator", and its row appears in the Assets panel's GENERATORS column. Click that row and lower its amplitude and wavelength in the Inspector: the ground reshapes as you drag. Select the Terrain and choose the grass material for its first layer.
3. **Add scatter.** Press **+** and choose Scatter. Click `terrain/scatter.generator` in the Assets panel, choose the sphere as its first mesh and the rock material for it. Rocks appear on the terrain around the camera, resting on the ground.
4. **Place content across cells.** Drag `cube` into view. Fly a few hundred metres east and add another cube there. Back at the first, add a third cube and give it a script with an entity property (such as the workflow test's `beacon.luau`, cataloged as [scripting](scripting.md) says), and choose the far cube as its target from its cell's list.
5. **Save.** Press ⌘S. The top bar no longer says "modified", and both generator files have your values.
6. **Stream and watch residency.** Open the Streaming and Residency panels and press G for the Cells tool. Fly away and back: cells, rocks, and terrain come and go around the camera, the Streaming counts follow, and the Residency panel's cells bar grows and shrinks with them.
7. **Play.** Press ⌘P. The Game view shows the terrain, the rocks, and the cubes from the world's camera; the scripted cube turns toward its target in the far cell. Press ⌘P again: the editor's cells are as they were, and nothing is unsaved.
8. **Package.** Quit the editor, then run `maya_package ~/Desktop/Valley Valley.app --scene levels/valley.world` ([projects](projects.md#packages)) and copy `Valley.app` to another folder.
9. **The package plays the same.** Open it with `Valley.app/Contents/MacOS/maya_player levels/valley.world`. It shows what Play showed: the same terrain, rocks, and cubes, and the scripted cube turning.

### Manual script, W1

Run on the reference machine with a Release build, after assembling `build/w1-astc` as [above](#w1).

1. **The player follows S1.** Run `maya_player build/w1-astc/project.maya`. The first frame shows the clearing with the board, the helmet, and CesiumMan walking. The camera walks east over the hill, turns back three times, then flies north-east 220 m up, and starts again at the clearing after 160 s.
2. **Look for seams and popping.** While it walks, watch the ground at cell edges and the scatter at a distance: no cracks, no gaps between tiles, and no rocks or boulders visibly changing shape as the camera passes.
3. **Look at the shadows.** Rocks, boulders, deadwood, and the landmarks cast shadows on the terrain, and hills cast shadows on each other near the camera.
4. **Play in the editor.** Press ⌘P. The Game view follows S1 as the player does, and the Streaming panel shows cells coming and going around the play camera. Stop: the editor's loaded cells are as they were.
