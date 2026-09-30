// The Luau VM with Maya's sandbox and limits, and the `maya` library scripts reach the engine through
// (docs/scripting.md#the-engine-api).
#include "luau.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace maya::scripting {
namespace {

// --- Errors ----------------------------------------------------------------------------------------

/// Runs a binding. Maya's own errors (ScriptError, and invalid_argument and runtime_error from the
/// engine) become script errors at the calling line; Luau's own errors pass through untouched.
template<int (*F)(lua_State*, ScriptApi&)>
int guarded(lua_State* L) {
    auto message = std::string{};
    try {
        auto* api = Vm::of(L).api();
        if (!api || api->mode() == CallMode::declare)
            throw ScriptError("the engine is not available while a script loads; use it in hooks");
        return F(L, *api);
    } catch (const ScriptError& error) {
        message = error.what();
    } catch (const std::invalid_argument& error) {
        message = error.what();
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    luaL_error(L, "%s", message.c_str());
    return 0; // not reached: luaL_error throws
}

/// A binding that may run while a script loads (pure math).
template<int (*F)(lua_State*)>
int pure(lua_State* L) {
    return F(L);
}

void require_writable(const ScriptApi& api, const char* what) {
    if (api.mode() == CallMode::update)
        throw ScriptError(std::string("update hooks cannot ") + what + "; make changes in fixed_update");
}

// --- Values ----------------------------------------------------------------------------------------

std::string id_text(EntityId id) {
    auto text = std::ostringstream{};
    text << std::hex << id.high << ' ' << id.low;
    return text.str();
}

std::optional<EntityId> parse_id(std::string_view text) {
    auto input = std::istringstream(std::string(text));
    auto id = EntityId{};
    if (!(input >> std::hex >> id.high >> id.low) || !id.valid()) return std::nullopt;
    auto rest = std::string{};
    if (input >> rest) return std::nullopt;
    return id;
}

math::Vec3 check_vector(lua_State* L, int index) {
    const auto* v = luaL_checkvector(L, index);
    return {v[0], v[1], v[2]};
}

void push_vector(lua_State* L, const math::Vec3& v) {
    lua_pushvector(L, v.x, v.y, v.z);
}

QuaternionBox* to_quaternion(lua_State* L, int index) {
    return static_cast<QuaternionBox*>(lua_touserdatatagged(L, index, quaternion_tag));
}

math::Quat check_quaternion(lua_State* L, int index) {
    const auto* q = static_cast<QuaternionBox*>(luaL_checkudatatagged(L, index, quaternion_tag));
    return {q->x, q->y, q->z, q->w};
}

EntityId check_entity(lua_State* L, int index) {
    return static_cast<EntityBox*>(luaL_checkudatatagged(L, index, entity_tag))->id;
}

/// The entity's live handle; scripts hold persistent IDs and every use resolves them again.
EntityHandle live(const ScriptApi& api, EntityId id) {
    const auto handle = api.world().find(id);
    if (!handle) throw ScriptError("entity " + id_text(id) + " does not exist (yet, or any more)");
    return *handle;
}

const ComponentDescriptor& check_component(std::string_view name) {
    auto* schema = component_schema(name);
    if (!schema && !name.starts_with("maya.")) schema = component_schema("maya." + std::string(name));
    if (!schema) throw ScriptError("unknown component '" + std::string(name) + "'");
    return *schema;
}

const PropertyDescriptor& check_property(const ComponentDescriptor& schema, std::string_view name) {
    const auto* property = property_schema(schema.id, name);
    if (!property) throw ScriptError(std::string(schema.name) + " has no property '" + std::string(name) + "'");
    return *property;
}

void push_property(lua_State* L, const PropertyDescriptor& property, const PropertyValue& value) {
    std::visit([&]<class T>(const T& typed) {
        if constexpr (std::same_as<T, std::string>) lua_pushlstring(L, typed.data(), typed.size());
        else if constexpr (std::same_as<T, bool>) lua_pushboolean(L, typed);
        else if constexpr (std::same_as<T, float>) lua_pushnumber(L, typed);
        else if constexpr (std::same_as<T, math::Vec3>) push_vector(L, typed);
        else if constexpr (std::same_as<T, math::Quat>) push_quaternion(L, typed);
        else if constexpr (std::same_as<T, ChoiceValue>) {
            for (const auto& option : property.choices)
                if (option.value == typed.value) {
                    lua_pushlstring(L, option.name.data(), option.name.size());
                    return;
                }
            lua_pushnil(L);
        } else if constexpr (std::same_as<T, int32_t>) lua_pushinteger(L, typed);
        else if constexpr (std::same_as<T, uint32_t>) lua_pushnumber(L, double(typed));
        else if constexpr (std::same_as<T, std::vector<ScriptValue>>)
            throw ScriptError("a script's property values are read through its instance, not maya.script");
        else {
            if (typed.valid()) {
                const auto text = id_text(EntityId{typed.id.high, typed.id.low});
                lua_pushlstring(L, text.data(), text.size());
            } else {
                lua_pushnil(L);
            }
        }
    }, value);
}

PropertyValue to_property(lua_State* L, int index, const PropertyDescriptor& property) {
    const auto wrong = [&](const char* expected) {
        return ScriptError(std::string(property.name) + " needs " + expected + ", not a " + luaL_typename(L, index));
    };
    switch (property.type) {
    case PropertyType::text:
        if (lua_type(L, index) != LUA_TSTRING) throw wrong("a string");
        return std::string(lua_tostring(L, index));
    case PropertyType::boolean:
        if (!lua_isboolean(L, index)) throw wrong("a boolean");
        return lua_toboolean(L, index) != 0;
    case PropertyType::scalar:
        if (lua_type(L, index) != LUA_TNUMBER) throw wrong("a number");
        return float(lua_tonumber(L, index));
    case PropertyType::vector3:
        if (!lua_isvector(L, index)) throw wrong("a vector");
        return check_vector(L, index);
    case PropertyType::quaternion: {
        const auto* q = to_quaternion(L, index);
        if (!q) throw wrong("a quaternion");
        return math::Quat{q->x, q->y, q->z, q->w};
    }
    case PropertyType::choice: {
        if (lua_type(L, index) != LUA_TSTRING) throw wrong("a choice name");
        const auto name = std::string_view(lua_tostring(L, index));
        for (const auto& option : property.choices)
            if (option.name == name) return ChoiceValue{option.value};
        throw ScriptError(std::string(property.name) + " has no choice '" + std::string(name) + "'");
    }
    case PropertyType::integer:
    case PropertyType::flags: {
        if (lua_type(L, index) != LUA_TNUMBER) throw wrong("a whole number");
        const auto number = lua_tonumber(L, index);
        if (number != std::floor(number)) throw ScriptError(std::string(property.name) + " needs a whole number");
        if (property.type == PropertyType::integer) {
            if (number < INT32_MIN || number > INT32_MAX) throw ScriptError(std::string(property.name) + " is out of range");
            return int32_t(number);
        }
        if (number < 0 || number > UINT32_MAX) throw ScriptError(std::string(property.name) + " is out of range");
        return uint32_t(number);
    }
    case PropertyType::mesh_ref:
    case PropertyType::material_ref:
    case PropertyType::script_ref:
    case PropertyType::script_values: break;
    }
    throw ScriptError(std::string(property.name) + " cannot be set by scripts yet");
}

// --- The maya table --------------------------------------------------------------------------------

int maya_log(lua_State* L, ScriptApi& api) {
    auto text = std::string{};
    const auto count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        size_t length = 0;
        const auto* piece = luaL_tolstring(L, i, &length);
        if (i > 1) text += ' ';
        text.append(piece, length);
        lua_pop(L, 1);
    }
    api.log(std::move(text));
    return 0;
}

int maya_tick(lua_State* L, ScriptApi& api) {
    lua_pushnumber(L, double(api.tick()));
    return 1;
}

int maya_time(lua_State* L, ScriptApi& api) {
    lua_pushnumber(L, api.time());
    return 1;
}

int maya_delta(lua_State* L, ScriptApi& api) {
    lua_pushnumber(L, api.delta());
    return 1;
}

int maya_find(lua_State* L, ScriptApi& api) {
    const auto id = parse_id(luaL_checkstring(L, 1));
    if (!id) throw ScriptError("maya.find needs an entity ID such as \"6d617961 300\"");
    if (api.world().find(*id)) push_entity(L, *id);
    else lua_pushnil(L);
    return 1;
}

int maya_create(lua_State* L, ScriptApi& api) {
    require_writable(api, "create entities");
    const auto name = std::string(luaL_checkstring(L, 1));
    const auto position = lua_isnoneornil(L, 2) ? math::Vec3(0.0f) : check_vector(L, 2);
    const auto parent = lua_isnoneornil(L, 3) ? std::nullopt : std::optional(check_entity(L, 3));
    push_entity(L, api.create(name, position, parent));
    return 1;
}

// --- Input -----------------------------------------------------------------------------------------

struct KeyName {
    const char* name;
    KeyCode code;
};
constexpr auto named_keys = std::array{
    KeyName{"Space", KeyCode::Space}, KeyName{"Enter", KeyCode::Enter}, KeyName{"Escape", KeyCode::Escape},
    KeyName{"Tab", KeyCode::Tab}, KeyName{"Backspace", KeyCode::Backspace}, KeyName{"Left", KeyCode::Left},
    KeyName{"Right", KeyCode::Right}, KeyName{"Up", KeyCode::Up}, KeyName{"Down", KeyCode::Down},
    KeyName{"LeftShift", KeyCode::LeftShift}, KeyName{"RightShift", KeyCode::RightShift},
    KeyName{"LeftControl", KeyCode::LeftControl}, KeyName{"RightControl", KeyCode::RightControl},
    KeyName{"LeftAlt", KeyCode::LeftAlt}, KeyName{"RightAlt", KeyCode::RightAlt}};

KeyCode check_key(lua_State* L, int index) {
    const auto name = std::string_view(luaL_checkstring(L, index));
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return KeyCode(int(KeyCode::A) + (name[0] - 'A'));
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return KeyCode(int(KeyCode::Num0) + (name[0] - '0'));
    if (name.size() >= 2 && name[0] == 'F') {
        const auto number = std::atoi(std::string(name.substr(1)).c_str());
        if (number >= 1 && number <= 12 && name.substr(1) == std::to_string(number)) return KeyCode(int(KeyCode::F1) + number - 1);
    }
    for (const auto& key : named_keys)
        if (name == key.name) return key.code;
    throw ScriptError("unknown key '" + std::string(name) + "'; keys are named A-Z, 0-9, F1-F12, Space, Enter, Escape, Tab, "
                      "Backspace, Left, Right, Up, Down, LeftShift, RightShift, LeftControl, RightControl, LeftAlt, and RightAlt");
}

const InputFrame& input(const ScriptApi& api) {
    const auto* frame = api.input();
    if (!frame) throw ScriptError("input is read in fixed_update, where each tick has its own input");
    return *frame;
}

int input_down(lua_State* L, ScriptApi& api) {
    lua_pushboolean(L, input(api).down(check_key(L, 1)));
    return 1;
}
int input_pressed(lua_State* L, ScriptApi& api) {
    lua_pushboolean(L, input(api).went_down(check_key(L, 1)));
    return 1;
}
int input_released(lua_State* L, ScriptApi& api) {
    lua_pushboolean(L, input(api).went_up(check_key(L, 1)));
    return 1;
}
int input_look(lua_State* L, ScriptApi& api) {
    const auto& frame = input(api);
    lua_pushvector(L, frame.look.x, frame.look.y, 0.0f);
    return 1;
}
int input_scroll(lua_State* L, ScriptApi& api) {
    lua_pushnumber(L, input(api).scroll);
    return 1;
}

// --- Quaternions -----------------------------------------------------------------------------------

int quaternion_new(lua_State* L) {
    push_quaternion(L, {float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
                        float(luaL_checknumber(L, 4))});
    return 1;
}
int quaternion_identity(lua_State* L) {
    push_quaternion(L, math::Quat{});
    return 1;
}
int quaternion_from_axis_angle(lua_State* L) {
    auto axis = check_vector(L, 1);
    const auto angle = float(luaL_checknumber(L, 2));
    const auto length = axis.length();
    if (!(length > 1e-12f)) luaL_error(L, "the axis must not be zero");
    push_quaternion(L, math::Quat::from_axis_angle(axis * (1.0f / length), angle));
    return 1;
}
int quaternion_index(lua_State* L) {
    const auto q = check_quaternion(L, 1);
    const auto key = std::string_view(luaL_checkstring(L, 2));
    if (key.size() == 1) {
        const auto* component = key == "x" ? &q.x : key == "y" ? &q.y : key == "z" ? &q.z : key == "w" ? &q.w : nullptr;
        if (component) {
            lua_pushnumber(L, *component);
            return 1;
        }
    }
    lua_getuserdatametatable(L, quaternion_tag);
    lua_rawgetfield(L, -1, "methods");
    lua_rawgetfield(L, -1, std::string(key).c_str());
    return 1;
}
int quaternion_rotate(lua_State* L) {
    push_vector(L, check_quaternion(L, 1).rotate(check_vector(L, 2)));
    return 1;
}
int quaternion_normalized(lua_State* L) {
    auto q = check_quaternion(L, 1);
    const auto length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(length > 1e-12f)) luaL_error(L, "a zero quaternion cannot be normalized");
    push_quaternion(L, {q.x / length, q.y / length, q.z / length, q.w / length});
    return 1;
}
int quaternion_inverse(lua_State* L) {
    const auto q = check_quaternion(L, 1); // unit quaternions: the conjugate
    push_quaternion(L, {-q.x, -q.y, -q.z, q.w});
    return 1;
}
int quaternion_mul(lua_State* L) {
    push_quaternion(L, check_quaternion(L, 1) * check_quaternion(L, 2));
    return 1;
}
int quaternion_eq(lua_State* L) {
    const auto a = check_quaternion(L, 1), b = check_quaternion(L, 2);
    lua_pushboolean(L, a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w);
    return 1;
}
int quaternion_tostring(lua_State* L) {
    const auto q = check_quaternion(L, 1);
    auto text = std::ostringstream{};
    text << "quaternion(" << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ')';
    lua_pushstring(L, text.str().c_str());
    return 1;
}

