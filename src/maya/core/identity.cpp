#include "maya/core/identity.hpp"
#include <atomic>
#include <limits>
#include <random>
#include <stdexcept>

namespace maya::detail {
std::array<uint64_t, 2> generate_persistent_id() {
    // Random values are not a uniqueness proof; World rejects collisions on load/commit.
    thread_local auto generator = [] {
        auto entropy = std::random_device{};
        auto seed = std::seed_seq{entropy(), entropy(), entropy(), entropy(),
            entropy(), entropy(), entropy(), entropy()};
        return std::mt19937_64{seed};
    }();
    auto result = std::array<uint64_t, 2>{};
    do { result = {generator(), generator()}; } while (result[0] == 0 && result[1] == 0);
    return result;
}

uint64_t next_lifetime_token() {
    static auto next = std::atomic<uint64_t>{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("Maya lifetime tokens exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}
} // namespace maya::detail
