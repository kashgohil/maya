#include "maya/core/system_info.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <vector>

namespace maya {
namespace {

std::string sysctl_text(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    auto buffer = std::vector<char>(size);
    if (sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0) return {};
    return std::string(buffer.data());
}

template<class T> T sysctl_number(const char* name) {
    auto value = T{};
    auto size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : T{};
}

} // namespace

SystemInfo system_info() {
    @autoreleasepool {
        auto info = SystemInfo{};
        info.model = sysctl_text("hw.model");
        info.cpu = sysctl_text("machdep.cpu.brand_string");
        info.physical_cores = sysctl_number<uint32_t>("hw.physicalcpu");
        info.logical_cores = sysctl_number<uint32_t>("hw.logicalcpu");
        info.performance_cores = sysctl_number<uint32_t>("hw.perflevel0.physicalcpu");
        info.efficiency_cores = sysctl_number<uint32_t>("hw.perflevel1.physicalcpu");
        info.memory_bytes = sysctl_number<uint64_t>("hw.memsize");
        auto* process = [NSProcessInfo processInfo];
        const auto version = process.operatingSystemVersion;
        info.os = "macOS " + std::to_string(version.majorVersion) + "." + std::to_string(version.minorVersion) +
                  (version.patchVersion ? "." + std::to_string(version.patchVersion) : std::string{});
        if (const auto build = sysctl_text("kern.osversion"); !build.empty()) info.os += " (" + build + ")";
        switch (process.thermalState) {
        case NSProcessInfoThermalStateNominal: info.thermal_state = "nominal"; break;
        case NSProcessInfoThermalStateFair: info.thermal_state = "fair"; break;
        case NSProcessInfoThermalStateSerious: info.thermal_state = "serious"; break;
        case NSProcessInfoThermalStateCritical: info.thermal_state = "critical"; break;
        }
        info.low_power_mode = process.lowPowerModeEnabled;
        if (id<MTLDevice> device = MTLCreateSystemDefaultDevice()) {
            info.gpu = device.name.UTF8String;
            info.unified_memory = device.hasUnifiedMemory;
        }
        return info;
    }
}

std::optional<ProcessMemory> process_memory() noexcept {
    auto vm = task_vm_info_data_t{};
    auto count = mach_msg_type_number_t{TASK_VM_INFO_COUNT};
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS)
        return std::nullopt;
    return ProcessMemory{static_cast<size_t>(vm.phys_footprint), static_cast<size_t>(vm.resident_size),
                         static_cast<size_t>(vm.resident_size_peak)};
}

} // namespace maya
