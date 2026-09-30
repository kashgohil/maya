// The script system: maya.script components run as script instances in a play session
// (docs/scripting.md). One VM per session; hooks run in activation order; a failing instance is
// reported, its call's changes are discarded, and it is disabled while everything else keeps running.
#include "luau.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace maya::scripting {
namespace {

std::string id_text(EntityId id) {
    auto text = std::ostringstream{};
    text << std::hex << id.high << ' ' << id.low;
    return text.str();
}

/// Hashes for this tick's pending edits (entity, component) and writers (entity, component, property).
struct EditKeyHash {
    size_t operator()(const std::pair<EntityId, ComponentId>& key) const noexcept {
        return PersistentIdHash{}(key.first) * 31u + size_t(key.second);
    }
    size_t operator()(const std::tuple<EntityId, ComponentId, PropertyId>& key) const noexcept {
        return (PersistentIdHash{}(std::get<0>(key)) * 31u + size_t(std::get<1>(key))) * 31u + size_t(std::get<2>(key));
    }
};

uint64_t splitmix(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

struct Compiled {
    AssetId asset;
    std::string name;
    Vm::Loaded loaded;
    int metatable_ref = LUA_NOREF; // {__index = module}, shared by the script's instances
    ScriptDescription description;
    std::string error; // why the script cannot run
    bool reported = false;
};

class ScriptSystem final : public SimulationSystem, public ScriptApi {
public:
    ScriptSystem(ScriptSources sources, ScriptSettings settings) : m_sources(std::move(sources)), m_settings(settings) {}

    std::string_view name() const override { return "Scripts"; }

    void start(const World& world) override {
        m_world = &world;
        m_vm = std::make_unique<Vm>(m_settings.limits, m_settings.seed);
        m_vm->set_api(this);
    }

    void fixed_update(TickContext& tick) override {
        m_tick_context = &tick;
        m_physics = &tick.physics;
        m_commands = &tick.commands;
        m_bodies = &tick.bodies;
        m_messages = &tick.messages;
        const auto clear = Scope([this] {
            m_tick_context = nullptr;
            m_commands = nullptr;
            m_bodies = nullptr;
            m_messages = nullptr;
        });
        // Phase 1: instances whose entity or script went away stop, new script versions replace their
        // instances, and then new instances start.
        for (auto& instance : m_instances) {
            if (!instance.active) continue;
            const auto handle = tick.world.find(instance.entity);
            auto current = std::optional<ScriptComponent>{};
            if (handle) tick.world.with<ScriptComponent>(*handle, [&](const ScriptComponent& value) { current = value; });
            if (!current || current->script.id != instance.script) retire(instance);
        }
        apply_reloads();
        auto arriving = std::vector<std::pair<EntityHandle, AssetId>>{};
        tick.world.for_each<ScriptComponent>([&](EntityHandle entity, const ScriptComponent& component) {
            const auto id = tick.world.persistent_id(entity);
            if (id && component.script.valid() && !m_by_entity.contains(*id)) arriving.emplace_back(entity, component.script.id);
        });
        for (const auto& [entity, script] : arriving) activate(entity, script);
        // Phase 3: fixed updates, in activation order.
        for (auto& instance : m_instances)
            if (instance.active && !instance.disabled) call_hook(instance, "fixed_update", CallMode::fixed, tick.delta);
        emit();
        compact();
        if (m_settings.reloads) m_settings.reloads->set_memory(m_vm->memory());
    }

    /// Phase 7: each event goes to its two entities' instances (first, then second), then every
    /// late_fixed_update runs. When the session stops, only the events, with nothing to change.
    void late_fixed_update(TickContext& tick) override {
        m_tick_context = &tick;
        m_physics = &tick.physics;
        m_commands = tick.stopping ? nullptr : &tick.commands;
        m_bodies = tick.stopping ? nullptr : &tick.bodies;
        m_messages = &tick.messages;
        const auto clear = Scope([this] {
            m_tick_context = nullptr;
            m_commands = nullptr;
            m_bodies = nullptr;
            m_messages = nullptr;
        });
        const auto mode = tick.stopping ? CallMode::stop : CallMode::late;
        for (const auto& event : tick.events) {
            deliver(event, event.first_entity, event.first, event.second, 1.0f, mode);
            deliver(event, event.second_entity, event.second, event.first, -1.0f, mode);
        }
        if (!tick.stopping)
            for (auto& instance : m_instances)
                if (instance.active && !instance.disabled) call_hook(instance, "late_fixed_update", CallMode::late, tick.delta);
        if (tick.stopping) m_pending.clear();
        else emit();
    }

    void frame(FrameContext& frame) override {
        m_frame_context = &frame;
        m_physics = &frame.physics;
        m_messages = &frame.messages;
        const auto clear = Scope([this] {
            m_frame_context = nullptr;
            m_messages = nullptr;
        });
        for (auto& instance : m_instances)
            if (instance.active && !instance.disabled) call_hook(instance, "update", CallMode::update, frame.frame_delta);
    }

    void stop() noexcept override {
        try {
            // At the end of play: stop hooks, newest first, with no world to change.
            for (auto it = m_instances.rbegin(); it != m_instances.rend(); ++it)
                if (it->active && it->started) call_hook(*it, "stop", CallMode::stop, std::nullopt);
        } catch (...) {
        }
        m_instances.clear();
        m_by_entity.clear();
        m_compiled.clear();
        m_vm.reset();
    }

    // --- ScriptApi --------------------------------------------------------------------------------

    CallMode mode() const override { return m_mode; }
    const World& world() const override { return *m_world; }
    const PhysicsWorld* physics() const override { return m_physics; }
    const InputFrame* input() const override {
        const auto ticking = m_mode == CallMode::fixed || m_mode == CallMode::start || m_mode == CallMode::late;
        return m_tick_context && ticking ? &m_tick_context->input : nullptr;
    }
    BodyCommands& bodies() override {
        if (m_mode == CallMode::update) throw ScriptError("update hooks cannot push bodies; make changes in fixed_update");
        if (!m_bodies) throw ScriptError("bodies cannot be pushed at the end of play");
        return *m_bodies;
    }
    uint64_t tick() const override {
        return m_tick_context ? m_tick_context->tick : m_frame_context ? m_frame_context->tick : 0;
    }
    double time() const override {
        return m_tick_context ? m_tick_context->time : m_frame_context ? m_frame_context->time : 0.0;
    }
    float delta() const override { return m_tick_context ? m_tick_context->delta : 0.0f; }

    void log(std::string text) override {
        report(SimulationMessage::Level::info, m_current ? m_current->label + ": " + text : text);
    }

    void edit(EntityId entity, ComponentId component, PropertyId property, PropertyValue value) override {
        if (!m_commands) throw ScriptError("the world cannot change at the end of play");
        const auto handle = m_world->find(entity);
        if (!handle) throw ScriptError("entity " + id_text(entity) + " does not exist (yet, or any more)");
        if (component == ComponentId::script) throw ScriptError("a script's values change through its instance (self)");
        if (component == ComponentId::transform && m_physics)
            if (const auto motion = m_physics->motion_type(*handle); motion && *motion != MotionType::static_body)
                throw ScriptError("cannot set the transform of entity " + id_text(entity) + ": its " + motion_type_name(*motion) +
                                  " body's pose is written by physics; use " +
                                  (*motion == MotionType::kinematic ? "move_kinematic or teleport" : "add_force, add_impulse, set_velocity, or teleport"));
        const auto key = std::pair(entity, component);
        auto found = m_pending_index.find(key);
        auto base = found != m_pending_index.end() ? std::optional(m_pending[found->second].value)
                                                   : read_component(*m_world, *handle, component);
        if (!base) throw ScriptError("entity " + id_text(entity) + " has no " + std::string(component_schema(component)->name));
        const auto change = std::array{PropertyEdit{property, std::move(value)}};
        if (const auto result = edit_properties(*base, change); !result)
            throw ScriptError(std::string(component_schema(component)->name) + "." +
                              std::string(property_schema(component, property)->name) + ": " + std::string(result.message));
        if (found == m_pending_index.end()) {
            m_pending_index.emplace(key, m_pending.size());
            m_pending.push_back({entity, component, std::move(*base)});
        } else {
            if (found->second < m_call_pending && !m_journaled.contains(found->second)) {
                m_journal.push_back({found->second, m_pending[found->second].value});
                m_journaled.insert(found->second);
            }
            m_pending[found->second].value = std::move(*base);
        }
        // Two instances writing one property in a tick: the later one wins, and both are named.
        const auto writer = std::tuple(entity, component, property);
        if (const auto previous = m_writers.find(writer); previous != m_writers.end() && previous->second != m_current_index) {
            report(SimulationMessage::Level::warning,
                   m_instances[previous->second].label + " and " + m_current->label + " both set " +
                       std::string(component_schema(component)->name) + "." + std::string(property_schema(component, property)->name) +
                       " of entity " + id_text(entity) + " this tick; " + m_current->label + "'s value wins");
        }
        if (!m_writers.contains(writer)) m_call_writers.push_back(writer);
        m_writers[writer] = m_current_index;
    }

    EntityId create(std::string name, math::Vec3 position, std::optional<EntityId> parent) override {
        if (!m_commands) throw ScriptError("the world cannot change at the end of play");
        auto parent_handle = std::optional<EntityHandle>{};
        if (parent) {
            parent_handle = m_world->find(*parent);
            if (!parent_handle) throw ScriptError("the parent entity " + id_text(*parent) + " does not exist");
            if (!m_world->has<TransformComponent>(*parent_handle)) throw ScriptError("the parent entity has no transform");
        }
        auto transform = TransformComponent{};
        transform.translation = position;
        if (!validated_transform(transform)) throw ScriptError("the position is not finite");
        const auto id = next_id();
        const auto pending = m_commands->create(id);
        m_commands->add(pending, NameComponent{std::move(name)});
        m_commands->add(pending, transform);
        if (parent_handle) m_commands->reparent(pending, *parent_handle, ReparentPolicy::keep_local);
        m_created.insert(id);
        return id;
    }

    void destroy(EntityId entity) override {
        if (!m_commands) throw ScriptError("the world cannot change at the end of play");
        const auto handle = m_world->find(entity);
        if (!handle) throw ScriptError("entity " + id_text(entity) + " does not exist (yet, or any more)");
        m_commands->destroy(*handle);
        m_destroyed.insert(entity);
    }

private:
    struct Instance {
        EntityId entity;
        AssetId script;
        size_t compiled = 0;
        int self_ref = LUA_NOREF;
        bool active = true; // false once stopped or retired
        bool started = false; // start was entered
        bool disabled = false; // failed: no more hooks but stop
        std::string label; // "Crate (scripts/mover.luau)"
    };

    /// Runs a function when it goes out of scope.
    template<class F> struct Scope {
        F f;
        explicit Scope(F value) : f(std::move(value)) {}
        ~Scope() { f(); }
    };

    void report(SimulationMessage::Level level, std::string text) {
        if (!m_messages) return;
        m_messages->push_back({level, "Scripts", std::move(text), tick()});
    }

    Compiled& compiled(AssetId asset) {
        for (auto& script : m_compiled)
            if (script.asset == asset) return script;
        auto& script = m_compiled.emplace_back();
        script.asset = asset;
        script.name = id_text(EntityId{asset.high, asset.low});
        auto source = m_sources ? m_sources(asset) : ScriptSourceResult{std::nullopt, "no script sources were given"};
        if (!source.source) {
            script.error = "script " + script.name + " cannot be read: " + source.error;
            return script;
        }
        script = load(asset, source.source->name, compile(source.source->text));
        return script;
    }

    /// Loads a script version into the session's VM: its module, declarations, and instance metatable.
    Compiled load(AssetId asset, std::string name, const std::string& bytecode) {
        auto script = Compiled{};
        script.asset = asset;
        script.name = std::move(name);
        const auto previous = std::exchange(m_mode, CallMode::declare);
        script.loaded = m_vm->load_bytecode(script.name, bytecode);
        m_mode = previous;
        if (!script.loaded.error.empty()) {
            script.error = script.loaded.error;
            return script;
        }
        const auto error = m_vm->protect(script.loaded.thread, [&](lua_State* L) {
            lua_getref(L, script.loaded.module_ref);
            script.description = describe_module(L, -1, script.name);
            if (!script.description.error.empty()) return;
            lua_newtable(L);
            lua_getref(L, script.loaded.module_ref);
            lua_setfield(L, -2, "__index");
            script.metatable_ref = lua_ref(L, -1);
        });
        script.error = !error.empty() ? script.name + ": " + error : script.description.error;
        return script;
    }

    void release(Compiled& script) {
        if (script.metatable_ref != LUA_NOREF) lua_unref(m_vm->state(), script.metatable_ref);
        m_vm->release(script.loaded);
        script.metatable_ref = LUA_NOREF;
        script.loaded = {};
    }

    void activate(EntityHandle entity, AssetId asset) {
        const auto id = *m_world->persistent_id(entity);
        auto& script = compiled(asset);
        auto instance = Instance{};
        instance.entity = id;
        instance.script = asset;
        instance.compiled = size_t(&script - m_compiled.data());
        auto name = std::string{};
        m_world->with<NameComponent>(entity, [&](const NameComponent& value) { name = value.value; });
        instance.label = (name.empty() ? "entity " + id_text(id) : name) + " (" + script.name + ")";
        m_by_entity.emplace(id, m_instances.size());
        if (!script.error.empty()) {
            instance.disabled = true;
            if (!script.reported) report(SimulationMessage::Level::error, instance.label + ": " + script.error);
            script.reported = true;
            m_instances.push_back(std::move(instance));
            return;
        }
        auto values = std::vector<ScriptValue>{};
        m_world->with<ScriptComponent>(entity, [&](const ScriptComponent& component) { values = component.values; });
        for (const auto& problem : script_value_problems(script.description, values))
            report(SimulationMessage::Level::warning, instance.label + ": " + problem);
        m_instances.push_back(std::move(instance));
        start_instance(m_instances.back(), values);
    }

    /// Calls a recipient's event hook with the other entity and the contact: its point, the normal
    /// from the recipient toward the other, the approach speed, and whether a body was removed.
    void deliver(const PhysicsEvent& event, const std::optional<EntityHandle>& recipient, EntityId self, EntityId other,
                 float sign, CallMode mode) {
        if (!recipient) return; // gone, or being destroyed: counted by the physics world
        const auto found = m_by_entity.find(self);
        if (found == m_by_entity.end()) return;
        auto& instance = m_instances[found->second];
        if (!instance.active || instance.disabled) return;
        const char* hooks[] = {"on_contact_begin", "on_contact_end", "on_trigger_enter", "on_trigger_exit"};
        call_hook_with(instance, hooks[size_t(event.kind)], mode, [&](lua_State* L) {
            push_entity(L, other);
            lua_createtable(L, 0, 4);
            push_script_value(L, {"point", ScriptValueType::vector, event.point});
            lua_setfield(L, -2, "point");
            push_script_value(L, {"normal", ScriptValueType::vector, event.normal * sign});
            lua_setfield(L, -2, "normal");
            lua_pushnumber(L, event.speed);
            lua_setfield(L, -2, "speed");
            lua_pushboolean(L, event.removed);
            lua_setfield(L, -2, "removed");
            return 2;
        });
    }

    /// Builds an instance's self (its entity, and each declared property from `values` when one fits,
    /// else its default) and runs start.
    void start_instance(Instance& instance, const std::vector<ScriptValue>& values) {
        const auto& script = m_compiled[instance.compiled];
        const auto error = m_vm->protect(script.loaded.thread, [&](lua_State* L) {
            lua_newtable(L);
            push_entity(L, instance.entity);
            lua_setfield(L, -2, "entity");
            for (const auto& declared : script.description.properties) {
                auto chosen = ScriptValue{declared.name, declared.type, declared.default_value};
                for (const auto& value : values)
                    if (value.name == declared.name && value.type == declared.type && fits(declared, value)) chosen = value;
                push_script_value(L, chosen);
                lua_setfield(L, -2, declared.name.c_str());
            }
            lua_getref(L, script.metatable_ref);
            lua_setmetatable(L, -2);
            instance.self_ref = lua_ref(L, -1);
        });
        if (!error.empty()) { // it never starts, so it never stops
            instance.self_ref = LUA_NOREF;
            instance.disabled = true;
            report(SimulationMessage::Level::error, instance.label + ": " + error + " (in start; the instance is stopped)");
            return;
        }
        instance.started = true;
        call_hook(instance, "start", CallMode::start, std::nullopt);
    }

    /// Swaps in the script versions the host offered: each instance of a script keeps its exposed
    /// properties' current values, its old version stops, and then the new version starts, both in
    /// activation order. A version that cannot load here leaves the running one in place.
    void apply_reloads() {
        if (!m_settings.reloads) return;
        for (auto& version : m_settings.reloads->take()) {
            auto fresh = load(version.script, version.name, version.bytecode);
            if (!fresh.error.empty()) {
                report(SimulationMessage::Level::error, fresh.error + " (the running version stays)");
                release(fresh);
                continue;
            }
            const auto found = std::ranges::find(m_compiled, version.script, &Compiled::asset);
            if (found == m_compiled.end()) { // not used yet: later instances start from this version
                m_compiled.push_back(std::move(fresh));
                m_settings.reloads->record({version.script, version.name, tick(), 0});
                continue;
            }
            const auto index = size_t(found - m_compiled.begin());
            auto replaced = std::vector<std::pair<size_t, std::vector<ScriptValue>>>{};
            for (size_t i = 0; i < m_instances.size(); ++i)
                if (m_instances[i].active && m_instances[i].compiled == index) replaced.emplace_back(i, current_values(m_instances[i], fresh));
            for (const auto& [i, kept] : replaced) {
                auto& instance = m_instances[i];
                if (instance.started) call_hook(instance, "stop", CallMode::stop, std::nullopt);
                if (instance.self_ref != LUA_NOREF) lua_unref(m_vm->state(), instance.self_ref);
                instance.self_ref = LUA_NOREF;
                instance.started = instance.disabled = false;
            }
            release(m_compiled[index]);
            m_compiled[index] = std::move(fresh);
            for (const auto& [i, kept] : replaced) {
                // The authored values, then the kept play values, which win (the last fitting value is used).
                auto values = std::vector<ScriptValue>{};
                if (const auto handle = m_world->find(m_instances[i].entity))
                    m_world->with<ScriptComponent>(*handle, [&](const ScriptComponent& component) { values = component.values; });
                values.insert(values.end(), kept.begin(), kept.end());
                start_instance(m_instances[i], values);
            }
            m_settings.reloads->record({version.script, version.name, tick(), replaced.size()});
            report(SimulationMessage::Level::info, "Reloaded " + version.name + " (" + std::to_string(replaced.size()) +
                                                       (replaced.size() == 1 ? " instance)" : " instances)"));
        }
    }

    /// The values of an instance's exposed properties that `next` also declares, with the same type.
    std::vector<ScriptValue> current_values(const Instance& instance, const Compiled& next) {
        auto values = std::vector<ScriptValue>{};
        if (instance.self_ref == LUA_NOREF) return values;
        const auto& script = m_compiled[instance.compiled];
        m_vm->protect(script.loaded.thread, [&](lua_State* L) {
            lua_getref(L, instance.self_ref);
            for (const auto& declared : next.description.properties) {
                lua_rawgetfield(L, -1, declared.name.c_str());
                if (auto data = to_script_value(L, -1, declared.type))
                    values.push_back({declared.name, declared.type, std::move(*data)});
                lua_pop(L, 1);
            }
        });
        return values;
    }

    static bool fits(const ScriptPropertyDeclaration& declared, const ScriptValue& value) {
        auto number = std::optional<float>{};
        if (const auto* f = std::get_if<float>(&value.data)) number = *f;
        if (const auto* i = std::get_if<int32_t>(&value.data)) number = float(*i);
        if (!number) return true;
        return !(declared.minimum && *number < *declared.minimum) && !(declared.maximum && *number > *declared.maximum);
    }

    /// Stops an instance whose entity or script went away during play.
    void retire(Instance& instance) {
        if (instance.started) call_hook(instance, "stop", CallMode::stop, std::nullopt);
        instance.active = false;
        m_by_entity.erase(instance.entity);
    }

    /// Calls a hook if the script defines it. A failure discards the call's changes and disables the
    /// instance.
    void call_hook(Instance& instance, const char* hook, CallMode mode, std::optional<double> argument) {
        call_hook_with(instance, hook, mode, [&](lua_State* L) {
            if (argument) lua_pushnumber(L, *argument);
            return argument ? 1 : 0;
        });
    }
    /// Calls a hook with the arguments `push` places on the stack (it returns how many).
    template<class Push> void call_hook_with(Instance& instance, const char* hook, CallMode mode, Push&& push) {
        auto& script = m_compiled[instance.compiled];
        if (!script.error.empty() || instance.self_ref == LUA_NOREF) return;
        if (std::ranges::find(script.description.hooks, std::string_view(hook)) == script.description.hooks.end()) return;
        // The call is a transaction over the tick's commands and pending edits.
        const auto commands = m_commands ? m_commands->size() : 0;
        const auto body_requests = m_bodies ? m_bodies->size() : 0;
        const auto counter = m_id_counter;
        m_call_pending = m_pending.size();
        m_journal.clear();
        m_journaled.clear();
        m_call_writers.clear();
        const auto created = m_created;
        const auto destroyed = m_destroyed;
        m_mode = mode;
        m_current = &instance;
        m_current_index = size_t(&instance - m_instances.data());
        auto error = std::string{};
        if (auto failure = m_vm->protect(script.loaded.thread, [&](lua_State* L) {
                lua_getref(L, script.loaded.module_ref);
                if (lua_rawgetfield(L, -1, hook) != LUA_TFUNCTION) return; // removed since the script loaded
                lua_getref(L, instance.self_ref);
                const auto arguments = push(L);
                error = m_vm->call(L, 1 + arguments);
            });
            !failure.empty())
            error = std::move(failure);
        m_current = nullptr;
        m_mode = CallMode::declare;
        if (error.empty()) return;
        if (m_commands) m_commands->truncate(commands);
        if (m_bodies) m_bodies->truncate(body_requests);
        for (auto it = m_journal.rbegin(); it != m_journal.rend(); ++it) m_pending[it->index].value = std::move(it->previous);
        for (auto i = m_call_pending; i < m_pending.size(); ++i)
            m_pending_index.erase(std::pair(m_pending[i].entity, m_pending[i].component));
        m_pending.resize(m_call_pending);
        for (const auto& writer : m_call_writers) m_writers.erase(writer);
        m_id_counter = counter;
        m_created = created;
        m_destroyed = destroyed;
        instance.disabled = true;
        report(SimulationMessage::Level::error, instance.label + ": " + error + " (in " + hook + "; the instance is stopped)");
    }

    /// Appends this tick's property edits, one per changed component, in the order first touched.
    void emit() {
        for (auto& pending : m_pending) {
            if (m_destroyed.contains(pending.entity)) continue;
            const auto handle = m_world->find(pending.entity);
            if (!handle) continue;
            std::visit([&](auto& value) { m_commands->replace(*handle, std::move(value)); }, pending.value);
        }
        m_pending.clear();
        m_pending_index.clear();
        m_writers.clear();
        m_created.clear();
        m_destroyed.clear();
    }

    void compact() {
        if (m_instances.size() < 64 || std::ranges::count_if(m_instances, [](const Instance& i) { return i.active; }) * 2 >
                                           std::ptrdiff_t(m_instances.size()))
            return;
        for (auto& instance : m_instances)
            if (!instance.active && instance.self_ref != LUA_NOREF) lua_unref(m_vm->state(), instance.self_ref);
        std::erase_if(m_instances, [](const Instance& i) { return !i.active; });
        m_by_entity.clear();
        for (size_t i = 0; i < m_instances.size(); ++i) m_by_entity.emplace(m_instances[i].entity, i);
    }

    EntityId next_id() {
        for (;;) {
            const auto high = splitmix(m_settings.seed ^ splitmix(m_id_counter++));
            const auto id = EntityId{high, splitmix(high)};
            if (id.valid() && !m_world->find(id) && !m_created.contains(id)) return id;
        }
    }

    struct Pending {
        EntityId entity;
        ComponentId component;
        ComponentValue value;
    };
    struct JournalEntry {
        size_t index;
        ComponentValue previous;
    };

    ScriptSources m_sources;
    ScriptSettings m_settings;
    std::unique_ptr<Vm> m_vm;
    const World* m_world = nullptr;
    const PhysicsWorld* m_physics = nullptr;
    TickContext* m_tick_context = nullptr;
    FrameContext* m_frame_context = nullptr;
    WorldCommands* m_commands = nullptr;
    BodyCommands* m_bodies = nullptr;
    std::vector<SimulationMessage>* m_messages = nullptr;
    CallMode m_mode = CallMode::declare;
    Instance* m_current = nullptr;
    size_t m_current_index = 0;
    std::vector<Compiled> m_compiled;
    std::vector<Instance> m_instances; // activation order
    std::unordered_map<EntityId, size_t, PersistentIdHash> m_by_entity;
    // This tick's edits, and the current call's undo record.
    std::vector<Pending> m_pending;
    std::unordered_map<std::pair<EntityId, ComponentId>, size_t, EditKeyHash> m_pending_index;
    std::unordered_map<std::tuple<EntityId, ComponentId, PropertyId>, size_t, EditKeyHash> m_writers;
    std::vector<std::tuple<EntityId, ComponentId, PropertyId>> m_call_writers;
    size_t m_call_pending = 0;
    std::vector<JournalEntry> m_journal;
    std::unordered_set<size_t> m_journaled;
    std::unordered_set<EntityId, PersistentIdHash> m_created, m_destroyed;
    uint64_t m_id_counter = 0;
};

} // namespace

std::unique_ptr<SimulationSystem> make_script_system(ScriptSources sources, ScriptSettings settings) {
    return std::make_unique<ScriptSystem>(std::move(sources), settings);
}

} // namespace maya::scripting
