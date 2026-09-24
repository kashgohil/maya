<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="maya.svg">
    <img alt="Maya" src="maya.png" width="168">
  </picture>
</p>

<h1 align="center">Maya</h1>

<p align="center"><strong>A game engine for realistic worlds you can explore, and then change with your hands.</strong></p>

Maya is for games that ask you to pay attention. A large place. Light that belongs to the hour. Water you can enter. Weather that changes how far you can see, and how you move. Objects you can pick up, carry, and build with. Creatures that occupy the same space you do. You leave, and the world is still there when you come back.

It is a single-player engine. The work is the place, the way a person moves through it, and the tools that let someone make that place. A finished game runs on its own. The editor stays with the person who is building.

What runs today is the foundation that has to hold all of that: a Mac application that opens a window, draws a scene, and lets you fly a camera through it. The player and the editor are already two different programs.

## The loop it is built around

Author a zone. Enter it when you want to be inside it. Step back out, and the zone you authored is still the one you saved. What you did while playing stays in that session unless you choose to keep it.

The player opens that same zone with no editor attached. The world you test is the world someone else will play.

## Move

Getting around is the first thing the world has to get right.

On foot, where there is ground. Swimming, where there is water. Flying, when the space opens up. Climbing a surface that would otherwise stop you. Stairs, ledges, and steep ground should behave. Crossing from air into water should feel like entering a different place.

The volume you are in changes the movement. Water, open air, and the character of a biome each carry their own feel, their own sound, and their own look. An underwater stretch has its own light, its own drag, and its own horizon.

## Touch

A sandbox is a world that lets you do something to it.

Pick an object up. Place it. Start a structure. Keep what you are carrying. The engine is growing an interaction model for this, so a game can spend its effort on what is worth picking up.

What you change is worth keeping. A save covers the parts of the world you have altered, including zones that were not in memory when you put the game down. Loading brings back that same place.

## Travel

The world is meant to be larger than one room loaded at startup.

Biomes stream in as you travel and release what you have left behind. Crossing from one kind of country into another is a change of ground, light, sound, and what can live there. The machine you are playing on keeps the country around you.

From far enough away, detail steps down. Up close, it is the real thing. You should be able to go somewhere without waiting on a corridor, and you should be able to come back to a thing you built and find it where you left it.

## Light

Realistic, here, means light you can direct, and materials that hold up under it.

Maya is heading toward lighting done in linear color, exposure you can set, and a tone map so a hard sun and a dark interior can share an afternoon. Surfaces start simple and grow into materials with color, metal, and roughness that an artist can tune. Fog sits in the air. Underwater, light falls off the way it does in water.

A scene that looks like this still has to feel immediate. Light you can believe only counts if the world keeps its pace while you move through it.

## Life

The player is not the only body in the world.

Characters and creatures move on skeletons: a performance, a blend between performances, a mesh that follows the pose. They need somewhere to go, including through spaces that are not a flat floor, so finding a path has to understand a volume. On top of that sits behavior. A creature can patrol, keep its distance, come closer, and notice you.

You should be able to tell why a creature did what it did. The same tools that run the decision can show it.

Sound belongs in the same place. A mix you can shape. A sound that sits on a thing in the world, and falls off as you leave it. Weather, water, and a biome each get a voice.

## Make

The editor is how the world gets made.

A view of the scene. A way to select something and move, turn, or scale it. An inspector for the thing you selected. A browser for the meshes, materials, and other assets behind it. Volumes for water, air, and biome, placed in the world as objects you can edit. Change a material, a mesh, or a behavior and see the result while the editor is still open.

A new zone should be something one person can author and revise without editing the engine in C++.

The editor today is the window that work will land in. It has a hierarchy, a viewport, an inspector, an asset list, and a diagnostics panel, docked and resizable. The scene's structure can be edited and undone; properties and saving are next. It is already a separate program from the player, so the game you give to someone else is only the game.

## What you can run today

Three programs build from this repository.

**Player.** The program a finished game grows out of. It opens the sample's saved scene: a pyramid and cubes on a ground slab, lit by a directional sun, seen through the scene's camera, which you fly freely. The view renders offscreen and is then presented in the window. It cannot choose a different project yet. Project selection is still in its entry point.

