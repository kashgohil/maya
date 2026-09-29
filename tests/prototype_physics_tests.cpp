#include "jolt_prototype.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// The Jolt Physics prototype (#1015): what the decision record relies on.
using namespace maya::prototype;
using Catch::Approx;

TEST_CASE("A dropped box comes to rest on the floor and sleeps", "[prototype][physics]") {
    const auto run = run_box_drop({.ticks = 180});
    REQUIRE_FALSE(run.update_errors);
    REQUIRE(run.final_poses.size() == 1);
    CHECK(run.final_poses[0].position[1] == Approx(0.5f).margin(0.01f));
    CHECK(run.awake_at_end == 0);
    // The box starts tilted, lands on an edge, and settles flat: its first contact is with the floor.
    REQUIRE_FALSE(run.contacts.empty());
    CHECK(run.contacts.front().first == 0);
    CHECK(run.contacts.front().second == 2); // the floor's index is boxes + 1
}

TEST_CASE("Collision groups and masks decide which bodies collide", "[prototype][physics]") {
    // The ghost's group is not in the floor's mask, so the pair is filtered in both directions.
    const auto run = run_box_drop({.ticks = 180, .ghost = true});
    REQUIRE(run.final_poses.size() == 2);
    CHECK(run.final_poses[0].position[1] == Approx(0.5f).margin(0.01f));
    CHECK(run.final_poses[1].position[1] < -10.0f);
    CHECK(run.awake_at_end == 1);
    for (const auto& contact : run.contacts)
        CHECK(contact.first == 0); // never the ghost (index 1)
}

TEST_CASE("The same inputs give identical results whatever the thread count", "[prototype][physics]") {
    const auto serial = run_box_drop({.boxes = 300, .worker_threads = 0, .ticks = 240});
    const auto threaded = run_box_drop({.boxes = 300, .worker_threads = 4, .ticks = 240});
    const auto again = run_box_drop({.boxes = 300, .worker_threads = 4, .ticks = 240});
    REQUIRE_FALSE(serial.update_errors);
    CHECK(serial.trace_hash == threaded.trace_hash);
    CHECK(threaded.trace_hash == again.trace_hash);
    // Contact callbacks arrive on worker threads in any order; sorted, they are the same.
    CHECK_FALSE(serial.contacts.empty());
    CHECK(serial.contacts == threaded.contacts);
}

TEST_CASE("A saved physics state replays the following ticks exactly", "[prototype][physics]") {
    for (const auto workers : {0u, 4u}) {
        const auto run = run_box_drop({.boxes = 200, .worker_threads = workers, .ticks = 180, .replay_from = 45});
        CHECK(run.replay_checked);
        CHECK(run.replay_matches);
        CHECK(run.state_bytes > 0);
    }
}

TEST_CASE("Jolt allocates through the installed hooks and releases a world's memory", "[prototype][physics]") {
    run_box_drop({.ticks = 1}); // installs the hooks and registers Jolt's types once
    const auto before = jolt_allocations();
    reset_jolt_peak();
    run_box_drop({.boxes = 100, .ticks = 60});
    const auto after = jolt_allocations();
    CHECK(after.allocations > before.allocations);
    CHECK(after.peak_bytes > before.live_bytes + (16u << 20)); // the temporary allocator alone is 16 MiB
    CHECK(after.live_bytes == before.live_bytes);
}
