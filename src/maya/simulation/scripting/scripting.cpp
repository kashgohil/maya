// What scripts declare, read without running them (docs/scripting.md#exposed-properties).
#include "luau.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace maya {
namespace scripting {
namespace {

constexpr const char* hook_names[] = {"start", "fixed_update", "late_fixed_update", "update", "stop",
                                      "on_contact_begin", "on_contact_end", "on_trigger_enter", "on_trigger_exit"};

ScriptValueData default_for(ScriptValueType type) {
    switch (type) {
    case ScriptValueType::number: return 0.0f;
    case ScriptValueType::integer: return int32_t{0};
    case ScriptValueType::boolean: return false;
    case ScriptValueType::string: return std::string{};
    case ScriptValueType::vector: return math::Vec3(0.0f);
    case ScriptValueType::color: return math::Vec3(1.0f);
    case ScriptValueType::entity: break;
    }
    return EntityId{};
}

std::string type_list() {
    return "number, integer, boolean, string, vector, color, or entity";
}

bool numeric(ScriptValueType type) {
    return type == ScriptValueType::number || type == ScriptValueType::integer;
}

std::optional<float> as_number(const ScriptValueData& data) {
    if (const auto* f = std::get_if<float>(&data)) return *f;
    if (const auto* i = std::get_if<int32_t>(&data)) return float(*i);
    return std::nullopt;
}

bool identifier(std::string_view name) {
    return !name.empty() && name.size() <= 64 && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

} // namespace

ScriptDescription describe_module(lua_State* L, int index, std::string_view name) {
    auto description = ScriptDescription{};
    index = lua_absindex(L, index);
    for (const auto* hook : hook_names) {
        lua_rawgetfield(L, index, hook);
        if (lua_isfunction(L, -1)) description.hooks.emplace_back(hook);
        lua_pop(L, 1);
    }
    const auto fail = [&](std::string message) {
        description.error = std::string(name) + ": " + message;
        description.properties.clear();
        return description;
    };
    lua_rawgetfield(L, index, "properties");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return description;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return fail("'properties' must be a table of declarations");
    }
    const auto table = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        // key at -2, declaration at -1
        if (lua_type(L, -2) != LUA_TSTRING || !identifier(lua_tostring(L, -2))) {
            lua_settop(L, table - 1);
            return fail("property names are identifiers such as speed or jump_height");
        }
        auto declared = ScriptPropertyDeclaration{};
        declared.name = lua_tostring(L, -2);
        if (!lua_istable(L, -1)) {
            lua_settop(L, table - 1);
            return fail("property '" + declared.name + "' needs a table such as { type = \"number\", default = 1 }");
        }
        const auto entry = lua_gettop(L);
        lua_rawgetfield(L, entry, "type");
        const auto type = lua_type(L, -1) == LUA_TSTRING ? script_value_type(lua_tostring(L, -1)) : std::nullopt;
        lua_pop(L, 1);
        if (!type) {
            lua_settop(L, table - 1);
            return fail("property '" + declared.name + "' needs a type: " + type_list());
        }
        declared.type = *type;
        declared.default_value = default_for(*type);
        lua_rawgetfield(L, entry, "default");
        if (!lua_isnil(L, -1)) {
            const auto value = to_script_value(L, -1, *type);
            if (!value) {
                lua_settop(L, table - 1);
                return fail("property '" + declared.name + "' has a default that is not a " + script_value_type_name(*type));
            }
            declared.default_value = *value;
        }
        lua_pop(L, 1);
        for (auto [field, bound] : {std::pair{"min", &declared.minimum}, std::pair{"max", &declared.maximum}}) {
            lua_rawgetfield(L, entry, field);
            if (!lua_isnil(L, -1)) {
                if (lua_type(L, -1) != LUA_TNUMBER || !numeric(*type)) {
                    lua_settop(L, table - 1);
                    return fail("property '" + declared.name + "': '" + field + "' is a number, for number and integer properties");
                }
                *bound = float(lua_tonumber(L, -1));
            }
            lua_pop(L, 1);
        }
        for (auto [field, text] : {std::pair{"unit", &declared.unit}, std::pair{"label", &declared.label}}) {
            lua_rawgetfield(L, entry, field);
            if (lua_type(L, -1) == LUA_TSTRING) *text = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        if (const auto number = as_number(declared.default_value);
            number && ((declared.minimum && *number < *declared.minimum) || (declared.maximum && *number > *declared.maximum))) {
            lua_settop(L, table - 1);
            return fail("property '" + declared.name + "' has a default outside its range");
        }
        description.properties.push_back(std::move(declared));
        lua_pop(L, 1); // the declaration; the key stays for lua_next
    }
    lua_pop(L, 1); // properties
    // Lua tables have no order; properties are listed by name.
    std::ranges::sort(description.properties, {}, &ScriptPropertyDeclaration::name);
    return description;
}

} // namespace scripting

