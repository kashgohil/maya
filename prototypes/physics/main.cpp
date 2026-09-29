#include "jolt_prototype.hpp"

#include <algorithm>
#include <cstdio>
#include <thread>

// maya_physics_prototype: measures the #1015 falling-box scene in Jolt Physics and prints what it
// found. tests/prototype_physics_tests.cpp checks the behavior; see
// docs/architecture/physics-scripting-decision.md.
using namespace maya::prototype;

int main() {
    std::printf("Jolt Physics prototype\n");
    const auto one = run_box_drop({.ticks = 180, .ghost = true, .replay_from = 60});
    std::printf("One box dropped 5 m, 3 s at 60 Hz: rests at y %.4f m, %s; the masked ghost is at y %.2f m.\n",
                one.final_poses[0].position[1], one.awake_at_end == 1 ? "asleep" : "awake",
                one.final_poses[1].position[1]);
    std::printf("  saved state %zu bytes; replay from tick 60 %s\n", one.state_bytes,
                one.replay_matches ? "matches" : "DIFFERS");

    const auto hardware = std::max(2u, std::thread::hardware_concurrency());
    std::printf("1,000 boxes in 10 layers, 5 s at 60 Hz:\n");
    std::printf("  workers  step mean  step max  peak Jolt heap  awake at end  trace hash\n");
    for (const auto workers : {0u, 1u, 3u, 7u, hardware - 1}) {
        reset_jolt_peak();
        const auto run = run_box_drop({.boxes = 1000, .worker_threads = workers, .ticks = 300});
        std::printf("  %7u  %6.3f ms  %5.3f ms  %9.1f MiB  %12u  %016llx%s\n", workers, run.step_ms_mean,
                    run.step_ms_max, double(jolt_allocations().peak_bytes) / (1 << 20), run.awake_at_end,
                    static_cast<unsigned long long>(run.trace_hash), run.update_errors ? "  (update errors)" : "");
    }
    std::printf("Jolt heap after the runs: %zu bytes (the type registry, kept for the process)\n",
                jolt_allocations().live_bytes);
    return 0;
}
