#pragma once

// Pieces the benchmark runner's files share; not part of its interface.

#include "benchmark.hpp"
#include <cstdint>

namespace maya::benchmark::detail {

/// SplitMix64: a small, portable generator, so the same seed selects the same entities everywhere.
constexpr uint64_t mix(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

/// P1: runs every worker configuration and run of the manifest, and records them in `result`.
void run_physics(Result& result, const Manifest& manifest);

} // namespace maya::benchmark::detail
