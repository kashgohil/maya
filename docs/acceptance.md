# Milestone acceptance: author, save, and run a scene

[Issue #1005](https://work.rezee.app/kash/issues/1005) closes the first milestone ([#988](https://work.rezee.app/kash/issues/988)). This page is the acceptance record:

- the automated checks, split by what they need;
- a short manual script for what only a person at the machine can check;
- the steady states that repeated work must return to;
- the scenes kept for regression;
- the benchmark baselines, and the limits of what they show.

## Automated checks

```bash
cmake -S . -B build && cmake --build build -j 8
tools/check_milestone.sh build
```

[check_milestone.sh](../tools/check_milestone.sh) runs three CTest groups. It runs a group only when what it needs is there, and reports every group that could not run as **unavailable**, with the reason. It never reports a skipped group as passed.

| Group | CTest selection | Needs | What it covers |
| --- | --- | --- | --- |
| CPU | `-L cpu` | nothing else | World, properties, scenes, assets, projects, simulation, metrics, the editor on the null device (including the authoring half of the acceptance workflow), the benchmark runner on the null device, argument handling, and UI isolation |
| GPU, headless | `-L gpu -LE smoke` | a Metal device | Metal rendering and retirement, GPU timing, the authored scene through the player's path, the V1 visual references, resize/reload/play-reset steadiness, and the editor on Metal |
| Windowed | `-L smoke` | Metal and a logged-in desktop session | The real `maya_player` on the authored project from another directory, the player's failure exit codes, the editor, player, and sample smoke runs, the desktop host, and benchmark CLI smoke runs |

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
| Loading, playing, and unloading a 10,000-entity scene 100 times | No buffers or resident meshes beyond the empty session. Two textures (the view target), no pending retirements, and no outside leases. | `l1_load` benchmark; `benchmark_tests.cpp` |
| Starting and stopping 100 play sessions from one authored scene | The same, and the authored scene is unchanged | `l1_play` benchmark; `benchmark_tests.cpp`; 40 editor Play/Stop rounds in `editor_play_tests.cpp` |
| Resizing a view 200 times across five sizes | Two textures sized for the current size, no pending retirements. The first size's texture no longer resolves, and Metal's reported memory grows by at most 32 MiB. | `acceptance_tests.cpp` |
| Evicting and reloading an asset | Its old handle resolves as stale; the reload is a new generation | `acceptance_tests.cpp` |
| 25 play resets | No handle from one session's World is alive in the next; no outside leases once the sessions end | `acceptance_tests.cpp` |

**Process footprint.** In the load and play cycles, the footprint rises by about 200 MiB during the first cycle, then stays flat for the other 99. All tracked counts are back at the empty session after every cycle. The retained memory is not attributed to engine allocations; it is consistent with allocator and driver retention. It is recorded as a plateau, not called a leak.

## Regression scenes

| Scene | Where | Purpose |
| --- | --- | --- |
| V1 visual reference | [v1_reference.scene](../samples/basic_scene/assets/v1_reference.scene) in the sample project | See below. |
| I1 repeated instances | [benchmarks/i1_*.benchmark](../benchmarks) (generated) | 1,000, 10,000, and 100,000 instances of one cube and one material; a subset view |
| L1 load/unload and play reset | [benchmarks/l1_load.benchmark](../benchmarks/l1_load.benchmark), [l1_play.benchmark](../benchmarks/l1_play.benchmark) | 100 cycles of a 10,000-entity scene; malformed and missing-asset scenes |
| The sample scene | [basic.scene](../samples/basic_scene/assets/basic.scene) | The fast regression case; not evidence of scale |

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
- **Build:** Release, at revision `7329fda` plus the #1005 runner changes. The engine code is that of `7329fda`.
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
| `l1_load`: a 10,000-entity scene from its file | fair → fair | 39.7 / 46.4 | 2.48 | 0.13 | back at the empty session | 218 MiB empty; 406–415 MiB; slope +41 KB/cycle |
| `l1_play`: play sessions from the authored scene | fair → fair | 6.9 / 7.2 | 2.39 | 0.13 | back at the empty session | 218 MiB empty; 402–408 MiB; slope +37 KB/cycle |

- **Refusals.** The malformed and missing-asset scenes were refused, with nothing left behind.
- **Authored scene.** It was unchanged after the play cycles.
- **Footprint slopes.** Earlier runs of the same workloads measured slopes from −25 to +54 KB/cycle, within about ±10 MiB of cycle-to-cycle variation. The data shows no unbounded growth.

### Budget proposals

No budget was agreed before this milestone. The following are **proposals** from the observations above, for review. They are not approved budgets, and they hold only for this machine and these workloads.

| Proposal | Observed | Why this value |
| --- | --- | --- |
| Tracked counts return exactly to the empty session after load, play, and resize cycles | Exact in every run | Already enforced by tests; any drift is a defect |
| Process footprint slope over cycles 11–100 at most 100 KB/cycle | −25 to +54 KB/cycle | About twice the observed spread; flags growth of 10 MiB per 100 cycles |
| I1 10k CPU frame P99 at most 2.5 ms, GPU P95 at most 1.5 ms, on a cool M4 Pro | 2.04–2.13 ms, 1.14–1.22 ms | About 20% above the observations. Tighten once repeated baselines show the variance. |
| Loading a 10,000-entity scene: P95 at most 50 ms; starting play: P95 at most 10 ms | 38.5–46.4 ms; 6.6–7.6 ms | Headroom over warm and cool observations |
| I1 100k: **no budget** | 20.5–20.8 ms CPU, over the proposed 16.67 ms target | A stress input, not a capacity. Instancing and culling come first. |

## Limits against the workload contract

- **Reference hardware.**
  - The contract's Baseline profile (Apple M1, 8-core GPU, 16 GiB, 1920×1080) and Headroom profile (M2 Pro) were not available. Both are **unmeasured**.
  - The measurements above come from an M4 Pro. They are that machine's observations and are not interchangeable with either profile.
- **Scale.** The sample scene is not evidence of large-game readiness, and neither is I1: its 100,000 instances are a stress input, not a promised capacity.
  - At 100,000 instances a frame takes about 21 ms of CPU time, which is over the proposed 16.67 ms interactive target. The renderer encodes one draw with its own constants per instance and does not cull. On this machine, the full 100k protocol also runs into thermal throttling.
  - Instancing, batching, and culling are the known next steps.
- **Not implemented, so not measured:** per-pass GPU timing, presentation interpolation, streaming (S1), physics, scripting, texture and PBR/shadow quality, import and cook times, and edit-to-preview latency.
- **Present pacing** is not measured: benchmarks render offscreen. The editor's live display shows the frame interval, but it is not a controlled measurement.
- **Retained footprint.** The ~200 MiB footprint plateau after the first load cycle is unattributed. Attributing it needs Instruments on the reference hardware.
- **Cold-cache loads.** The OS file cache is not controlled, so every load time is a warm-cache time.
