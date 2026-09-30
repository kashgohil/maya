# Scripting

[Issue #1018](https://work.rezee.app/kash/issues/1018) runs [Luau 0.740](architecture/physics-scripting-decision.md#luau) scripts in play sessions. A script is a project asset, attached to an entity by a `maya.script` component, and it runs in the player and in the editor's play mode. The design follows the [scripting boundary](architecture/runtime-world-contracts.md#scripting-boundary) and the [fixed-tick phases](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick).

Later issues add to this:
- **#1020:** the Assets panel's script column, dragging scripts onto entities, and reloading edited scripts.
- **#1021:** body requests, queries, events, and `late_fixed_update`.

## A script

The sample project's [spin.luau](../samples/basic_scene/assets/scripts/spin.luau) turns its entity like the built-in `maya.spin` component. The Beacon in [physics.scene](../samples/basic_scene/assets/physics.scene) uses it:

```lua
local Spin = {}

Spin.properties = {
    axis = { type = "vector", default = vector.create(0, 1, 0), label = "Axis" },
    speed = { type = "number", default = math.pi / 4, unit = "rad/s", label = "Speed" },
}

function Spin:fixed_update(dt: number)
    if vector.magnitude(self.axis) <= 1e-6 or self.speed == 0 then
        return
    end
    local turn = maya.quaternion.from_axis_angle(self.axis, self.speed * dt)
    self.entity:set_rotation((self.entity:rotation() * turn):normalized())
end

return Spin
```

- **What a script returns.** A table: its hooks, and an optional `properties` table. It returns nothing else.
- **Instances.** Each entity with the script gets its own instance, `self`. `self.entity` is the entity, and each declared property is a field such as `self.speed`. Keep per-entity state in `self`; script globals are per script, not per entity.
- **Types.** Optional Luau type annotations, such as `dt: number`, are accepted and not checked.

## Script assets and the component

- **Catalog entries.** A catalog lists scripts as `script <high> <low> "<path>"`, for example `script 6d617961 20 "scripts/spin.luau"`.
- **Loading.** The registry reads them as source text (`ScriptAsset`, loaded by `AssetProvider::load_script`, which any provider inherits). Play sessions and the editor compile the source; bytecode is never stored or loaded.
- **`maya.script` (ID 11)** has two properties:
  - `script`, a script asset reference;
  - `values`, the values of the script's declared properties, by name.

  In a scene file the values are one line: a count, then each value's name, type, and data.

```text
  component maya.script 1
    script 6d617961 20
    values 2 speed number 1.5 target entity 6d617961 300
```

An entity has at most one script. The Inspector shows the script and one field per declared property ([Inspector](inspector.md#script-components)).

## Properties

`properties` maps each name to a declaration:

| Field | Meaning |
| --- | --- |
| `type` | Required: `number`, `integer`, `boolean`, `string`, `vector`, `color`, or `entity`. |
| `default` | Optional. Without one: 0, 0, false, "", the zero vector, white, and no entity. |
| `min`, `max` | Optional, for number and integer properties. |
| `unit`, `label` | Optional text for the Inspector. |

- **Names.** They are identifiers such as `speed` or `jump_height`, and are listed by name, since Lua tables have no order.
- **Instance values.** When an instance starts, each property takes its value from the component if one is stored under that name, with that type, within the range. Otherwise it takes the default.
- **Values that do not fit.** A value of another type, out of range, or for a property the script does not declare is reported as a warning. An undeclared value is kept in the scene, not dropped; the Inspector lists it with a **Remove unused values** button.

`describe_script(name, source)` compiles a script and runs its top level in a fresh sandbox to read its declarations and hooks, without running any hook. `script_value_problems(description, values)` explains values that do not fit.

## Hooks

| Hook | When |
| --- | --- |
| `start(self)` | Once, when the instance starts: at the tick boundary, before its first `fixed_update`. At the start of play this is in document order; an entity that gets a script during play starts at the next tick. |
| `fixed_update(self, dt)` | Every tick, before physics, after the built-in systems. Instances run in the order they started. |
| `update(self, frame_dt)` | Once per host frame after its ticks, also while paused. Read-only. |
| `stop(self)` | Once, if `start` ran. When the entity is destroyed, loses its component, or gets another script, this is the next tick, and the entity may already be gone (`self.entity:alive()` is false). At the end of play, newest first, with no world left to change. |

A script's hooks are the functions its table holds when it loads; functions added to the table later are not called. The scripts run as one system, `script_system`, which `play_systems` places after the built-in ones.

## The engine API

Scripts reach the engine only through the read-only `maya` table and methods on entity values. Values cross as:
- numbers, booleans, and strings;
- Luau vectors (`vector.create(x, y, z)`, with the `vector` library);
- quaternions;
- entity values.

An entity value holds a persistent ID and is checked again on every use.

| `maya` | |
| --- | --- |
| `log(...)` | Writes a line to the editor's Diagnostics or the player's output. |
| `tick()`, `time()`, `delta()` | The tick index, simulation seconds, and fixed interval. |
| `find("6d617961 300")` | The entity with that ID, or nil. |
| `create(name, position?, parent?)` | Queues a new entity with a name and a position, below `parent` when given. Returns its entity value, which exists from the next tick. |
| `input.down(key)`, `input.pressed(key)`, `input.released(key)` | This tick's keys: `A`–`Z`, `0`–`9`, `F1`–`F12`, `Space`, `Enter`, `Escape`, `Tab`, `Backspace`, `Left`, `Right`, `Up`, `Down`, `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, and `RightAlt`. |
| `input.look()`, `input.scroll()` | Pointer movement (a vector) and scrolling for this tick. |
| `quaternion.new(x, y, z, w)`, `quaternion.identity()`, `quaternion.from_axis_angle(axis, radians)` | Quaternions, which have fields `x`, `y`, `z`, `w`, the methods `rotate(vector)`, `normalized()`, and `inverse()`, and `*` to compose them. |

| Entity method | |
| --- | --- |
| `id()`, `name()`, `alive()`, `parent()`, `children()` | Identity and hierarchy. |
| `position()`, `rotation()`, `scale()` | The local transform. |
| `world_position()`, `world_rotation()` | The world pose. |
| `set_position(v)`, `set_rotation(q)`, `set_scale(v)` | Change the local transform. |
| `has(component)`, `get(component, property)`, `set(component, property, value)` | Any component's properties by name, such as `get("maya.light", "intensity")`; the `maya.` prefix is optional. Choices are names such as `"spot"`. Asset references and script values are not settable from scripts yet. |
| `destroy()` | Queues the entity's removal, with everything below it. |

Reads see the World as the previous tick committed it.

## What scripts may change

- **Commands.** Every change is a command, visible from the next tick. Edits to one component in a tick are merged, then applied as one replacement.
- **Validation.** A property edit is validated like an Inspector edit: an intensity of −1 is an error at that line.
- **Physics bodies.** A script may not set the transform of a kinematic or dynamic body ([physics](physics.md#requests-during-a-tick)).
- **`update` hooks** change nothing, because between-frame changes would depend on the frame rate. A write there is an error.
- **Conflicts.** When two instances write the same property in a tick, the later one wins, and a warning names both.
- **Created entities** get IDs from a sequence seeded by the session, so a replayed session creates the same entities.

## Sandbox and limits

- **Libraries.** Scripts see the read-only Luau libraries `string`, `table`, `math`, `bit32`, `utf8`, `buffer`, `vector`, and `coroutine`, plus `maya`.
- **Removed.** `print`, `debug`, `os`, `getfenv`, and `setfenv` are removed. Luau has no file, process, environment, or code-loading functions of its own.
- **Globals.** Each script has its own globals; writing a library table is an error.
- **Work budget.** Each hook call gets a work budget counted at Luau safepoints (loop back edges and calls), never in wall time, so the same script and inputs always pass or always fail.
- **Memory.** The play session's VM has a memory limit. Loading a script and setting up its instances count against it too: a script that cannot be set up is that script's error, like one that runs out while running.
- **Coroutines.** They may run within a call; a hook that yields is an error.

`ScriptSettings` sets the limits and the seed:

| Setting | Default |
| --- | --- |
| `limits.work_per_call` | 1,000,000 safepoints |
| `limits.memory_bytes` | 64 MiB per play session |
| `seed` | `0x6d617961`; seeds `math.random` and created entity IDs |

## Errors

A syntax, runtime, budget, or memory error names the entity, the script, the line, and a short traceback:
`Faulty (scripts/faulty.luau): scripts/faulty.luau:8: attempt to call a nil value …`.

- **What stops.** The failing call's commands and edits are discarded, and that instance gets no more hooks except `stop`. Every other script, native system, and physics keeps running.
- **Where it shows.**
  - The editor adds it to Diagnostics under **script** and shows a notice.
  - The player prints it.
  - Logs from `maya.log` go to the same places, without a notice.
- **Scripts that cannot compile.** A script that cannot be read or compiled is reported once, and its instances do not start.

Play sessions report these through `PlayFrame::messages` (`SimulationMessage` with a level: info, warning, or error). Native systems keep their contract: a throwing native system still stops the session.

## Determinism

The same scene, inputs, and seed give the same result:
- scripts run in a stable order;
- work is counted, not timed;
- `math.random` and created IDs come from the seed;
- `os`, with its clock, is not available.

Luau's `pairs` order over keys that are tables, functions, or userdata depends on addresses, so scripts must not let it affect the simulation ([scheduling](architecture/scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick)).

## Using it from C++

```cpp
#include "maya/simulation/script_assets.hpp"

auto systems = maya::play_systems(maya::registry_script_sources(registry)); // built-ins, then scripts
auto started = maya::PlaySession::start(document, maya::asset_property_context(registry), std::move(systems));
const auto frame = started.session->update(wall_delta);
for (const auto& message : frame.messages) report(message.level, message.text);
```

- **Where Luau lives.** The scripting host is in MayaSimulation (`src/maya/simulation/scripting/`), the only place that includes Luau. The CTest check `maya_library_headers` enforces it along with Jolt's boundary.
- **Public API.** [scripting.hpp](../include/maya/simulation/scripting.hpp) has no Luau types.
- **Frame hook.** `SimulationSystem::frame(FrameContext&)` is the new once-per-frame hook; `TickContext::messages` and `FrameContext::messages` collect reports.

## Cost

Release on the M4 Pro reference machine (thermal state nominal), per tick with every instance running:

| Scripted entities | Empty `fixed_update` | Moving with `set_position` each tick |
| --- | --- | --- |
| 1,000 | 0.10 ms mean, 0.15 ms worst | 0.48 ms mean, 0.59 ms worst |
| 10,000 | 0.97 ms mean, 1.1 ms worst | 4.8 ms mean, 5.2 ms worst |

That is about 97 ns per call for an empty hook and 480 ns per moved instance, flat from 1,000 to 10,000. `spin.luau` needs fewer than 5 safepoints per call, and plays in a VM limited to 512 KiB, so the defaults leave a wide margin. A move costs a read, a validated property edit, and one transform write in the World batch per entity. These are observations, not budgets; `maya_scripting_tests "[cost]"` prints them.

## Tests

[scripting_tests.cpp](../tests/scripting_tests.cpp) (`maya_scripting_tests`, CPU) covers:
- **Declarations:** properties, defaults, ranges, units, labels, and hooks, and the errors of malformed ones.
- **Sandbox:**
  - no system access, code loading, or collector control;
  - read-only shared tables and separate globals;
  - no engine use while a script loads;
  - the load's work budget.
- **Lifecycle:**
  - `start`, `fixed_update`, and `stop` in activation order;
  - a script attached during play;
  - a destroyed entity's `stop`.
- **Values:** authored values reaching instances, and values that do not fit reported and replaced.
- **Changes:** moving entities, editing properties by name and choice, creating entities that exist from the next tick, and destroying them.
- **Errors:**
  - messages with script and line;
  - the failing call's move and creation discarded;
  - other scripts continuing;
  - compile errors reported once;
  - errors in `start`, `update`, and `stop`, with `stop` still following a failed `start`;
  - a failing native system still stopping the session.
- **Limits:** the work budget and the memory limit, each stopping only its script, including a script that arrives when the VM is full.
- **Refusals:** writes to physics bodies, and writes from `update`.
- **Conflicts** between two scripts.
- **Input** by key name.
- **Determinism:** random numbers and created IDs repeating from the seed.
- **Spin parity:** a spin script matching the built-in `maya.spin` for 600 ticks.
- **Stale handles:** held entity values checked on every use.

Also:
- [asset_tests.cpp](../tests/asset_tests.cpp): script catalog entries, loading, reloading, and missing files.
- [property_tests.cpp](../tests/property_tests.cpp): the component's schema and value validation.
- [scene_tests.cpp](../tests/scene_tests.cpp): value encoding and refused values.
- [world_tests.cpp](../tests/world_tests.cpp): dropping a batch's newest commands (`WorldCommands::truncate`).
- [editor_scripting_tests.cpp](../tests/editor_scripting_tests.cpp): the Inspector's script fields with undo, unused values, compile errors in the Inspector, and script logs and failures in Diagnostics while play continues.
- The editor–player parity tests now play the sample scenes with scripts.
