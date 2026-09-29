#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Scripting prototypes for #1015: one host per candidate language (Lua 5.4 and Luau) with the same
// small native API, to compare embedding, sandboxing, limits, and cost in this build before the
// scripting host (#1018) is written. Nothing here is engine API.
namespace maya::prototype {

struct ScriptLimits {
    size_t memory_bytes = 4u << 20; // the VM's allocator refuses to grow past this
    /// Work allowed per call, in the language's own deterministic unit: Lua VM instructions (a
    /// count hook every 1,000), or Luau safepoints (loop back edges and calls, via its interrupt).
    uint64_t work_per_call = 10'000'000;
};

/// A call a script made to maya.add_force(x, y, z).
struct ForceCall {
    double x, y, z;
    bool operator==(const ForceCall&) const = default;
};

/// A sandboxed VM with one script loaded at a time. The native API is a `maya` table with
/// add_force(x, y, z) and log(message). Scripts cannot reach files, processes, the environment,
/// bytecode loading, or the garbage collector's controls.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual std::string_view language() const = 0;
    /// Compiles and runs a chunk in a fresh environment, replacing the previous script. Returns
    /// the error ("name:line: message"), or empty on success.
    virtual std::string load(std::string_view name, std::string_view source) = 0;
    /// Calls the script's global `function` with one number. Returns the error, or empty.
    virtual std::string call(std::string_view function, double argument) = 0;
    /// Bytes the VM has allocated now.
    virtual size_t memory_in_use() const = 0;

    std::vector<ForceCall> forces;
    std::vector<std::string> log;
};

/// The host of the language this program was built with. Lua and Luau export the same C API names,
/// so each language is linked into its own executable (lua_host.cpp or luau_host.cpp).
std::unique_ptr<ScriptHost> make_script_host(ScriptLimits limits = {});

} // namespace maya::prototype