namespace scripting {
ScriptDescription describe_bytecode(std::string_view name, const std::string& bytecode, ScriptLimits limits) {
    try {
        auto vm = Vm(limits, 0);
        auto loaded = vm.load_bytecode(name, bytecode);
        if (!loaded.error.empty()) return {{}, {}, loaded.error};
        auto* T = loaded.thread;
        lua_getref(T, loaded.module_ref);
        auto description = describe_module(T, -1, name);
        lua_pop(T, 1);
        vm.release(loaded);
        return description;
    } catch (const std::exception& error) {
        return {{}, {}, std::string(name) + ": " + error.what()};
    }
}
} // namespace scripting

ScriptDescription describe_script(std::string_view name, std::string_view source, ScriptLimits limits) {
    return scripting::describe_bytecode(name, scripting::compile(source), limits);
}

std::string ScriptReloads::offer(AssetId script, ScriptSource source) {
    // Compiled and described here, off the tick; the session only loads the bytecode.
    auto bytecode = scripting::compile(source.text);
    if (const auto description = scripting::describe_bytecode(source.name, bytecode, m_limits); !description)
        return description.error;
    // A newer offer for the same script replaces one the session has not taken yet.
    std::erase_if(m_offered, [&](const Version& version) { return version.script == script; });
    m_offered.push_back({script, std::move(source.name), std::move(bytecode)});
    return {};
}

std::vector<std::string> script_value_problems(const ScriptDescription& description, const std::vector<ScriptValue>& values) {
    auto problems = std::vector<std::string>{};
    for (const auto& value : values) {
        const auto declared = std::ranges::find(description.properties, value.name, &ScriptPropertyDeclaration::name);
        if (declared == description.properties.end()) {
            problems.push_back("'" + value.name + "' is not a property of the script; its value is kept but unused");
            continue;
        }
        if (declared->type != value.type) {
            problems.push_back("'" + value.name + "' holds a " + script_value_type_name(value.type) + " but the script declares a " +
                               script_value_type_name(declared->type) + "; the default is used");
            continue;
        }
        const auto number = scripting::as_number(value.data);
        if (number && ((declared->minimum && *number < *declared->minimum) || (declared->maximum && *number > *declared->maximum))) {
            auto range = std::ostringstream{};
            range << (declared->minimum ? *declared->minimum : -INFINITY) << " to " << (declared->maximum ? *declared->maximum : INFINITY);
            problems.push_back("'" + value.name + "' is outside its range (" + range.str() + "); the default is used");
        }
    }
    return problems;
}

std::unique_ptr<SimulationSystem> script_system(ScriptSources sources, ScriptSettings settings) {
    return scripting::make_script_system(std::move(sources), settings);
}

std::vector<std::unique_ptr<SimulationSystem>> play_systems(ScriptSources sources, ScriptSettings settings) {
    auto systems = builtin_systems();
    systems.push_back(script_system(std::move(sources), settings));
    return systems;
}

} // namespace maya