// --- Entities --------------------------------------------------------------------------------------

template<class T>
std::optional<T> committed(const ScriptApi& api, EntityHandle handle) {
    auto value = std::optional<T>{};
    api.world().with<T>(handle, [&](const T& stored) { value = stored; });
    return value;
}

TransformComponent transform(const ScriptApi& api, EntityId id) {
    const auto value = committed<TransformComponent>(api, live(api, id));
    if (!value) throw ScriptError("entity " + id_text(id) + " has no transform");
    return *value;
}

int entity_id(lua_State* L, ScriptApi&) {
    const auto text = id_text(check_entity(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int entity_alive(lua_State* L, ScriptApi& api) {
    lua_pushboolean(L, api.world().find(check_entity(L, 1)).has_value());
    return 1;
}
int entity_name(lua_State* L, ScriptApi& api) {
    const auto name = committed<NameComponent>(api, live(api, check_entity(L, 1)));
    lua_pushstring(L, name ? name->value.c_str() : "");
    return 1;
}
int entity_parent(lua_State* L, ScriptApi& api) {
    const auto parent = api.world().parent(live(api, check_entity(L, 1)));
    const auto id = parent ? api.world().persistent_id(*parent) : std::nullopt;
    if (id) push_entity(L, *id);
    else lua_pushnil(L);
    return 1;
}
int entity_children(lua_State* L, ScriptApi& api) {
    const auto children = api.world().children(live(api, check_entity(L, 1)));
    lua_createtable(L, int(children.size()), 0);
    int index = 1;
    for (const auto child : children)
        if (const auto id = api.world().persistent_id(child)) {
            push_entity(L, *id);
            lua_rawseti(L, -2, index++);
        }
    return 1;
}
int entity_position(lua_State* L, ScriptApi& api) {
    push_vector(L, transform(api, check_entity(L, 1)).translation);
    return 1;
}
int entity_rotation(lua_State* L, ScriptApi& api) {
    push_quaternion(L, transform(api, check_entity(L, 1)).rotation);
    return 1;
}
int entity_scale(lua_State* L, ScriptApi& api) {
    push_vector(L, transform(api, check_entity(L, 1)).scale);
    return 1;
}
TransformComponent world_pose(const ScriptApi& api, EntityId id) {
    const auto matrix = api.world().world_matrix(live(api, id));
    const auto pose = matrix ? decompose_transform(*matrix) : std::nullopt;
    if (!pose) throw ScriptError("entity " + id_text(id) + " has no world pose that separates into position, rotation, and scale");
    return *pose;
}
int entity_world_position(lua_State* L, ScriptApi& api) {
    push_vector(L, world_pose(api, check_entity(L, 1)).translation);
    return 1;
}
int entity_world_rotation(lua_State* L, ScriptApi& api) {
    push_quaternion(L, world_pose(api, check_entity(L, 1)).rotation);
    return 1;
}
int entity_set_position(lua_State* L, ScriptApi& api) {
    require_writable(api, "move entities");
    api.edit(check_entity(L, 1), ComponentId::transform, 1, check_vector(L, 2));
    return 0;
}
int entity_set_rotation(lua_State* L, ScriptApi& api) {
    require_writable(api, "turn entities");
    api.edit(check_entity(L, 1), ComponentId::transform, 2, check_quaternion(L, 2));
    return 0;
}
int entity_set_scale(lua_State* L, ScriptApi& api) {
    require_writable(api, "scale entities");
    api.edit(check_entity(L, 1), ComponentId::transform, 3, check_vector(L, 2));
    return 0;
}
int entity_has(lua_State* L, ScriptApi& api) {
    const auto handle = live(api, check_entity(L, 1));
    const auto& schema = check_component(luaL_checkstring(L, 2));
    lua_pushboolean(L, read_component(api.world(), handle, schema.id).has_value());
    return 1;
}
int entity_get(lua_State* L, ScriptApi& api) {
    const auto id = check_entity(L, 1);
    const auto& schema = check_component(luaL_checkstring(L, 2));
    const auto& property = check_property(schema, luaL_checkstring(L, 3));
    const auto value = read_component(api.world(), live(api, id), schema.id);
    if (!value) throw ScriptError("entity " + id_text(id) + " has no " + std::string(schema.name));
    push_property(L, property, *read_property(*value, property.id));
    return 1;
}
int entity_set(lua_State* L, ScriptApi& api) {
    require_writable(api, "change properties");
    const auto id = check_entity(L, 1);
    const auto& schema = check_component(luaL_checkstring(L, 2));
    const auto& property = check_property(schema, luaL_checkstring(L, 3));
    api.edit(id, schema.id, property.id, to_property(L, 4, property));
    return 0;
}
int entity_destroy(lua_State* L, ScriptApi& api) {
    require_writable(api, "destroy entities");
    api.destroy(check_entity(L, 1));
    return 0;
}
int entity_eq(lua_State* L) {
    lua_pushboolean(L, check_entity(L, 1) == check_entity(L, 2));
    return 1;
}
int entity_tostring(lua_State* L) {
    const auto text = "entity " + id_text(check_entity(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int entity_index(lua_State* L) {
    check_entity(L, 1);
    lua_getuserdatametatable(L, entity_tag);
    lua_rawgetfield(L, -1, "methods");
    lua_rawgetfield(L, -1, luaL_checkstring(L, 2));
    return 1;
}

void set_functions(lua_State* L, std::initializer_list<luaL_Reg> functions) {
    for (const auto& function : functions) {
        lua_pushcfunction(L, function.func, function.name);
        lua_setfield(L, -2, function.name);
    }
}

void make_metatable(lua_State* L, int tag, lua_CFunction index, lua_CFunction eq, lua_CFunction to_string,
                    std::initializer_list<luaL_Reg> methods, lua_CFunction mul = nullptr) {
    lua_newtable(L);
    lua_pushcfunction(L, index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, eq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, to_string, "__tostring");
    lua_setfield(L, -2, "__tostring");
    if (mul) {
        lua_pushcfunction(L, mul, "__mul");
        lua_setfield(L, -2, "__mul");
    }
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "__metatable");
    lua_newtable(L);
    set_functions(L, methods);
    lua_setreadonly(L, -1, true);
    lua_setfield(L, -2, "methods");
    lua_setreadonly(L, -1, true);
    lua_setuserdatametatable(L, tag);
}

} // namespace

// --- The VM ----------------------------------------------------------------------------------------

Vm::Vm(ScriptLimits limits, uint64_t seed) : m_limits(limits) {
    m_state = lua_newstate(&Vm::allocate, this);
    if (!m_state) throw std::runtime_error("the script VM could not be created within its memory limit");
    auto* callbacks = lua_callbacks(m_state);
    callbacks->userdata = this;
    callbacks->interrupt = &Vm::interrupt;
    luaL_openlibs(m_state);
    // Scripts see no stdout, stack inspection, clock, or environment switching; errors reach the host.
    for (const auto* name : {"print", "debug", "os", "getfenv", "setfenv"}) {
        lua_pushnil(m_state);
        lua_setglobal(m_state, name);
    }
    // math.random repeats from the session's seed instead of Luau's clock-and-address seed.
    lua_getglobal(m_state, "math");
    lua_getfield(m_state, -1, "randomseed");
    lua_pushinteger(m_state, int(seed & 0x7fffffff));
    lua_call(m_state, 1, 0);
    lua_pop(m_state, 1);
    open_maya_library(m_state);
    luaL_sandbox(m_state); // the global table and every library become read-only
    // Made once, so that protect allocates nothing of its own per call.
    lua_pushcfunction(
        m_state,
        [](lua_State* L) -> int {
            auto* task = static_cast<Task*>(lua_touserdata(L, 1));
            task->run(L, task->data);
            return 0;
        },
        "protect");
    m_runner = lua_ref(m_state, -1);
    lua_pop(m_state, 1);
}

Vm::~Vm() {
    if (m_state) lua_close(m_state);
}

void* Vm::allocate(void* user, void* block, size_t old_size, size_t new_size) {
    auto& vm = *static_cast<Vm*>(user);
    if (!block) old_size = 0;
    if (new_size == 0) {
        std::free(block);
        vm.m_bytes -= old_size;
        return nullptr;
    }
    if (new_size > old_size && vm.m_bytes - old_size + new_size > vm.m_limits.memory_bytes) return nullptr;
    auto* moved = std::realloc(block, new_size);
    if (moved) vm.m_bytes = vm.m_bytes - old_size + new_size;
    return moved;
}

void Vm::interrupt(lua_State* L, int gc) {
    if (gc >= 0) return; // a garbage collection step, not script work
    auto& vm = of(L);
    if (++vm.m_work > vm.m_limits.work_per_call)
        luaL_error(L, "work budget exceeded (%llu safepoints)", static_cast<unsigned long long>(vm.m_limits.work_per_call));
}

namespace {
/// Adds a short traceback to an error message.
int traceback(lua_State* L) {
    const auto* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "error object is not a string", 1);
    return 1;
}

std::string take_error(lua_State* L) {
    const auto* message = lua_tostring(L, -1);
    auto text = std::string(message ? message : "error object is not a string");
    lua_pop(L, 1);
    // Keep the message and at most four traceback lines.
    auto lines = 0;
    for (size_t at = 0; (at = text.find('\n', at)) != std::string::npos; ++at)
        if (++lines == 5) {
            text.resize(at);
            break;
        }
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}
} // namespace

std::string compile(std::string_view source) {
    return Luau::compile(std::string(source));
}

Vm::Loaded Vm::load_bytecode(std::string_view name, const std::string& bytecode) {
    // Everything here that allocates in the VM runs protected, so running out of memory is this
    // script's error, not the session's.
    auto loaded = Loaded{};
    if (auto error = protect(m_state, [&](lua_State* L) {
            loaded.thread = lua_newthread(L);
            loaded.thread_ref = lua_ref(L, -1);
        });
        !error.empty()) {
        loaded.thread = nullptr;
        loaded.error = std::string(name) + ": " + error;
        return loaded;
    }
    const auto chunk = "=" + std::string(name);
    if (auto error = protect(loaded.thread, [&](lua_State* L) {
            luaL_sandboxthread(L); // its own globals, falling through to the read-only ones
            if (luau_load(L, chunk.c_str(), bytecode.data(), bytecode.size(), 0) != 0) {
                loaded.error = take_error(L);
                return;
            }
            loaded.error = call(L, 0, 1); // the chunk returns its module
            if (!loaded.error.empty()) return;
            if (!lua_istable(L, -1)) {
                loaded.error = std::string(name) + ": a script must return a table of hooks and properties";
                return;
            }
            loaded.module_ref = lua_ref(L, -1);
        });
        !error.empty())
        loaded.error = error;
    // Compile and runtime errors start with "name:line:"; others, such as running out of memory, get the name.
    if (!loaded.error.empty() && !loaded.error.starts_with(std::string(name) + ":"))
        loaded.error = std::string(name) + ": " + loaded.error;
    return loaded;
}

void Vm::release(const Loaded& loaded) {
    if (loaded.module_ref != LUA_NOREF) lua_unref(m_state, loaded.module_ref);
    if (loaded.thread_ref != LUA_NOREF) lua_unref(m_state, loaded.thread_ref);
}

std::string Vm::call(lua_State* thread, int arguments, int results) {
    // The function and its arguments are on top; the traceback handler goes below them.
    const auto base = lua_gettop(thread) - arguments;
    lua_pushcfunction(thread, traceback, "traceback");
    lua_insert(thread, base);
    m_work = 0;
    const auto status = lua_pcall(thread, arguments, results, base);
    if (status != LUA_OK) {
        auto error = take_error(thread);
        lua_remove(thread, base);
        return error;
    }
    lua_remove(thread, base);
    return {};
}

std::string Vm::run_protected(lua_State* thread, Task& task) {
    // Pushing the runner and its task needs no allocation; the call's own stack growth is protected.
    lua_getref(thread, m_runner);
    lua_pushlightuserdata(thread, &task);
    const auto status = lua_pcall(thread, 1, 0, 0);
    return status == LUA_OK ? std::string{} : take_error(thread);
}

// --- Values shared with the host ---------------------------------------------------------------------

void push_entity(lua_State* L, EntityId id) {
    auto* box = static_cast<EntityBox*>(lua_newuserdatataggedwithmetatable(L, sizeof(EntityBox), entity_tag));
    box->id = id;
}

void push_quaternion(lua_State* L, const math::Quat& value) {
    auto* box = static_cast<QuaternionBox*>(lua_newuserdatataggedwithmetatable(L, sizeof(QuaternionBox), quaternion_tag));
    *box = {value.x, value.y, value.z, value.w};
}

void push_script_value(lua_State* L, const ScriptValue& value) {
    std::visit([&]<class T>(const T& data) {
        if constexpr (std::same_as<T, float>) lua_pushnumber(L, data);
        else if constexpr (std::same_as<T, int32_t>) lua_pushinteger(L, data);
        else if constexpr (std::same_as<T, bool>) lua_pushboolean(L, data);
        else if constexpr (std::same_as<T, std::string>) lua_pushlstring(L, data.data(), data.size());
        else if constexpr (std::same_as<T, math::Vec3>) push_vector(L, data);
        else {
            if (data.valid()) push_entity(L, data);
            else lua_pushnil(L);
        }
    }, value.data);
}

std::optional<ScriptValueData> to_script_value(lua_State* L, int index, ScriptValueType type) {
    switch (type) {
    case ScriptValueType::number:
        if (lua_type(L, index) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, index))) return float(lua_tonumber(L, index));
        return std::nullopt;
    case ScriptValueType::integer: {
        if (lua_type(L, index) != LUA_TNUMBER) return std::nullopt;
        const auto number = lua_tonumber(L, index);
        if (number != std::floor(number) || number < INT32_MIN || number > INT32_MAX) return std::nullopt;
        return int32_t(number);
    }
    case ScriptValueType::boolean:
        if (lua_isboolean(L, index)) return lua_toboolean(L, index) != 0;
        return std::nullopt;
    case ScriptValueType::string:
        if (lua_type(L, index) == LUA_TSTRING) return std::string(lua_tostring(L, index));
        return std::nullopt;
    case ScriptValueType::vector:
    case ScriptValueType::color:
        if (lua_isvector(L, index)) {
            const auto* v = lua_tovector(L, index);
            if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) return math::Vec3{v[0], v[1], v[2]};
        }
        return std::nullopt;
    case ScriptValueType::entity:
        if (lua_isnil(L, index)) return EntityId{};
        if (const auto* box = static_cast<EntityBox*>(lua_touserdatatagged(L, index, entity_tag))) return box->id;
        return std::nullopt;
    }
    return std::nullopt;
}

