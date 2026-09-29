#include "script_host.hpp"
#include <catch2/catch_test_macros.hpp>

// The scripting prototypes (#1015), built once for Lua 5.4 (maya_prototype_lua_tests) and once for
// Luau (maya_prototype_luau_tests). The same expectations apply to both unless a test says otherwise.
using namespace maya::prototype;

namespace {

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

bool is_luau(const ScriptHost& host) {
    return host.language() == "Luau";
}

constexpr const char* mover = R"(
local ticks = 0
function fixed_update(dt)
    ticks = ticks + 1
    maya.add_force(0, 9.81 * dt, ticks)
end
)";

} // namespace

TEST_CASE("A script's fixed update calls a native function", "[prototype][scripting]") {
    auto host = make_script_host();
    REQUIRE(host->load("mover", mover).empty());
    for (int tick = 0; tick < 60; ++tick)
        REQUIRE(host->call("fixed_update", 1.0 / 60.0).empty());
    REQUIRE(host->forces.size() == 60);
    CHECK(host->forces.front() == ForceCall{0.0, 9.81 / 60.0, 1.0});
    CHECK(host->forces.back() == ForceCall{0.0, 9.81 / 60.0, 60.0});
    REQUIRE(host->load("logger", "maya.log('ready')").empty());
    CHECK(host->log == std::vector<std::string>{"ready"});
}

TEST_CASE("Script errors name the script and line and leave the host usable", "[prototype][scripting]") {
    auto host = make_script_host();
    const auto syntax = host->load("broken", "local x = 1\nfunction fixed_update(dt\n");
    CHECK(contains(syntax, "broken:3:"));

    REQUIRE(host->load("faulty", "function fixed_update(dt)\n    local missing = nil\n    missing()\nend\n").empty());
    CHECK(contains(host->call("fixed_update", 0.0), "faulty:3: attempt to call a nil value"));

    REQUIRE(host->load("bad_argument", "function fixed_update(dt) maya.add_force('up') end").empty());
    const auto argument = host->call("fixed_update", 0.0);
    CHECK(contains(argument, "argument #1 to 'add_force' (number expected, got string)"));
    CHECK(host->forces.empty());

    CHECK(host->call("missing_function", 0.0) == "'missing_function' is not a function");
    REQUIRE(host->load("mover", mover).empty());
    CHECK(host->call("fixed_update", 0.0).empty());
    CHECK(host->forces.size() == 1);
}

TEST_CASE("Scripts cannot reach the system, load code, or control the collector", "[prototype][scripting]") {
    auto host = make_script_host();
    for (const auto* name : {"io", "debug", "package", "require", "load", "loadstring", "loadfile", "dofile",
                             "collectgarbage", "print"}) {
        INFO(name);
        CHECK(host->load("probe", std::string("assert(") + name + " == nil)").empty());
    }
    // Luau keeps a clock-and-dates os library; Lua has none.
    CHECK(host->load("probe", "assert(os == nil or (os.execute == nil and os.getenv == nil and os.remove == nil))").empty());
}

TEST_CASE("Shared tables are read-only and each script has its own globals", "[prototype][scripting]") {
    auto host = make_script_host();
    CHECK_FALSE(host->load("vandal", "math.pi = 3").empty());
    CHECK_FALSE(host->load("vandal", "maya.add_force = nil").empty());
    CHECK_FALSE(host->load("vandal", "string.rep = nil").empty());
    CHECK_FALSE(host->load("vandal", "setmetatable(math, {})").empty());
    REQUIRE(host->load("reader", "assert(math.pi > 3.14 and type(maya.add_force) == 'function')").empty());

    REQUIRE(host->load("first", "shared_value = 42").empty());
    CHECK(host->load("second", "assert(shared_value == nil)").empty());
}

TEST_CASE("The work budget stops runaway scripts deterministically", "[prototype][scripting]") {
    auto host = make_script_host({.work_per_call = 100'000});
    REQUIRE(host->load("spinner", "function fixed_update(dt) while true do end end").empty());
    CHECK(contains(host->call("fixed_update", 0.0), "work budget exceeded"));

    // The budget restarts for every call, and it counts work, not time: the same loop either always
    // fits or never does.
    REQUIRE(host->load("counter", R"(
function fixed_update(n)
    local total = 0
    for i = 1, n do total = total + i end
    maya.add_force(total, 0, 0)
end
)").empty());
    for (int i = 0; i < 5; ++i)
        CHECK(host->call("fixed_update", 1000).empty());
    const auto heavy = host->call("fixed_update", 1e7);
    CHECK(contains(heavy, "work budget exceeded"));
    CHECK(host->call("fixed_update", 1e7) == heavy);
    CHECK(host->forces.size() == 5);
}

TEST_CASE("The memory limit stops a script without harming the host", "[prototype][scripting]") {
    auto host = make_script_host({.memory_bytes = 2u << 20});
    REQUIRE(host->load("mover", mover).empty());
    const auto loaded = host->memory_in_use();
    CHECK(loaded < (1u << 20));

    REQUIRE(host->load("hoarder", "function fixed_update(dt) local t = {} for i = 1, 1e8 do t[i] = i end end").empty());
    CHECK(contains(host->call("fixed_update", 0.0), "not enough memory"));
    CHECK(host->memory_in_use() <= (2u << 20));

    REQUIRE(host->load("mover", mover).empty());
    CHECK(host->memory_in_use() == loaded);
    CHECK(host->call("fixed_update", 0.0).empty());
}

TEST_CASE("Only Luau accepts type annotations", "[prototype][scripting]") {
    auto host = make_script_host();
    const auto typed = host->load("typed", "local function scale(v: number, k: number): number return v * k end\n"
                                           "function fixed_update(dt: number) maya.add_force(0, scale(dt, 2), 0) end");
    CHECK(typed.empty() == is_luau(*host));
    if (typed.empty()) {
        CHECK(host->call("fixed_update", 0.5).empty());
        CHECK(host->forces.back() == ForceCall{0.0, 1.0, 0.0});
    }
}
