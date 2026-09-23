#pragma once

#include "maya/core/identity.hpp"
#include <limits>

namespace maya {
inline constexpr uint32_t invalid_entity_slot = std::numeric_limits<uint32_t>::max();

/// Transient identity; resolve through the World on every use. Never serialize.
struct EntityHandle {
    uint64_t world = 0;
    uint32_t slot = invalid_entity_slot;
    uint64_t generation = 0;
    auto operator<=>(const EntityHandle&) const = default;
};

/// Only meaningful in the command buffer that returned it; not a live handle.
struct PendingEntity {
    uint64_t batch = 0;
    uint32_t index = invalid_entity_slot;
    auto operator<=>(const PendingEntity&) const = default;
};
} // namespace maya
