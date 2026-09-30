#pragma once

// The scripting host's private view of Luau. Only src/maya/simulation/scripting/ includes Luau
// (checked by maya_library_headers); nothing here reaches a public header.
#include "maya/properties/schema.hpp"
#include "maya/simulation/scripting.hpp"

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace maya::scripting {

// Userdata tags; each has its metatable registered in the VM.
constexpr int entity_tag = 1;
constexpr int quaternion_tag = 2;

struct EntityBox {
    EntityId id;
};
struct QuaternionBox {
    float x, y, z, w;
};

/// What the hook being called may do.
enum class CallMode {
    declare, // running a script's top level: no engine access
    start, // an instance's start hook, at the tick boundary
    fixed, // fixed_update, before physics
    update, // once per frame: read-only
    stop, // an instance's stop hook
};

/// The engine services a hook call reaches through the `maya` table and entity methods. The script
/// system implements it; each call sets what applies. Methods throw ScriptError with a message.
class ScriptApi {
public:
    virtual ~ScriptApi() = default;
    virtual CallMode mode() const = 0;
    virtual const World& world() const = 0;
    virtual const PhysicsWorld* physics() const = 0;
    virtual const InputFrame* input() const = 0; // fixed-tick hooks only
    virtual uint64_t tick() const = 0;
    virtual double time() const = 0;
    virtual float delta() const = 0;
    virtual void log(std::string text) = 0;
    /// Queues a property edit, validated like an Inspector edit. Visible from the next tick.
    virtual void edit(EntityId entity, ComponentId component, PropertyId property, PropertyValue value) = 0;
    /// Queues a new entity with a name and position, below `parent` when given. Returns its ID.
    virtual EntityId create(std::string name, math::Vec3 position, std::optional<EntityId> parent) = 0;
    virtual void destroy(EntityId entity) = 0;
};

class ScriptError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Luau bytecode for `source`. A syntax error is encoded in it, and reported when it is loaded.
std::string compile(std::string_view source);

/// A Luau VM with Maya's sandbox, limits, and `maya` library. Single thread.
class Vm {
public:
    Vm(ScriptLimits limits, uint64_t seed);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    lua_State* state() const noexcept { return m_state; }
    void set_api(ScriptApi* api) noexcept { m_api = api; }
    ScriptApi* api() const noexcept { return m_api; }
    size_t memory() const noexcept { return m_bytes; }

    /// Compiles `source` and runs it in a new sandboxed thread. On success the returned module (the
    /// chunk's result, which must be a table) is left referenced in `module_ref`.
    struct Loaded {
        int thread_ref = LUA_NOREF;
        lua_State* thread = nullptr;
        int module_ref = LUA_NOREF;
        std::string error;
    };
    Loaded load(std::string_view name, std::string_view source) { return load_bytecode(name, compile(source)); }
    /// Loads what compile() made; a compile error is reported here, as "name:line: message".
    Loaded load_bytecode(std::string_view name, const std::string& bytecode);
    void release(const Loaded& loaded);

    /// Calls the function below `arguments` on `thread`'s stack, within the work budget, replacing it
    /// and its arguments with `results` values. Returns the error ("name:line: message" and a short
    /// traceback), or empty.
    std::string call(lua_State* thread, int arguments, int results = 0);
    /// Runs `work` on `thread` as a protected call, so a Luau error inside it (running out of memory,
    /// above all) is returned as its message instead of escaping to the session. What `work` leaves on
    /// the stack is discarded; keep values with references.
    template<class F> std::string protect(lua_State* thread, F&& work) {
        auto task = Task{[](lua_State* L, void* data) { (*static_cast<std::remove_reference_t<F>*>(data))(L); },
                         const_cast<void*>(static_cast<const void*>(&work))};
        return run_protected(thread, task);
    }

    static Vm& of(lua_State* L) { return *static_cast<Vm*>(lua_callbacks(L)->userdata); }

private:
    struct Task {
        void (*run)(lua_State*, void*);
        void* data;
    };
    std::string run_protected(lua_State* thread, Task& task);
    static void* allocate(void* user, void* block, size_t old_size, size_t new_size);
    static void interrupt(lua_State* L, int gc);

    ScriptLimits m_limits;
    lua_State* m_state = nullptr;
    ScriptApi* m_api = nullptr;
    size_t m_bytes = 0;
    uint64_t m_work = 0;
    int m_runner = LUA_NOREF; // the C function protect runs tasks in
};

/// What a script declares, from its bytecode, run in a fresh VM within `limits` (scripting.cpp).
ScriptDescription describe_bytecode(std::string_view name, const std::string& bytecode, ScriptLimits limits);

/// Reads `properties` and the defined hooks from a script's module table at `index` (scripting.cpp).
ScriptDescription describe_module(lua_State* L, int index, std::string_view name);
/// The script system (script_host.cpp).
std::unique_ptr<SimulationSystem> make_script_system(ScriptSources sources, ScriptSettings settings);

/// Registers the `maya` table and the entity and quaternion metatables in a fresh VM (script_api.cpp).
void open_maya_library(lua_State* L);

void push_entity(lua_State* L, EntityId id);
void push_quaternion(lua_State* L, const math::Quat& value);
/// A script value (a property value from a maya.script component) as a Lua value.
void push_script_value(lua_State* L, const ScriptValue& value);
/// The Lua value at `index` as data of `type`, or nullopt when it has another type.
std::optional<ScriptValueData> to_script_value(lua_State* L, int index, ScriptValueType type);

} // namespace maya::scripting
