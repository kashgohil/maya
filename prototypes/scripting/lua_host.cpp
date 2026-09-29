#include "script_host.hpp"

// Lua 5.4 is compiled as C++ (see CMakeLists.txt), so its headers are included without extern "C".
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <cstdlib>
#include <cstring>

namespace maya::prototype {
namespace {

constexpr int count_hook_interval = 1000; // VM instructions between count hooks

class LuaHost final : public ScriptHost {
public:
    explicit LuaHost(ScriptLimits limits) : m_limits(limits) {
        m_state = lua_newstate(&LuaHost::allocate, this);
        *static_cast<LuaHost**>(lua_getextraspace(m_state)) = this;
        build_sandbox();
        lua_sethook(m_state, &LuaHost::count_hook, LUA_MASKCOUNT, count_hook_interval);
    }
    ~LuaHost() override { lua_close(m_state); }

    std::string_view language() const override { return "Lua 5.4"; }

    std::string load(std::string_view name, std::string_view source) override {
        auto* L = m_state;
        release_environment();
        const auto chunk = "=" + std::string(name);
        // Text only: precompiled chunks are refused, since malformed bytecode can crash the VM.
        if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK)
            return pop_error();
        // A fresh environment per script. Reads fall through to the shared, read-only sandbox;
        // writes (the script's globals) stay in the environment.
        lua_newtable(L);
        lua_newtable(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_sandbox);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        m_environment = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_setupvalue(L, -2, 1); // the chunk's _ENV
        return protected_call(0);
    }

    std::string call(std::string_view function, double argument) override {
        auto* L = m_state;
        if (m_environment == LUA_NOREF)
            return "no script is loaded";
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_environment);
        lua_getfield(L, -1, std::string(function).c_str());
        lua_remove(L, -2);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return "'" + std::string(function) + "' is not a function";
        }
        lua_pushnumber(L, argument);
        return protected_call(1);
    }

    size_t memory_in_use() const override { return m_bytes; }

private:
    static void* allocate(void* user, void* block, size_t old_size, size_t new_size) {
        auto& host = *static_cast<LuaHost*>(user);
        if (!block)
            old_size = 0; // for a new block Lua passes the object type here, not a size
        if (new_size == 0) {
            std::free(block);
            host.m_bytes -= old_size;
            return nullptr;
        }
        if (new_size > old_size && host.m_bytes - old_size + new_size > host.m_limits.memory_bytes)
            return nullptr; // Lua raises a memory error in the script
        auto* moved = std::realloc(block, new_size);
        if (moved)
            host.m_bytes = host.m_bytes - old_size + new_size;
        return moved;
    }

    static void count_hook(lua_State* L, lua_Debug*) {
        auto& host = **static_cast<LuaHost**>(lua_getextraspace(L));
        host.m_work += count_hook_interval;
        if (host.m_work > host.m_limits.work_per_call)
            luaL_error(L, "work budget exceeded (%d instructions)", int(host.m_limits.work_per_call));
    }

    static LuaHost& host_of(lua_State* L) { return **static_cast<LuaHost**>(lua_getextraspace(L)); }

    static int add_force(lua_State* L) {
        host_of(L).forces.push_back({luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)});
        return 0;
    }

    static int write_log(lua_State* L) {
        host_of(L).log.emplace_back(luaL_checkstring(L, 1));
        return 0;
    }

    static int refuse_write(lua_State* L) { return luaL_error(L, "the sandbox is read-only"); }

    // A table whose reads go to `index` and whose writes fail. getmetatable returns false.
    void push_read_only(int index) {
        auto* L = m_state;
        index = lua_absindex(L, index);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushvalue(L, index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, &LuaHost::refuse_write);
        lua_setfield(L, -2, "__newindex");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_setmetatable(L, -2);
    }

    // Lua has no sandbox of its own: open the safe libraries, then copy what scripts may use into
    // a read-only table. io, os, package/require, debug, load/loadfile/dofile, collectgarbage, and
    // rawset are left out. The shared string metatable is hidden from getmetatable.
    void build_sandbox() {
        auto* L = m_state;
        const luaL_Reg libraries[] = {{LUA_GNAME, luaopen_base},       {LUA_TABLIBNAME, luaopen_table},
                                      {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                                      {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}};
        for (const auto& library : libraries) {
            luaL_requiref(L, library.name, library.func, 1);
            lua_pop(L, 1);
        }
        lua_pushliteral(L, "");
        lua_getmetatable(L, -1);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 2);

        lua_newtable(L); // the sandbox's contents
        lua_pushglobaltable(L);
        for (const auto* name : {"assert", "error", "ipairs", "next", "pairs", "pcall", "select", "tonumber", "tostring",
                                 "type", "xpcall", "rawequal", "rawget", "rawlen", "getmetatable", "setmetatable",
                                 "_VERSION"}) {
            lua_getfield(L, -1, name);
            lua_setfield(L, -3, name);
        }
        for (const auto* name : {LUA_TABLIBNAME, LUA_STRLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, LUA_COLIBNAME}) {
            lua_getfield(L, -1, name);
            push_read_only(-1);
            lua_setfield(L, -4, name);
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // the global table
        const luaL_Reg maya[] = {{"add_force", &LuaHost::add_force}, {"log", &LuaHost::write_log}, {nullptr, nullptr}};
        luaL_newlib(L, maya);
        push_read_only(-1);
        lua_setfield(L, -3, "maya");
        lua_pop(L, 1);
        push_read_only(-1);
        m_sandbox = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
    }

    void release_environment() {
        luaL_unref(m_state, LUA_REGISTRYINDEX, m_environment);
        m_environment = LUA_NOREF;
        lua_gc(m_state, LUA_GCCOLLECT);
    }

    std::string protected_call(int arguments) {
        m_work = 0;
        if (lua_pcall(m_state, arguments, 0, 0) != LUA_OK)
            return pop_error();
        return {};
    }

    std::string pop_error() {
        const char* message = lua_tostring(m_state, -1);
        auto error = std::string(message ? message : "error object is not a string");
        lua_pop(m_state, 1);
        return error;
    }

    ScriptLimits m_limits;
    lua_State* m_state = nullptr;
    size_t m_bytes = 0;
    uint64_t m_work = 0;
    int m_sandbox = LUA_NOREF;
    int m_environment = LUA_NOREF;
};

} // namespace

std::unique_ptr<ScriptHost> make_script_host(ScriptLimits limits) {
    return std::make_unique<LuaHost>(limits);
}

} // namespace maya::prototype
