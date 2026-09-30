# Physics and scripting libraries

Decided 30 September 2026 for [ISSUE-1015](https://work.rezee.app/kash/issues/1015), the first issue of the [physics and behavior milestone](https://work.rezee.app/kash/issues/1014), under [DOC-58](https://work.rezee.app/kash/docs/58).

| Decision | Choice | Instead of |
| --- | --- | --- |
| Physics library | **Jolt Physics 5.6.0**, confirmed by the prototype | PhysX 5, the fallback; not needed |
| Scripting language | **Luau 0.740**, chosen by the project owner from the prototype results | Lua 5.4.9 |

The contracts built on these choices are in [runtime and world](runtime-world-contracts.md#physics-boundary) (the physics and scripting boundaries) and [scheduling](scheduling-contracts.md#physics-and-behavior-in-the-fixed-tick) (hooks, the fixed-tick phases, reset, replay, and reload). The physics stress workload is [P1](performance-baseline.md#p1-physics-stress). Nothing here is engine code yet: `MayaPhysics` ([#1017](https://work.rezee.app/kash/issues/1017)) and the scripting host ([#1018](https://work.rezee.app/kash/issues/1018)) implement these decisions.

## The prototypes

Configure with `-DMAYA_BUILD_PROTOTYPES=ON`. The option is off by default, so a normal build neither downloads nor compiles these libraries until #1017 and #1018 adopt them.

| Target | What it is |
| --- | --- |
| `maya_physics_prototype` (`MayaPrototypePhysics`, `prototypes/physics`; replaced by [MayaPhysics](../physics.md) in #1017) | A falling-box scene in a Jolt PhysicsSystem: a static floor, 1 m boxes, collision groups and masks, contact callbacks, allocator hooks, and state save and restore. It printed timings, heap use, and trace hashes. |
| `maya_lua_prototype`, `maya_luau_prototype` (`MayaPrototypeLuaHost`, `MayaPrototypeLuauHost`, [prototypes/scripting](../../prototypes/scripting/script_host.hpp)) | The same sandboxed host in each language. A `maya` table offers `add_force(x, y, z)` and `log(message)`. The host has a memory limit and a deterministic work budget per call. It prints call cost, loop cost, and memory. Lua and Luau export the same C API names, so they are never linked into one program. |
| `maya_prototype_physics_tests`, `maya_prototype_lua_tests`, `maya_prototype_luau_tests` | The behavior the decisions rely on, in CTest under the `prototype` label. One scripting test file runs against both languages. |

#1017 replaced the physics prototype with [MayaPhysics](../physics.md), whose tests carry its checks over, and made Jolt part of every build. The scripting prototypes stay until #1018, which removes the Lua host and the Lua 5.4 dependency.

## Jolt Physics

| Topic | Decision and evidence |
| --- | --- |
| License | MIT. Packaged games ship its copyright and permission notice, as they will for Dear ImGui and Luau. |
| Version and pinning | Tag `v5.6.0` (commit `e77f1755`), fetched shallowly with FetchContent from `Build/`. Jolt's own targets stay out of `all`; only the Jolt library builds. An upgrade is its own change and reruns the physics tests and P1. |
| Build options | Single precision (`DOUBLE_PRECISION` off, since world extent is still [open](README.md#open-product-decisions)). `OBJECT_LAYER_BITS` 32, giving 16 collision groups and 16-bit masks. RTTI is on, because UndefinedBehaviorSanitizer's vptr check needs Jolt's type information. Exceptions stay off. No object stream, GPU compute backends, profiler, install rules, or link-time optimization. The debug renderer remains available in Debug and Release for #1022. `USE_ASSERTS` is off by default; the prototype tests also pass with it on. |
| Warnings | Jolt compiles with its own `-Wall -Werror` in its own directory scope. Maya adds its headers as system headers, so Maya's `-Wall -Wextra -Wpedantic` applies to Maya code only, and that code builds without warnings. |
| Process-wide state | Allocator hooks, trace and assert hooks, the `Factory`, and type registration are global. They are installed once, before any Jolt object exists, and kept for the life of the process. A world's own memory is released with its `PhysicsSystem`: after the runs, 6,256 bytes remain, all of it the type registry. |
| Memory hooks | `JPH::Allocate`, `Reallocate`, `Free`, `AlignedAllocate`, and `AlignedFree` are process-wide function pointers. Free passes no size, so the prototype counts bytes with `malloc_size`. Each world also has a `TempAllocatorImpl` for per-step scratch memory, 16 MiB in the prototype. The hooks give process totals; per-world accounting comes from the adapter (temporary allocator, body and pair limits). |
| Threading | `PhysicsSystem::Update` runs on the calling thread and fans work out to a `JobSystem` (the prototype uses `JobSystemThreadPool`). Contact, activation, and step listeners run on worker threads in no fixed order. Jolt's `JobSystem` interface lets a later Maya job system replace the pool. |
| Determinism | Jolt is deterministic for the same binary, the same API calls in the same order, and the same inputs. The prototype checked this: 1,000 boxes over 300 ticks give identical poses on every tick with 0, 1, 3, 7, and 13 worker threads, and sorted contact starts match too. `SaveState`/`RestoreState` replays the following ticks exactly (334 bytes for one box). `CROSS_PLATFORM_DETERMINISTIC` (about 8% slower, per Jolt) stays off. Broad-phase query order and callback order are not deterministic; the adapter sorts them. |
| Embedding API | `BodyInterface` creates, moves, and pushes bodies. `BodyCreationSettings` carries motion type, shape, layer, and user data, and user data carries Maya's own index, never a pointer. Filtering uses mask-based groups (`ObjectLayerPairFilterMask`, `BroadPhaseLayerInterfaceMask`). Snapshots use `StateRecorderImpl`, and debug drawing goes through `DebugRenderer`. |
| Errors | Jolt does not throw. `Update` returns error flags, for example when a pair or contact cache is full. Maya code called by Jolt (listeners, filters) must not throw through Jolt frames. |

Measured on the M4 Pro reference machine (14 cores, thermal state nominal), Release, 1,000 1 m boxes dropped in 10 layers onto a floor, 300 ticks at 60 Hz:

| Worker threads | Mean step | Max step | Peak Jolt heap | Awake at the end |
| --- | --- | --- | --- | --- |
| 0 | 0.98 ms | 2.26 ms | 28.9 MiB | 900 |
| 1 | 0.51 ms | 0.97 ms | 28.9 MiB | 900 |
| 3 | 0.35 ms | 0.70 ms | 28.9 MiB | 900 |
| 7 | 0.31 ms | 0.46 ms | 28.9 MiB | 900 |
| 13 | 0.36 ms | 0.49 ms | 28.9 MiB | 900 |

The trace hash is `d513b239a20c23aa` in every row. The peak includes the 16 MiB temporary allocator. These are prototype observations, not a P1 baseline. The Jolt static library is 3.0 MB.

**PhysX 5** was the fallback if Jolt showed a blocking problem. It showed none, and PhysX's GPU paths do not apply to Metal. Revisit only if Jolt misses P1 on the reference hardware or lacks a feature a later milestone needs.

## Luau

| Topic | Decision and evidence |
| --- | --- |
| License | MIT (Roblox, with the Lua.org copyright for the Lua-derived parts); the notice ships with packaged games. |
| Version and pinning | Tag `0.740` (commit `c0e346ed`), shallow FetchContent. Luau releases weekly. Maya pins a tag and updates in its own change, with the scripting tests rerun. Bytecode is never stored across versions: scripts are shipped and cached as source and compiled by the host. |
| Libraries built | `Luau.VM` and `Luau.Compiler`, with `Luau.Ast`, `Luau.Common`, and `Luau.Bytecode`: about 2.5 MB of static libraries. The type checker (`Luau.Analysis`) and native code generation (`Luau.CodeGen`) are not built. The first could check scripts in the editor later; the second is an optimization to measure against real scripts first. |
| Embedding API | `lua_newstate` with Maya's allocator, then `luaL_openlibs`, then removing what scripts may not use, then `luaL_sandbox`. Each script runs in a thread made with `lua_newthread` and `luaL_sandboxthread`. The host compiles source with `Luau::compile` and loads it with `luau_load`. Calls use `lua_pcall`. `lua_callbacks` carries the host pointer and the `interrupt` that enforces the work budget. |
| Errors | Luau is built as C++ (`LUA_USE_LONGJMP` 0), so script errors unwind with exceptions and destructors in native functions run. Messages name the script and line: `faulty:3: attempt to call a nil value`. |
| Sandbox | `luaL_sandbox` makes the global table and every library read-only. `luaL_sandboxthread` gives each script its own globals, which fall through to the read-only ones. Luau has no file, process, environment, or bytecode-loading functions. The host also removes `print` and `debug`; the contract also removes `os`, `getfenv`, and `setfenv`. |
| Limits | Memory: the allocator refuses to grow past a limit, and the script gets "not enough memory". Work: the interrupt runs at loop back edges and calls, and it counts them. Counting work rather than measuring time makes the budget deterministic: a call either always fits or never does. |
| Threading | A VM belongs to one thread at a time. Maya runs it only on the world's owner thread. |
| Types | Luau accepts optional type annotations (`function f(v: number): number`); the compiler ignores them. Lua 5.4 rejects them as syntax errors. |

The same host and checks were built in each language (Release, M4 Pro, nominal). The costs include the work budget's hook or interrupt.

| | Lua 5.4.9 | Luau 0.740 |
| --- | --- | --- |
| Call to a script function that makes one native call | 69–80 ns | 45–50 ns |
| 1,000,000-iteration arithmetic loop | 37–39 ms | 6.4 ms |
| VM memory with a small script loaded | 17 KB | 345 KB |
| Static libraries | 0.45 MB | 2.5 MB |
| Sandbox | Written by the host: read-only proxy tables, a hidden string metatable, and an allow-list of functions | `luaL_sandbox` and per-script threads |
| Work budget | Count hook every 1,000 VM instructions | Interrupt at safepoints |
| Syntax error message | `broken:3: ')' expected near <eof>` | `broken:3: Expected ')' (to close '(' at line 2), got <eof>` |

Both hosts pass the same checks: native calls, error recovery, the sandbox, read-only shared tables, separate globals, the work budget, and the memory limit.

**Why Luau.** It is about six times faster on script arithmetic without native code generation. Its sandbox is built in rather than hand-assembled, it accepts optional types, and it has a deterministic work limit. Its costs are a larger VM (0.35 MB per VM, and Maya uses one per play session), a faster-moving release line that must be pinned deliberately, and slightly less detail in some runtime errors. Lua 5.4 is smaller and steadier. Its hand-built sandbox is the kind of code that fails quietly, and Lua 5.4 also randomizes its string hash seed. Lua 5.5 (5.5.1, July 2026) is now the current PUC-Lua series. It was not prototyped; its embedding API is close to 5.4's and does not change the comparison.

## Validation

Validated on 30 September 2026:
- **Builds.** Release, and Debug with `MAYA_SANITIZERS=undefined`: both build with no warnings in Maya code.
- **Offline and clean export.** Configured with `FETCHCONTENT_FULLY_DISCONNECTED=ON` from cached sources, and from a clean export of the source tree: both build and pass all 47 CTest tests.
- **Prototype tests.** `maya_prototype_physics` (5 cases, 26 assertions), `maya_prototype_lua` (7 cases, 114 assertions), and `maya_prototype_luau` (7 cases, 116 assertions) pass, also under UBSan. The physics tests also pass with Jolt's asserts on. (#1017 replaced the physics prototype tests with `maya_physics`.)
- **What this does not show.** It is not a P1 baseline, and not evidence for the engine's physics or scripting, which do not exist yet. Jolt and Luau are not themselves sanitized; only Maya code is.
