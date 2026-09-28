#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace maya {

/// The machine a measurement ran on, as the platform reports it. Unknown fields are empty or zero.
struct SystemInfo {
    std::string model; // e.g. "Mac14,5"
    std::string cpu; // e.g. "Apple M2 Max"
    uint32_t physical_cores = 0;
    uint32_t logical_cores = 0;
    uint32_t performance_cores = 0;
    uint32_t efficiency_cores = 0;
    uint64_t memory_bytes = 0;
    std::string os; // e.g. "macOS 26.0 (25A354)"
    std::string gpu; // the default Metal device's name
    bool unified_memory = false;
    std::string thermal_state; // nominal, fair, serious, or critical
    bool low_power_mode = false;
};
SystemInfo system_info();

/// This process's memory as the platform reports it.
struct ProcessMemory {
    size_t footprint = 0; // physical footprint: what the OS charges the process, including GPU memory on unified systems
    size_t resident = 0; // resident set size
    size_t peak_resident = 0;
};
std::optional<ProcessMemory> process_memory() noexcept;

} // namespace maya