void open_maya_library(lua_State* L) {
    make_metatable(L, entity_tag, entity_index, entity_eq, entity_tostring,
                   {{"id", guarded<entity_id>}, {"alive", guarded<entity_alive>}, {"name", guarded<entity_name>},
                    {"parent", guarded<entity_parent>}, {"children", guarded<entity_children>},
                    {"position", guarded<entity_position>}, {"rotation", guarded<entity_rotation>},
                    {"scale", guarded<entity_scale>}, {"world_position", guarded<entity_world_position>},
                    {"world_rotation", guarded<entity_world_rotation>}, {"set_position", guarded<entity_set_position>},
                    {"set_rotation", guarded<entity_set_rotation>}, {"set_scale", guarded<entity_set_scale>},
                    {"has", guarded<entity_has>}, {"get", guarded<entity_get>}, {"set", guarded<entity_set>},
                    {"destroy", guarded<entity_destroy>}});
    make_metatable(L, quaternion_tag, quaternion_index, quaternion_eq, quaternion_tostring,
                   {{"rotate", pure<quaternion_rotate>}, {"normalized", pure<quaternion_normalized>},
                    {"inverse", pure<quaternion_inverse>}},
                   quaternion_mul);
    lua_newtable(L);
    set_functions(L, {{"log", guarded<maya_log>}, {"tick", guarded<maya_tick>}, {"time", guarded<maya_time>},
                      {"delta", guarded<maya_delta>}, {"find", guarded<maya_find>}, {"create", guarded<maya_create>}});
    lua_newtable(L);
    set_functions(L, {{"down", guarded<input_down>}, {"pressed", guarded<input_pressed>}, {"released", guarded<input_released>},
                      {"look", guarded<input_look>}, {"scroll", guarded<input_scroll>}});
    lua_setfield(L, -2, "input");
    lua_newtable(L);
    set_functions(L, {{"new", pure<quaternion_new>}, {"identity", pure<quaternion_identity>},
                      {"from_axis_angle", pure<quaternion_from_axis_angle>}});
    lua_setfield(L, -2, "quaternion");
    lua_setglobal(L, "maya");
}

} // namespace maya::scripting
