#pragma once

#include "maya/simulation/simulation.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Luau scripts for play sessions (docs/scripting.md). Luau stays inside the scripting host: nothing
// here exposes its types.
namespace maya {

/// A script asset's source text and the name its messages use (normally its project path).
struct ScriptSource {
    std::string name;
    std::string text;
};
struct ScriptSourceResult {
    std::optional<ScriptSource> source;
    std::string error; // why the asset cannot be read, when source is empty
};
/// Finds a script asset's source; see registry_script_sources in script_assets.hpp.
using ScriptSources = std::function<ScriptSourceResult(AssetId)>;

struct ScriptLimits {
    /// Work allowed per hook call, counted at Luau safepoints (loop back edges and calls), never in
    /// wall time, so the same script and inputs always pass or always fail.
    uint64_t work_per_call = 1'000'000;
    size_t memory_bytes = size_t{64} << 20; // the play session's VM
};
class ScriptReloads;
struct ScriptSettings {
    ScriptLimits limits;
    /// Seeds math.random and the IDs of entities scripts create, so a session repeats exactly.
    uint64_t seed = 0x6d617961;
    /// New script versions from the host while the session plays; none when empty.
    std::shared_ptr<ScriptReloads> reloads;
};

/// A property a script declares in its `properties` table.
struct ScriptPropertyDeclaration {
    std::string name;
    ScriptValueType type = ScriptValueType::number;
    ScriptValueData default_value = 0.0f;
    std::optional<float> minimum, maximum; // number and integer only
    std::string unit, label;
};
/// What a script declares: its properties (sorted by name) and the hooks it defines.
struct ScriptDescription {
    std::vector<ScriptPropertyDeclaration> properties;
    std::vector<std::string> hooks;
    std::string error; // "name:line: message" when the script cannot be compiled or described
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Compiles a script and runs its top level in a fresh sandbox, within `limits`, to read what it
/// declares. No hook runs. The editor uses it to show a script's properties.
ScriptDescription describe_script(std::string_view name, std::string_view source, ScriptLimits limits = {});
/// Why `values` do not fit `description`, per value (a type mismatch, a value out of range, or a
/// property the script does not declare); empty when they all fit.
std::vector<std::string> script_value_problems(const ScriptDescription& description, const std::vector<ScriptValue>& values);

/// New versions of scripts for a playing session (docs/scripting.md#reload). The host offers a changed
/// script, which is compiled and described here, off the tick. The session's script system, which
/// shares this object through ScriptSettings::reloads, swaps that script's instances at its next tick:
/// each old instance stops, then the new version starts, in activation order, keeping the exposed
/// properties' current values. Host and session use it on one thread.
class ScriptReloads {
public:
    explicit ScriptReloads(ScriptLimits limits = {}) : m_limits(limits) {}

    /// Offers `source` as the new version of `script`. When it cannot be compiled or described, nothing
    /// changes, the running version stays, and the error is returned ("name:line: message").
    std::string offer(AssetId script, ScriptSource source);

    /// A reload a session applied. Replays of a session that reloaded are not promised to match.
    struct Applied {
        AssetId script;
        std::string name;
        uint64_t tick = 0; // the tick whose boundary swapped it
        size_t instances = 0; // how many instances restarted
    };
    const std::vector<Applied>& applied() const noexcept { return m_applied; }
    /// The session VM's memory in use after its last tick, in bytes.
    size_t memory() const noexcept { return m_memory; }

    // For the script system.
    struct Version {
        AssetId script;
        std::string name;
        std::string bytecode;
    };
    std::vector<Version> take() { return std::exchange(m_offered, {}); }
    void record(Applied applied) { m_applied.push_back(std::move(applied)); }
    void set_memory(size_t bytes) noexcept { m_memory = bytes; }

private:
    ScriptLimits m_limits;
    std::vector<Version> m_offered;
    std::vector<Applied> m_applied;
    size_t m_memory = 0;
};

/// The system that runs maya.script components: one sandboxed Luau VM for the session, instances
/// started in activation order, hooks in the fixed tick and once per frame. A failing script instance
/// is reported (TickContext::messages) and disabled; the session keeps running.
std::unique_ptr<SimulationSystem> script_system(ScriptSources sources, ScriptSettings settings = {});

/// The built-in behaviors followed by the script system: what the player and the editor play.
std::vector<std::unique_ptr<SimulationSystem>> play_systems(ScriptSources sources, ScriptSettings settings = {});

} // namespace maya
