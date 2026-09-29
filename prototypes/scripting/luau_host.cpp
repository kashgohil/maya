#include "script_host.hpp"

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <cstdlib>

namespace maya::prototype {
namespace {

class LuauHost final : public ScriptHost {
public:
    explicit LuauHost(ScriptLimits limits) : m_limits(limits) {
        m_state = lua_newstate(&LuauHost::allocate, this);
        auto* callbacks = lua_callbacks(m_state);
        callbacks->userdata = this;
        callbacks->interrupt = &LuauHost::interrupt;
        // Luau's libraries have no file, process, environment, or bytecode-loading functions, and
        // its os library only reads the clock and dates. print writes to stdout and debug inspects
        // other functions' stacks, so both are removed; errors come back to the host instead.
        luaL_openlibs(m_state);
        for (const auto* name : {"print", "debug"}) {
            lua_pushnil(m_state);
            lua_setglobal(m_state, name);
        }
        const luaL_Reg maya[] = {{"add_force", &LuauHost::add_force}, {"log", &LuauHost::write_log}, {nullptr, nullptr}};
        luaL_register(m_state, "maya", maya);
        lua_pop(m_state, 1);
        // Makes the global table and every library table read-only.
        luaL_sandbox(m_state);
    }
    ~LuauHost() override { lua_close(m_state); }

    std::string_view language() const override { return "Luau"; }

    std::string load(std::string_view name, std::string_view source) override {
        release_thread();
        // Each script runs in its own thread whose globals fall through to the read-only sandbox.
        m_thread = lua_newthread(m_state);
        luaL_sandboxthread(m_thread);
        m_thread_ref = lua_ref(m_state, -1);
        lua_pop(m_state, 1);
        // Source only: the host compiles it here, and scripts cannot load code themselves.
        const auto bytecode = Luau::compile(std::string(source));
        const auto chunk = "=" + std::string(name);
        if (luau_load(m_thread, chunk.c_str(), bytecode.data(), bytecode.size(), 0) != 0)
            return pop_error();
        return protected_call(0);
    }

    std::string call(std::string_view function, double argument) override {
        if (!m_thread)
            return "no script is loaded";
        lua_getglobal(m_thread, std::string(function).c_str());
        if (!lua_isfunction(m_thread, -1)) {
            lua_pop(m_thread, 1);
            return "'" + std::string(function) + "' is not a function";
        }
        lua_pushnumber(m_thread, argument);
        return protected_call(1);
    }

    size_t memory_in_use() const override { return m_bytes; }

private:
    static LuauHost& host_of(lua_State* L) { return *static_cast<LuauHost*>(lua_callbacks(L)->userdata); }

    static void* allocate(void* user, void* block, size_t old_size, size_t new_size) {
        auto& host = *static_cast<LuauHost*>(user);
        if (!block)
            old_size = 0;
        if (new_size == 0) {
            std::free(block);
            host.m_bytes -= old_size;
            return nullptr;
        }
        if (new_size > old_size && host.m_bytes - old_size + new_size > host.m_limits.memory_bytes)
            return nullptr; // Luau raises a memory error in the script
        auto* moved = std::realloc(block, new_size);
        if (moved)
            host.m_bytes = host.m_bytes - old_size + new_size;
        return moved;
    }

    // Called at loop back edges and calls (gc < 0), and for garbage collection steps (gc >= 0).
    static void interrupt(lua_State* L, int gc) {
        if (gc >= 0)
            return;
        auto& host = host_of(L);
        if (++host.m_work > host.m_limits.work_per_call)
            luaL_error(L, "work budget exceeded (%d safepoints)", int(host.m_limits.work_per_call));
    }

    static int add_force(lua_State* L) {
        host_of(L).forces.push_back({luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)});
        return 0;
    }

    static int write_log(lua_State* L) {
        host_of(L).log.emplace_back(luaL_checkstring(L, 1));
        return 0;
    }

    void release_thread() {
        if (m_thread_ref != LUA_NOREF)
            lua_unref(m_state, m_thread_ref);
        m_thread_ref = LUA_NOREF;
        m_thread = nullptr;
        lua_gc(m_state, LUA_GCCOLLECT, 0);
    }

    std::string protected_call(int arguments) {
        m_work = 0;
        if (lua_pcall(m_thread, arguments, 0, 0) != LUA_OK)
            return pop_error();
        return {};
    }

    std::string pop_error() {
        const char* message = lua_tostring(m_thread, -1);
        auto error = std::string(message ? message : "error object is not a string");
        lua_pop(m_thread, 1);
        return error;
    }

    ScriptLimits m_limits;
    lua_State* m_state = nullptr;
    lua_State* m_thread = nullptr; // the loaded script's thread, kept alive by m_thread_ref
    int m_thread_ref = LUA_NOREF;
    size_t m_bytes = 0;
    uint64_t m_work = 0;
};

} // namespace

std::unique_ptr<ScriptHost> make_script_host(ScriptLimits limits) {
    return std::make_unique<LuauHost>(limits);
}

} // namespace maya::prototype
