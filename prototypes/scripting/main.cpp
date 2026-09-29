#include "script_host.hpp"

#include <chrono>
#include <cstdio>

// maya_lua_prototype / maya_luau_prototype: measure the #1015 scripting host of the language the
// program was built with and print what they found. tests/prototype_scripting_tests.cpp checks the
// behavior; see docs/architecture/physics-scripting-decision.md.
using namespace maya::prototype;

namespace {

double elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

constexpr const char* mover = R"(
local ticks = 0
function fixed_update(dt)
    ticks = ticks + 1
    maya.add_force(0, 9.81 * dt, ticks)
end
)";

constexpr const char* compute = R"(
function fixed_update(n)
    local sum = 0
    for i = 1, n do
        local x = i * 0.5
        sum = sum + x * x - math.floor(x)
    end
    maya.add_force(sum, 0, 0)
end
)";

} // namespace

int main() {
    auto host = make_script_host({.work_per_call = 100'000'000});
    std::printf("%s prototype\n", host->language().data());

    host->load("mover", mover);
    std::printf("  %zu bytes in the VM with a small script loaded\n", host->memory_in_use());
    host->forces.reserve(100'000);
    auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 100'000; ++i)
        host->call("fixed_update", 1.0 / 60.0);
    std::printf("  %.0f ns per fixed_update call that makes one native call (100,000 calls)\n",
                elapsed_ms(started) * 1e6 / 100'000);

    host->load("compute", compute);
    for (const auto iterations : {200'000, 1'000'000}) {
        started = std::chrono::steady_clock::now();
        const auto error = host->call("fixed_update", iterations);
        std::printf("  %.2f ms for a %d-iteration arithmetic loop%s%s\n", elapsed_ms(started), iterations,
                    error.empty() ? "" : ": ", error.c_str());
    }

    std::printf("Error messages:\n");
    std::printf("  %s\n", host->load("broken", "local x = 1\nfunction fixed_update(dt\n").c_str());
    host->load("faulty", "function fixed_update(dt)\n    local missing = nil\n    missing()\nend\n");
    std::printf("  %s\n", host->call("fixed_update", 0.0).c_str());
    const auto typed = host->load("typed", "local function f(v: number): number return v end");
    std::printf("Type annotations: %s%s\n", typed.empty() ? "accepted" : "rejected, ", typed.c_str());
    return 0;
}