**Editor.** An editing window with dockable panels around a viewport of the sample scene. Hold the right mouse button over the viewport to fly: WASD moves, Q and E go down and up, and Shift is faster. Scroll over it to move forward and back. Typing in a field never moves the camera. In the hierarchy you can create, rename, duplicate, delete, and drag entities to reparent them, and undo or redo any of it with ⌘Z and ⇧⌘Z. Editing properties and saving come next.

**Sample.** The same scene as the player, kept as a sample, so the demo can stay a demo while the player becomes a game.

In the player and the sample, you look with the mouse and move with WASD. Space rises. Escape closes the window; in the editor, Escape only stops flying. The title bar carries a smoothed frame rate, the frame time, and the size of the view in pixels.

The scene is small on purpose. It is the loop the rest of the engine has to survive: open a window, draw frames, shut down cleanly, and be willing to do it again.

## Under the hood

Maya is written in C++20 and runs on macOS. Drawing goes through Metal. The runtime, the desktop window, the editor, and the sample are separate pieces. A game can be built without the editor. The editor can be built without the sample. The window outlives the graphics device: startup that fails gives back what it took, and a session that has ended can start again.

Worlds are measured in metres, seconds, and kilograms. The space is right-handed, with up on Y. Those are the rules the later systems are being written against.

The contracts for identity, time, and rendering are in the [architecture notes](docs/architecture/README.md). The path from this foundation to a shippable sandbox is the [roadmap](ENGINE_ROADMAP.md).

## Setup

A Mac, Xcode Command Line Tools, and CMake 3.20 or newer. The first configure fetches pinned GLFW and Catch2 sources.

```bash
cmake -S . -B build
cmake --build build -j 4

./build/maya_player
./build/maya_editor
./build/maya_sample
```

`maya_player` and `maya_sample` capture the mouse. `maya_editor` captures it only while you fly. The editor fetches Dear ImGui the first time it is configured; a build without the editor never downloads it. Add `-DCMAKE_BUILD_TYPE=Release` when configuring a release build. CMake writes `build/compile_commands.json` for clangd.

### Where the files are

Shaders live in `resources/shaders/metal/`; the editor's fonts (Inter and Geist Mono, SIL Open Font License) and icons (Phosphor, MIT) live in `resources/fonts/`. The sample's catalog, scene, meshes, and materials are in `samples/basic_scene/assets/`.

Maya searches in this order:

1. `MAYA_RESOURCES`, when it is set.
2. Parents of the executable.
3. The working directory.

A build made inside the repository finds its content, because a parent of the executable is the repository. For a binary copied elsewhere, point `MAYA_RESOURCES` at a tree that still contains `resources/` and `samples/basic_scene/assets/`.

### Build only what you need

| Target | What it is |
| --- | --- |
| MayaRHI | The graphics device API: validation, handles, resource retirement, and a CPU-only test device. |
| MayaRenderer | Turns a world into images: extraction, camera views, offscreen targets, and presentation. |
| MayaRuntime | The engine session, core utilities, and Metal. No window and no editor. |
| MayaDesktop | The window, input, and launch loop. |
| MayaBasicScene | The sample scene. |
| MayaEditor | The editor: panels, viewport, and input routing, on Dear ImGui. |
| maya_player | The player. |
| maya_editor | The editor. |
| maya_sample | The sample. |

Runtime only:

```bash
cmake -S . -B build/runtime-only \
  -DMAYA_BUILD_EDITOR=OFF -DMAYA_BUILD_PLAYER=OFF \
  -DMAYA_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build build/runtime-only --target MayaRuntime
```

Player, without the editor or the tests:

```bash
cmake -S . -B build/player-only \
  -DMAYA_BUILD_EDITOR=OFF -DMAYA_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build build/player-only --target maya_player
```

### Check it

```bash
ctest --test-dir build -L cpu --output-on-failure
ctest --test-dir build -L gpu --output-on-failure
```

CPU tests never open a window. GPU tests need a Mac session with a display, and they open one briefly. Each program accepts `--help` and `--smoke N`. N is a positive frame count, and it defaults to 120 when omitted. Smoke mode still uses a real window and Metal, steps at a fixed 1/60 of a second, ignores the camera, and exits with a failure if those frames do not complete. It checks the session, not the pixels. GPU tests run with Metal API validation, and the device tests read rendered pixels back.

Sanitizers, in their own build directory:

```bash
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug -DMAYA_ENABLE_SANITIZERS=ON
cmake --build build/sanitized -j 4
ctest --test-dir build/sanitized -L cpu --output-on-failure
```
